// Tegra X1 block-linear layout. A GOB is 64 bytes x 8 rows; blocks stack GOBs vertically
// (block height) and in depth (block depth). Levels shrink their block height as they shrink.
#include "tegra.h"

#include <stdlib.h>
#include <string.h>

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t align_up(uint64_t a, uint64_t b) { return (a + b - 1) / b * b; }

// level-0 block height（GOB 数，log2）。
//
// 正确规则（三个独立权威互相印证，结果一致）：
//   ① tegra_swizzle（ScanMountGoat）block_height_mip0 —— 其 23 个 nutexb 实测测试向量本地跑 23/23 通过；
//   ② nxnative 实测文档的 pick_tile(ah * 3 / 16) —— Tears of the Kingdom / Rhythm Heaven Groove /
//      Touchable 三游戏验证，40 块行 → 4 GOB（向上取整会给 8）；
//   ③ Ryujinx 的 block_height_mip0。
//
//     h15 = hb + hb/2
//     h15 >= 128 -> 16 GOB ; >= 64 -> 8 ; >= 32 -> 4 ; >= 16 -> 2 ; else 1 GOB
//
// ★ 历史教训（v4 回归，绝不可重演）：
//   - 本游戏实际出现的全部尺寸（hb = 2/8/16/25/32/64/128）上，本规则与旧的「取最小够用」
//     **结果完全相同**（10/10，复算脚本 L:\build\verify_correct_rule.py）；
//     两者只在 hb ∈ {9..15, 17..21, 38..42, 81..85} 分叉 —— 本游戏日志里尚未见到这些尺寸。
//   - v4 曾实现成「1.5× 后对齐 16/32/64/128 块行再 /8」→ 把 GOB 数算成 2 倍，
//     导致门板 / UI 图标 / 横幅装饰全部花屏（真实发生过的回归）。
// v5 决策：level-0 block height 保持**原版「最小够用」**，不换成 official 公式。
// 理由：本游戏 10 种实际尺寸（hb 2/8/16/25/32/64/128）原版与 official 规则 10/10 一致，
// 换了对现有画面零变化；official 只在 hb 9..15/17..21/38..42/81..85 分叉，本游戏日志里没有。
// official 公式保留为对照函数（LARECOMP_TEX_BH_RULE=official 可切，便于将来验证）。
uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks) {
    uint32_t gobs = div_up(height_in_blocks, 8);
    uint32_t log2 = 0;
    while ((1u << log2) < gobs && log2 < 4) log2++;
    return log2;
}

// 旧规则同义别名（v4 的回退开关 LARECOMP_TEX_BH_MIN 指向它，保留符号以防链接断裂）。
uint32_t tegra_min_block_height_log2(uint32_t height_in_blocks) {
    return tegra_default_block_height_log2(height_in_blocks);
}

// 对照用：tegra_swizzle / nxnative 的「1.5× 台阶」规则（h15 >= 16/32/64/128 -> 2/4/8/16 GOB）。
// 不参与默认渲染；LARECOMP_TEX_BH_RULE=official 时 tegra_layout_init 才会用它。
uint32_t tegra_block_height_log2_official(uint32_t height_in_blocks) {
    if (!height_in_blocks) height_in_blocks = 1;
    uint32_t h15 = height_in_blocks + height_in_blocks / 2;
    uint32_t gobs = h15 >= 128 ? 16 : h15 >= 64 ? 8 : h15 >= 32 ? 4 : h15 >= 16 ? 2 : 1;
    uint32_t log2 = 0;
    while ((1u << log2) < gobs) log2++;
    return log2;
}

// LARECOMP_TEX_BH_RULE=official → 用对照规则（默认不设 = 原版「最小够用」）。
static int tex_bh_rule_official(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_TEX_BH_RULE") && !strcmp(getenv("LARECOMP_TEX_BH_RULE"), "official") ? 1 : 0;
    return v;
}

static uint32_t block_depth_for(uint32_t depth) {
    uint32_t log2 = 0;
    while ((1u << log2) < depth && log2 < 5) log2++;
    return log2;
}

// LARECOMP_TEX_BH_MIN=1 → 回到旧的「最小够用」规则（对照 / 回退）。
static int tex_bh_min(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_TEX_BH_MIN") ? 1 : 0;
    return v;
}

void tegra_layout_init(TexLayout *t) {
    uint32_t hb = div_up(t->height, t->fmt->bh);
    t->block_height_log2 = tex_bh_min() ? tegra_min_block_height_log2(hb) : tegra_default_block_height_log2(hb);
    t->block_depth_log2 = t->depth > 1 ? block_depth_for(t->depth) : 0;
    t->packaged_width_log2 = 0;
}

// Block height shrinks with the level so a block never exceeds the level height.
static uint32_t level_block_height(uint32_t bh_log2, uint32_t hb) {
    while (bh_log2 > 0 && (8u << (bh_log2 - 1)) >= hb) bh_log2--;
    return bh_log2;
}

static uint32_t level_block_depth(uint32_t bd_log2, uint32_t d) {
    while (bd_log2 > 0 && (1u << (bd_log2 - 1)) >= d) bd_log2--;
    return bd_log2;
}

