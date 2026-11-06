/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ASTC for hosts that cannot sample it (D3D11 under ANGLE, most desktop
 * GPUs).  Phone games ship ASTC; without it a Unity title unpacks every
 * texture on the CPU at each load and an Unreal one refuses to start.  Here
 * an ASTC image is decoded (ARM's astc-encoder, decode-only build), packed
 * into BC3 that the GPU reads compressed (stb_dxt) or kept as RGBA8 where BC3
 * cannot be used, and the result is kept on disk keyed by the image's
 * content, so the next start reads it back instead of converting again.
 */
#include "luna_texcache.h"
#include "third_party/astcenc/astcenc.h"
#include <string.h>                    /* stb_dxt uses memcpy without including it */
#define STB_DXT_IMPLEMENTATION
#include "lib/stb_dxt.h"

#include <algorithm>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <atomic>

namespace {

struct BlockDims { unsigned fmt; int w, h; };
const BlockDims kDims[] = {
    {0x93B0, 4, 4},  {0x93B1, 5, 4},  {0x93B2, 5, 5},  {0x93B3, 6, 5},
    {0x93B4, 6, 6},  {0x93B5, 8, 5},  {0x93B6, 8, 6},  {0x93B7, 8, 8},
    {0x93B8, 10, 5}, {0x93B9, 10, 6}, {0x93BA, 10, 8}, {0x93BB, 10, 10},
    {0x93BC, 12, 10},{0x93BD, 12, 12},
};

std::mutex g_mu;                       /* one decoder at a time per context */
astcenc_context *g_ctx[2][sizeof kDims / sizeof kDims[0]];
std::string g_dir;
bool g_dir_ready;
std::atomic<unsigned long long> g_hits, g_misses, g_bytes_in, g_bytes_out;

uint64_t mix64(uint64_t h, const void *p, size_t n, uint64_t prime)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= prime; }
    return h;
}

/* Two independent 64-bit hashes of the input and its description. */
void content_key(unsigned fmt, int w, int h, int want_bc3, const void *data, size_t size,
                 char out[40])
{
    const uint32_t desc[4] = { fmt, (uint32_t)w, (uint32_t)h, (uint32_t)want_bc3 };
    uint64_t a = mix64(1469598103934665603ull, desc, sizeof desc, 1099511628211ull);
    a = mix64(a, data, size, 1099511628211ull);
    uint64_t b = mix64(0x9E3779B97F4A7C15ull, data, size, 0x100000001B3ull ^ 0xff51afd7ed558ccdull);
    b = mix64(b, desc, sizeof desc, 0x100000001B3ull ^ 0xff51afd7ed558ccdull);
    snprintf(out, 40, "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
}

const std::string &cache_dir()
{
    if (g_dir_ready) return g_dir;
    g_dir_ready = true;
    const char *off = getenv("LUNARIA_TEXCACHE");
    if (off && off[0] == '0') { g_dir.clear(); return g_dir; }
    const char *dir = getenv("LUNARIA_TEXCACHE_DIR");
    std::string d;
    if (dir && *dir) d = dir;
    else {
        const char *root = getenv("LUNARIA_DATA_ROOT");
        d = std::string(root && *root ? root : ".") + "/cache/textures";
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(d), ec);
    if (ec) {
        fprintf(stderr, "[texcache] cannot create %s (%s); converting every start\n",
                d.c_str(), ec.message().c_str());
        d.clear();
    } else {
        fprintf(stderr, "[texcache] ASTC conversions kept in %s\n", d.c_str());
    }
    g_dir = d;
    return g_dir;
}

struct Header { char magic[4]; uint32_t fmt, w, h, mode, size; };

bool cache_read(const std::string &path, unsigned fmt, int w, int h, int *mode,
                void **out, size_t *out_size)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    Header hd;
    bool ok = fread(&hd, sizeof hd, 1, f) == 1 && !memcmp(hd.magic, "LTC1", 4) &&
              hd.fmt == fmt && hd.w == (uint32_t)w && hd.h == (uint32_t)h &&
              (hd.mode == LUNA_TEX_BC3 || hd.mode == LUNA_TEX_RGBA8) && hd.size;
    void *buf = ok ? malloc(hd.size) : nullptr;
    ok = ok && buf && fread(buf, 1, hd.size, f) == hd.size;
    fclose(f);
    if (!ok) { free(buf); return false; }
    *mode = (int)hd.mode; *out = buf; *out_size = hd.size;
    return true;
}

