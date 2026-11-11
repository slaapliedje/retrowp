/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "imgcache.h"

static struct {
    wp_cache_path_fn fmt;
    u32 *counter;
    plat_file *file;
} g_cache;

static wp_status cache_begin(void *user, i32 width_px, i32 height_px,
                             char *path_out, u32 path_cap, u32 *path_len)
{
    char path[128];
    u32 n;

    (void)user; (void)width_px; (void)height_px;
    if (g_cache.fmt == NULL || g_cache.counter == NULL) return WP_ERR;
    memset(path, 0, sizeof(path));
    g_cache.fmt(path, *g_cache.counter);
    n = (u32)strlen(path);
    if (n == 0 || n >= path_cap) return WP_ERR;
    if (plat_file_open(path, PLAT_FILE_WRITE, &g_cache.file) != WP_OK) {
        g_cache.file = NULL;
        return WP_ERR;
    }
    (*g_cache.counter)++;
    memcpy(path_out, path, n);
    *path_len = n;
    return WP_OK;
}

static wp_status cache_write(void *user, const u8 *bytes, u32 len)
{
    (void)user;
    if (g_cache.file == NULL) return WP_ERR;
    return plat_file_write(g_cache.file, bytes, len);
}

static wp_status cache_end(void *user)
{
    (void)user;
    if (g_cache.file != NULL) {
        plat_file_close(g_cache.file);
        g_cache.file = NULL;
    }
    return WP_OK;
}

void wp_image_cache_sink(wp_image_sink *sink, wp_cache_path_fn fmt, u32 *counter)
{
    g_cache.fmt = fmt;
    g_cache.counter = counter;
    sink->begin = cache_begin;
    sink->write = cache_write;
    sink->end = cache_end;
    sink->user = NULL;
}

static wp_status cache_read(void *user, wp_span blob_path, u32 offset, u8 *out, u32 cap, u32 *out_len)
{
    const wp_document *doc = (const wp_document *)user;
    char path[128];
    plat_file *f;
    wp_status st;

    *out_len = 0;
    if (doc == NULL || blob_path.length == 0 || blob_path.length >= sizeof(path)) return WP_ERR;
    memcpy(path, wp_strpool_get(&doc->strings, blob_path), blob_path.length);
    path[blob_path.length] = '\0';
    if (plat_file_open(path, PLAT_FILE_READ, &f) != WP_OK) return WP_ERR;
    st = plat_file_seek(f, (i32)offset, PLAT_SEEK_SET);
    if (st == WP_OK) st = plat_file_read(f, out, cap, out_len);
    plat_file_close(f);
    return st;
}

void wp_image_cache_source(wp_image_source *src, const wp_document *doc)
{
    src->read = cache_read;
    src->user = (void *)doc;
}
