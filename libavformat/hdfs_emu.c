/*
 * Input cache protocol.
 * Copyright (c) 2011,2014 Michael Niedermayer
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Based on file.c by Fabrice Bellard
 */

/**
 * @TODO
 *      support keeping files
 *      support filling with a background thread
 */

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/file.h"
#include "libavutil/opt.h"
#include "libavutil/tree.h"
#include "avformat.h"
#include <fcntl.h>
#if HAVE_IO_H
#include <io.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <stdlib.h>
#include <curl/curl.h>
#include "os_support.h"
#include "url.h"

typedef struct CacheEntry {
    int64_t logical_pos;
    int64_t physical_pos;
    int size;
} CacheEntry;

typedef struct Context {
    AVClass *class;
    int  fd;
    char base_url[1000];
    CURL *curl;
    struct AVTreeNode *root;
    int64_t logical_pos;
    int64_t cache_pos;
    int64_t cache_hit, cache_miss;
    int64_t file_size;
    int64_t block_size;
} Context;

typedef struct WriteContext {
    char *buffer;
    int64_t current_pos;
    int64_t max_size;
    URLContext *h;
} WriteContext;

static int add_entry(URLContext *h, const unsigned char *buf, int size);

static int cmp(void *key, const void *node)
{
    return (*(int64_t *) key) - ((const CacheEntry *) node)->logical_pos;
}

static size_t write_buffer(void *buffer, size_t size, size_t nmemb, void *userp)
{
    WriteContext *context = userp;
    int64_t data2write = size * nmemb;
    if (data2write + context->current_pos > context->max_size) {
        av_log(context->h, AV_LOG_ERROR,
                "too much data %"PRId64" > %"PRId64"\n",
                data2write + context->current_pos, context->max_size);
    }
    memcpy(context->buffer + context->current_pos, buffer, data2write);
    context->current_pos += data2write;
    return data2write;
}

static int feed_cache(URLContext* h) {
    char url[1000];
    int ret = 0;
    WriteContext context;
    Context *c= h->priv_data;
    int64_t offset = 0;
    int64_t old_pos = 0;

    context.current_pos = 0;
    context.max_size = c->block_size;
    context.buffer = av_malloc(context.max_size);
    context.h = h;

    offset = (c->logical_pos / c->block_size) * c->block_size;
    snprintf(url, sizeof(url), c->base_url, offset); // / c->block_size);
    curl_easy_setopt(c->curl, CURLOPT_URL, url);
    curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, write_buffer);
    curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(c->curl, CURLOPT_FOLLOWLOCATION, 1L);
    ret = curl_easy_perform(c->curl);
    if(ret != CURLE_OK) {
        av_log(h, AV_LOG_ERROR, "curl_easy_perform() failed: %s\n",
                curl_easy_strerror(ret));
        ret = 0;
        goto cleanup;
    }
    old_pos = c->logical_pos;
    c->logical_pos = offset;
    ret = !add_entry(h, context.buffer, context.current_pos);
    c->logical_pos = old_pos;
cleanup:
    av_freep(&context.buffer);
    return ret;
}

static int cache_open(URLContext *h, const char *arg, int flags, AVDictionary **options)
{
    int ret;
    char *buffername;
    Context *c= h->priv_data;
    char url[1024];
    av_log(h, AV_LOG_WARNING, "checking hdfs -- %s\n", arg);

    if (!av_strnstr(arg, "hdfs://", strlen(arg))) {
        return -1;
    }

    av_strstart(arg, "hdfs://", &arg);
    if ((ret = snprintf(url, sizeof(url), "http://%s", arg)) < 0 ||
            (ret = snprintf(c->base_url, sizeof(c->base_url),
                            "http://%s/%%d", arg)) < 0) {
        av_log(h, AV_LOG_ERROR, "URL too long\n");
        return ret;
    }
    c->curl = curl_easy_init();
    if (!c->curl) {
        av_log(h, AV_LOG_ERROR, "Failed to init curl\n");
        return -2;
    }
    curl_easy_setopt(c->curl, CURLOPT_NOBODY, 1);
    curl_easy_setopt(c->curl, CURLOPT_URL, url);
    ret = curl_easy_perform(c->curl);
    if (ret != CURLE_OK) {
        av_log(h, AV_LOG_ERROR, "Failed to get file size: %s\n", curl_easy_strerror(ret));
        curl_easy_cleanup(c->curl);
        return ret;
    }

    do {
        double fileSize;
        curl_easy_getinfo(c->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &fileSize);
        c->file_size = (int64_t) fileSize;
        if (c->file_size < 1) {
            av_log(h, AV_LOG_ERROR, "Failed to get file size: %s\n", curl_easy_strerror(ret));
            curl_easy_cleanup(c->curl);
            return -1;
        }
    } while (0);
    curl_easy_setopt(c->curl, CURLOPT_NOBODY, 0);

    c->fd = av_tempfile("ffcache", &buffername, 0, h);
    if (c->fd < 0){
        av_log(h, AV_LOG_ERROR, "Failed to create tempfile\n");
        return c->fd;
    }

    unlink(buffername);
    av_freep(&buffername);

    c->root = NULL;

    return 0;
}