void cache_write(const std::string &path, unsigned fmt, int w, int h, int mode,
                 const void *data, size_t size)
{
    /* Written aside and renamed, so a half-written file is never read back. */
    const std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    Header hd;
    memcpy(hd.magic, "LTC1", 4);
    hd.fmt = fmt; hd.w = (uint32_t)w; hd.h = (uint32_t)h;
    hd.mode = (uint32_t)mode; hd.size = (uint32_t)size;
    bool ok = fwrite(&hd, sizeof hd, 1, f) == 1 && fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
    if (!ok || ec) std::filesystem::remove(std::filesystem::u8path(tmp), ec);
}

void *encode_bc3(const unsigned char *rgba, int w, int h, size_t *out_size)
{
    const int bx = (w + 3) / 4, by = (h + 3) / 4;
    *out_size = (size_t)bx * by * 16;
    unsigned char *out = (unsigned char *)malloc(*out_size);
    if (!out) return nullptr;
    unsigned char block[64];
    for (int y = 0; y < by; ++y)
        for (int x = 0; x < bx; ++x) {
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    /* Edge blocks repeat the last row/column. */
                    const int sx = x * 4 + i < w ? x * 4 + i : w - 1;
                    const int sy = y * 4 + j < h ? y * 4 + j : h - 1;
                    memcpy(block + (j * 4 + i) * 4, rgba + ((size_t)sy * w + sx) * 4, 4);
                }
            stb_compress_dxt_block(out + ((size_t)y * bx + x) * 16, block, 1, STB_DXT_NORMAL);
        }
    return out;
}

} // namespace

extern "C" int luna_astc_format(unsigned fmt, int *bw, int *bh, int *srgb)
{
    const unsigned base = fmt >= 0x93D0 && fmt <= 0x93DD ? fmt - 0x20 : fmt;
    for (const BlockDims &d : kDims)
        if (d.fmt == base) {
            if (bw) *bw = d.w;
            if (bh) *bh = d.h;
            if (srgb) *srgb = base != fmt;
            return 1;
        }
    return 0;
}

extern "C" int luna_astc_transcode(unsigned fmt, int w, int h, const void *data, size_t size,
                                   int want_bc3, int *mode, void **out, size_t *out_size)
{
    int bw = 0, bh = 0, srgb = 0;
    if (!luna_astc_format(fmt, &bw, &bh, &srgb) || w <= 0 || h <= 0 || !data) return -1;
    const size_t need = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    if (size < need) return -1;

    const std::string &dir = cache_dir();
    std::string path;
    if (!dir.empty()) {
        char key[40];
        content_key(fmt, w, h, want_bc3, data, need, key);
        path = dir + "/" + key + ".ltc";
        if (cache_read(path, fmt, w, h, mode, out, out_size)) {
            ++g_hits;
            return 0;
        }
    }

    unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
    if (!rgba) return -1;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const size_t idx = (size_t)(std::find_if(std::begin(kDims), std::end(kDims),
            [&](const BlockDims &d) { return d.w == bw && d.h == bh; }) - std::begin(kDims));
        astcenc_context *&ctx = g_ctx[srgb][idx];
        if (!ctx) {
            astcenc_config cfg;
            if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR,
                                    (unsigned)bw, (unsigned)bh, 1, ASTCENC_PRE_FASTEST,
                                    ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS ||
                astcenc_context_alloc(&cfg, 1, &ctx, nullptr) != ASTCENC_SUCCESS) {
                ctx = nullptr;
                free(rgba);
                return -1;
            }
        }
        void *planes[1] = { rgba };
        astcenc_image img;
        img.dim_x = (unsigned)w; img.dim_y = (unsigned)h; img.dim_z = 1;
        img.data_type = ASTCENC_TYPE_U8;
        img.data = planes;
        const astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
        const astcenc_error e = astcenc_decompress_image(ctx, (const uint8_t *)data, need, &img, &swz, 0);
        astcenc_decompress_reset(ctx);
        if (e != ASTCENC_SUCCESS) {
            static int n;
            if (n++ < 4)
                fprintf(stderr, "[texcache] ASTC %dx%d decode failed: %s\n", w, h,
                        astcenc_get_error_string(e));
            free(rgba);
            return -1;
        }
    }
    if (want_bc3) {
        size_t bc_size = 0;
        void *bc = encode_bc3(rgba, w, h, &bc_size);
        free(rgba);
        if (!bc) return -1;
        *mode = LUNA_TEX_BC3; *out = bc; *out_size = bc_size;
    } else {
        *mode = LUNA_TEX_RGBA8; *out = rgba; *out_size = (size_t)w * h * 4;
    }
    ++g_misses;
    g_bytes_in += need;
    g_bytes_out += *out_size;
    if (!path.empty()) cache_write(path, fmt, w, h, *mode, *out, *out_size);
    return 0;
}

extern "C" void luna_texcache_stats(unsigned long long *hits, unsigned long long *misses)
{
    if (hits) *hits = g_hits;
    if (misses) *misses = g_misses;
}
