#include "astc_rt.h"
#include "../settings.h"
#include "bc.h"
#include <astcenc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct astcenc_context *g_ctx_ldr;
static struct astcenc_context *g_ctx_srgb;
static int g_state;      // 0 = not tried, 1 = ready, -1 = failed
static int g_quality = -1;  // effort the contexts were built with
static int g_disabled = -1;

// ---- bounded in-memory cache of encoded results --------------------------------
// The same texture can be re-created (backend pool churn, dynamic flags) and re-uploaded
// several times; without a cache each pass re-encodes and drops frames (visible as a
// 1-2 frame flicker). Keyed by block content + size + kind/srgb + effort.
typedef struct {
    uint64_t key;
    uint8_t *data;
    size_t len;
} AstcEntry;
#define ASTC_CACHE_MAX (64u << 20)
static AstcEntry *g_ent;
static int g_nent, g_capent;
static size_t g_bytes;

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static uint64_t cache_key(const uint8_t *bc, size_t bc_len, uint32_t w, uint32_t h, int kind, bool srgb) {
    uint64_t k = 1469598103934665603ull;
    k = fnv(k, bc, bc_len);
    k = fnv(k, (const uint8_t *)&w, sizeof w);
    k = fnv(k, (const uint8_t *)&h, sizeof h);
    uint32_t m = (uint32_t)kind | (srgb ? 0x100u : 0u) | ((uint32_t)g_quality << 16);
    k = fnv(k, (const uint8_t *)&m, sizeof m);
    return k;
}

static const uint8_t *cache_get(uint64_t key, size_t len) {
    for (int i = 0; i < g_nent; i++)
        if (g_ent[i].key == key && g_ent[i].len == len) return g_ent[i].data;
    return NULL;
}

static void cache_drop_all(void) {
    for (int i = 0; i < g_nent; i++) free(g_ent[i].data);
    g_nent = 0;
    g_bytes = 0;
}

static void cache_put(uint64_t key, const uint8_t *data, size_t len) {
    if (len > (ASTC_CACHE_MAX >> 2)) return;
    if (g_bytes + len > ASTC_CACHE_MAX) cache_drop_all();
    if (g_nent == g_capent) {
        int nc = g_capent ? g_capent * 2 : 64;
        AstcEntry *p = (AstcEntry *)realloc(g_ent, (size_t)nc * sizeof *p);
        if (!p) return;
        g_ent = p;
        g_capent = nc;
    }
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);
    g_ent[g_nent].key = key;
    g_ent[g_nent].data = copy;
    g_ent[g_nent].len = len;
    g_nent++;
    g_bytes += len;
}
// --------------------------------------------------------------------------------

bool astc_rt_enabled(void) {
    if (g_disabled < 0) g_disabled = getenv("LARECOMP_NO_RT_ASTC") ? 1 : 0;
    return g_disabled == 0;
}

static struct astcenc_context *make_ctx(bool srgb) {
    struct astcenc_config cfg;
    float q = (float)g_quality;
    enum astcenc_error e = astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR,
                                               4, 4, 1, q, 0, &cfg);
    if (e != ASTCENC_SUCCESS) return NULL;
    struct astcenc_context *c = NULL;
    e = astcenc_context_alloc(&cfg, 1, &c, NULL);
    return e == ASTCENC_SUCCESS ? c : NULL;
}

bool astc_rt_init(void) {
    int want = g_settings.astc_quality;
    if (want < 0) want = 0;
    if (want > 100) want = 100;
    if (g_state && want == g_quality) return g_state > 0;
    pthread_mutex_lock(&g_mu);
    if (!g_state || want != g_quality) {
        if (g_ctx_ldr) { astcenc_context_free(g_ctx_ldr); g_ctx_ldr = NULL; }
        if (g_ctx_srgb) { astcenc_context_free(g_ctx_srgb); g_ctx_srgb = NULL; }
        cache_drop_all();
        g_quality = want;
        g_ctx_ldr = make_ctx(false);
        g_ctx_srgb = make_ctx(true);
        g_state = (g_ctx_ldr && g_ctx_srgb) ? 1 : -1;
    }
    pthread_mutex_unlock(&g_mu);
    return g_state > 0;
}

int astc_rt_encode_bc(const uint8_t *bc, uint32_t w, uint32_t h, int bc_kind, bool srgb,
                      uint8_t *out, size_t out_cap) {
    if (!bc || !out || !w || !h || bc_kind < 1) return 1;
    size_t bx = (w + 3u) / 4u, by = (h + 3u) / 4u, need = bx * by * 16u;
    if (out_cap < need) return 2;
    if (!astc_rt_init()) return 3;

    size_t bs = (bc_kind == 1 || bc_kind == 4) ? 8u : 16u;
    size_t bc_len = bx * by * bs;
    uint64_t key = cache_key(bc, bc_len, w, h, bc_kind, srgb);

    pthread_mutex_lock(&g_mu);
    const uint8_t *hit = cache_get(key, need);
    if (hit) {
        memcpy(out, hit, need);
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    pthread_mutex_unlock(&g_mu);

    // ★ v17.7 磁盘缓存：同样的内容+参数，第二次运行直接读盘，不再解码/转码。
    //   键里已经包含内容哈希、尺寸、kind/srgb、quality，命中即等价；
    //   读不全（got != need）就当未命中，避免半截文件导致错图。
    const char *dro = getenv("LARECOMP_DATA_ROOT");
    char dp[820];
    dp[0] = 0;
    if (dro && *dro) {
        snprintf(dp, sizeof dp, "%s/cache/astc_%016llx.bin", dro, (unsigned long long)key);
        FILE *df = fopen(dp, "rb");
        if (df) {
            size_t got = fread(out, 1, need, df);
            fclose(df);
            if (got == need) return 0;
        }
    }

    uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba) return 4;
    bc_decode_ldr(bc, w, h, bc_kind, srgb, rgba);

    struct astcenc_image img;
    memset(&img, 0, sizeof img);
    img.dim_x = w;
    img.dim_y = h;
    img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_U8;
    void *slice = rgba;
    img.data = &slice;
    struct astcenc_swizzle sw = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

    struct astcenc_context *ctx = srgb ? g_ctx_srgb : g_ctx_ldr;
    pthread_mutex_lock(&g_mu);
    enum astcenc_error e = astcenc_compress_image(ctx, &img, &sw, out, need, 0);
    if (e == ASTCENC_SUCCESS) {
        astcenc_compress_reset(ctx);
        cache_put(key, out, need);
        if (dp[0]) {   // 落盘（失败无所谓，只是下次还要重算）
            char dir[820];
            snprintf(dir, sizeof dir, "%s/cache", dro);
            mkdir(dir, 0777);
            FILE *df = fopen(dp, "wb");
            if (df) { fwrite(out, 1, need, df); fclose(df); }
        }
    }
    pthread_mutex_unlock(&g_mu);
    free(rgba);
    return e == ASTCENC_SUCCESS ? 0 : 5;
}
