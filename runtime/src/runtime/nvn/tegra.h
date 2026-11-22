// Tegra X1 texture layout: formats and block-linear (GOB) swizzling.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t bpb;     // bytes per block (per texel for uncompressed)
    uint8_t bw, bh;  // block dimensions in texels
    uint8_t kind;    // FMT_COLOR, FMT_DEPTH, ...
    const char *name;
} FormatInfo;

enum { FMT_COLOR = 0, FMT_DEPTH = 1, FMT_STENCIL = 2, FMT_DEPTH_STENCIL = 3, FMT_COMPRESSED = 4 };

const FormatInfo *nvn_format_info(uint32_t nvn_format);

typedef struct {
    uint32_t width, height, depth, layers, levels;  // depth: 3D depth; layers: array layers / cube faces
    const FormatInfo *fmt;
    uint32_t block_height_log2;  // GOBs per block (log2) for level 0
    uint32_t block_depth_log2;
    // Packaged (BNTX) textures carry a width alignment: NVNpackagedTextureLayout byte 1's low nibble is
    // log2 of a width alignment in GOBs. Levels at least one tile (that many GOBs wide, one block high and
    // deep) are padded to whole tiles; smaller levels form the mip tail (no padding). 0 = no alignment.
    uint32_t packaged_width_log2;
} TexLayout;

typedef struct {
    uint64_t offset;              // from the start of the layer
    uint32_t w, h, d;             // texel dimensions
    uint32_t wb, hb;              // in blocks
    uint32_t block_height_log2, block_depth_log2;
} LevelLayout;

// Level-0 block height of a texture whose block height is not packed (the NVN driver's automatic rule).
//   h15 = height_in_blocks + height_in_blocks/2
//   h15 >= 128 -> 16 GOBs ; >= 64 -> 8 ; >= 32 -> 4 ; >= 16 -> 2 ; else 1
// Three independent authorities agree on this rule (tegra_swizzle block_height_mip0, its 23 measured
// nutexb vectors, and nxnative's pick_tile(ah*3/16) confirmed on three games).
// ★ It equals the old "smallest covering" rule for every size this game has (hb 2/8/16/25/32/64/128:
//   10/10), and diverges only at hb 9..15, 17..21, 38..42, 81..85. Do NOT reintroduce v4's
//   "1.5x then align to 16/32/64/128 block rows then /8" (it doubles the GOB count: the door / UI
//   icon / banner regression).
uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks);
// The old "smallest covering" rule, kept for comparison and for LARECOMP_TEX_BH_MIN=1.
uint32_t tegra_min_block_height_log2(uint32_t height_in_blocks);
// Diagnostic alias of tegra_default_block_height_log2 (kept because the old texture audit printed it).
uint32_t tegra_block_height_log2_official(uint32_t height_in_blocks);
void tegra_layout_init(TexLayout *t);
LevelLayout tegra_level(const TexLayout *t, uint32_t level);
uint64_t tegra_layer_size(const TexLayout *t);
uint64_t tegra_total_size(const TexLayout *t);

// Copy a block-linear level slice into a linear buffer (row pitch = wb * bpb).
// packaged_width_log2 must be the same value used to compute the level offsets/size (0 for plain
// textures): the GOB columns are padded to a whole tile so the stored rows land where the game put them.
void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t block_height_log2, uint32_t block_depth_log2, uint32_t packaged_width_log2);
void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t block_height_log2, uint32_t block_depth_log2, uint32_t packaged_width_log2);