static int add_entry(URLContext *h, const unsigned char *buf, int size)
{
    Context *c= h->priv_data;
    int64_t pos = -1;
    int ret;
    CacheEntry *entry = NULL, *next[2] = {NULL, NULL};
    CacheEntry *entry_ret;
    struct AVTreeNode *node = NULL;

    //FIXME avoid lseek
    pos = lseek(c->fd, 0, SEEK_END);
    if (pos < 0) {
        ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "seek in cache failed\n");
        goto fail;
    }
    c->cache_pos = pos;

    ret = write(c->fd, buf, size);
    if (ret < 0) {
        ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "write in cache failed\n");
        goto fail;
    }
    c->cache_pos += ret;

    entry = av_tree_find(c->root, &c->logical_pos, cmp, (void**)next);

    if (!entry)
        entry = next[0];

    if (!entry ||
            entry->logical_pos  + entry->size != c->logical_pos ||
            entry->physical_pos + entry->size != pos
       ) {
        entry = av_malloc(sizeof(*entry));
        node = av_tree_node_alloc();
        if (!entry || !node) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        entry->logical_pos = c->logical_pos;
        entry->physical_pos = pos;
        entry->size = ret;

        entry_ret = av_tree_insert(&c->root, entry, cmp, &node);
        if (entry_ret && entry_ret != entry) {
            ret = -1;
            av_log(h, AV_LOG_ERROR, "av_tree_insert failed\n");
            goto fail;
        }
    } else
        entry->size += ret;

    return 0;
fail:
    //we could truncate the file to pos here if pos >=0 but ftruncate isn't available in VS so
    //for simplicty we just leave the file a bit larger
    av_free(entry);
    av_free(node);
    return ret;
}

static int cache_read(URLContext *h, unsigned char *buf, int size)
{
    Context *c= h->priv_data;
    CacheEntry *entry, *next[2] = {NULL, NULL};
    int r;

    entry = av_tree_find(c->root, &c->logical_pos, cmp, (void**)next);

    if (!entry)
        entry = next[0];

    if (entry) {
        int64_t in_block_pos = c->logical_pos - entry->logical_pos;
        av_assert0(entry->logical_pos <= c->logical_pos);
        if (in_block_pos < entry->size) {
            int64_t physical_target = entry->physical_pos + in_block_pos;

            if (c->cache_pos != physical_target) {
                r = lseek(c->fd, physical_target, SEEK_SET);
            } else
                r = c->cache_pos;

            if (r >= 0) {
                c->cache_pos = r;
                r = read(c->fd, buf, FFMIN(size, entry->size - in_block_pos));
            }

            if (r > 0) {
                c->cache_pos += r;
                c->logical_pos += r;
                c->cache_hit ++;
                // av_log(h, AV_LOG_WARNING, "now pos is %"PRId64"\n", c->logical_pos);
                return r;
            }
        }
    }

    // Cache miss
    if (c->logical_pos >= c->file_size) {
        av_log(h, AV_LOG_WARNING, "EOF\n");
        return -1;
    }
    c->cache_miss ++;
    if (!feed_cache(h)) {
        av_log(h, AV_LOG_ERROR, "Failed to feed the cache.\n");
        return -1;
    }
    return cache_read(h, buf, size);
}

static int64_t cache_seek(URLContext *h, int64_t pos, int whence)
{
    Context *c= h->priv_data;
    // av_log(h, AV_LOG_WARNING, "seek to %"PRId64", %d\n", pos, whence);

    if (whence == AVSEEK_SIZE)
        return c->file_size;

    if (whence == SEEK_CUR) {
        whence = SEEK_SET;
        pos += c->logical_pos;
    } else if (whence == SEEK_END) {
        whence = SEEK_SET;
        pos += c->file_size;
    }

    if (whence == SEEK_SET && pos >= 0 && pos < c->file_size) {
        //Seems within filesize, assume it will not fail.
        c->logical_pos = pos;
        // av_log(h, AV_LOG_WARNING, "now pos is %"PRId64"\n", c->logical_pos);
        return pos;
    }
    return AVERROR(EINVAL);
}

static int cache_close(URLContext *h)
{
    Context *c= h->priv_data;

    av_log(h, AV_LOG_INFO, "Statistics, cache hits:%"PRId64" cache misses:%"PRId64"\n",
            c->cache_hit, c->cache_miss);

    close(c->fd);
    av_tree_destroy(c->root);
    curl_easy_cleanup(c->curl);

    return 0;
}

static const AVOption options[] = {
    { "block_size", "HDFS block size", offsetof(Context, block_size), AV_OPT_TYPE_INT64,
        { .i64 = 64 * 1024 * 1024 }, 1024 * 1024, 1024 * 1024 * 512L,
        AV_OPT_FLAG_DECODING_PARAM},
    {NULL},
};

static const AVClass cache_context_class = {
    .class_name = "hdfs",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

URLProtocol ff_hdfs_emu_protocol = {
    .name                = "hdfs",
    .url_open2           = cache_open,
    .url_read            = cache_read,
    .url_seek            = cache_seek,
    .url_close           = cache_close,
    .priv_data_size      = sizeof(Context),
    .priv_data_class     = &cache_context_class,
    // .flags               = URL_PROTOCOL_FLAG_NETWORK,
};
