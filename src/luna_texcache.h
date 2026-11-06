/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ASTC on hosts without it: decode, repack, and keep the result on disk.
 * See luna_texcache.cpp.
 */
#ifndef LUNA_TEXCACHE_H
#define LUNA_TEXCACHE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LUNA_TEX_BC3 = 1, LUNA_TEX_RGBA8 = 2 };

/* 1 when `fmt` is an ASTC LDR 2D format (RGBA or SRGB8_ALPHA8); block size and
 * sRGB-ness come back through the pointers (any may be NULL). */
int luna_astc_format(unsigned fmt, int *block_w, int *block_h, int *srgb);

/* Decode one w x h ASTC image.  want_bc3: repack as BC3 (DXT5), else RGBA8.
 * On success returns 0 with *mode (LUNA_TEX_*) and a malloc'd *out the caller
 * frees.  The disk cache (LUNARIA_TEXCACHE=0 disables, LUNARIA_TEXCACHE_DIR
 * moves it) is consulted first and filled after a conversion. */
int luna_astc_transcode(unsigned fmt, int w, int h, const void *data, size_t size,
                        int want_bc3, int *mode, void **out, size_t *out_size);

void luna_texcache_stats(unsigned long long *hits, unsigned long long *misses);

#ifdef __cplusplus
}
#endif
#endif