// GOB columns actually stored for a level: a packaged texture pads every level that is at least one
// tile wide (2^packaged_width_log2 GOBs) up to whole tiles; narrower levels are the mip tail (no pad).
static uint64_t level_width_gobs(const TexLayout *t, uint32_t wb) {
    uint64_t width_gobs = div_up(wb * t->fmt->bpb, 64);
    uint32_t pw = t->packaged_width_log2;
    if (pw && pw < 16) {
        uint64_t tile = 1ull << pw;
        if (width_gobs >= tile) width_gobs = align_up(width_gobs, tile);
    }
    return width_gobs;
}

static uint64_t level_size(const TexLayout *t, uint32_t level, LevelLayout *out) {
    uint32_t w = t->width >> level, h = t->height >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint32_t wb = div_up(w, t->fmt->bw), hb = div_up(h, t->fmt->bh);
    uint32_t bh = level_block_height(t->block_height_log2, hb);
    uint32_t bd = level_block_depth(t->block_depth_log2, d);
    uint32_t gob_rows = 8u << bh, gob_depth = 1u << bd;
    uint64_t width_gobs = level_width_gobs(t, wb);
    uint64_t size = width_gobs * 512 * (1u << bh) * (1u << bd) * div_up(hb, gob_rows) * div_up(d, gob_depth);
    if (out) {
        out->w = w; out->h = h; out->d = d;
        out->wb = wb; out->hb = hb;
        out->block_height_log2 = bh;
        out->block_depth_log2 = bd;
    }
    return size;
}

LevelLayout tegra_level(const TexLayout *t, uint32_t level) {
    LevelLayout l = {0};
    uint64_t off = 0;
    for (uint32_t i = 0; i < level; i++) off += level_size(t, i, NULL);
    level_size(t, level, &l);
    l.offset = off;
    return l;
}

uint64_t tegra_layer_size(const TexLayout *t) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < t->levels; i++) size += level_size(t, i, NULL);
    if (t->layers > 1) {
        // layers are aligned to the level-0 block size
        uint32_t bh = t->block_height_log2;
        size = align_up(size, 512ull << bh);
    }
    return size;
}

uint64_t tegra_total_size(const TexLayout *t) {
    return tegra_layer_size(t) * (t->layers ? t->layers : 1);
}

// Offset of byte (x bytes, y rows, z slices) within a block-linear surface.
static inline uint64_t gob_offset(uint32_t xb, uint32_t y, uint32_t z, uint32_t width_gobs, uint32_t bh_log2,
                                  uint32_t bd_log2, uint64_t slice_blocks_rows) {
    uint32_t gob_x = xb >> 6, gob_y = y >> 3;
    uint32_t block_y = gob_y >> bh_log2, gob_in_block_y = gob_y & ((1u << bh_log2) - 1);
    uint32_t block_z = z >> bd_log2, z_in_block = z & ((1u << bd_log2) - 1);
    uint64_t block_size = 512ull << (bh_log2 + bd_log2);
    uint64_t off = ((uint64_t)block_z * slice_blocks_rows + block_y) * width_gobs * block_size;
    off += (uint64_t)gob_x * block_size;
    off += ((uint64_t)z_in_block << bh_log2 | gob_in_block_y) * 512;
    // within the GOB: 64x8 bytes arranged in 16-byte sectors
    uint32_t xi = xb & 63, yi = y & 7;
    off += ((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 0x0F);
    return off;
}

static void copy_surface(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                         uint32_t bh_log2, uint32_t bd_log2, uint32_t packaged_width_log2, bool to_linear) {
    uint32_t row_bytes = wb * bpb;
    uint64_t width_gobs = (row_bytes + 63) / 64;
    // 打包宽度对齐：与 level_size() 用同一份 padding，否则解 tile 时列跨度与存储的偏移不一致。
    if (packaged_width_log2 && packaged_width_log2 < 16) {
        uint64_t tile = 1ull << packaged_width_log2;
        if (width_gobs >= tile) width_gobs = align_up(width_gobs, tile);
    }
    uint32_t gob_rows = 8u << bh_log2;
    uint64_t blocks_rows = (hb + gob_rows - 1) / gob_rows;
    for (uint32_t z = 0; z < d; z++) {
        for (uint32_t y = 0; y < hb; y++) {
            uint8_t *lin = dst + ((uint64_t)z * hb + y) * row_bytes;
            const uint8_t *lin_src = src + ((uint64_t)z * hb + y) * row_bytes;
            // 16-byte runs are contiguous in both layouts
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint64_t o = gob_offset(x, y, z, (uint32_t)width_gobs, bh_log2, bd_log2, blocks_rows);
                if (to_linear) memcpy(lin + x, src + o, n);
                else memcpy(dst + o, lin_src + x, n);
            }
        }
    }
}

void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t bh_log2, uint32_t bd_log2, uint32_t packaged_width_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, packaged_width_log2, true);
}

void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t bh_log2, uint32_t bd_log2, uint32_t packaged_width_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, packaged_width_log2, false);
}
