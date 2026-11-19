#include "bc.h"
#include <stdlib.h>

/* BC7 解码：bcdec（MIT，单文件）静态嵌入本 TU */
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#include <string.h>

static inline void put_rgb565(uint16_t c, uint8_t *out) {
    uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
    out[3] = 255;
}

static void bc1_block_colors(const uint8_t *block, uint8_t pal[4][4]) {
    uint16_t c0 = (uint16_t)(block[0] | (block[1] << 8));
    uint16_t c1 = (uint16_t)(block[2] | (block[3] << 8));
    put_rgb565(c0, pal[0]);
    put_rgb565(c1, pal[1]);
    if (c0 > c1) {
        for (int i = 0; i < 3; i++) pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i]) / 3);
        for (int i = 0; i < 3; i++) pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i]) / 3);
        pal[2][3] = 255;
        pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        pal[3][3] = 0;
    }
}

static void bc1_indices(uint8_t *io, const uint8_t *idx_bytes, const uint8_t pal[4][4]) {
    uint32_t bits = (uint32_t)idx_bytes[0] | ((uint32_t)idx_bytes[1] << 8) |
                    ((uint32_t)idx_bytes[2] << 16) | ((uint32_t)idx_bytes[3] << 24);
    for (int i = 0; i < 16; i++) {
        int sel = (int)((bits >> (2 * i)) & 3);
        memcpy(io + i * 4, pal[sel], 4);
    }
}

static void bc4_block_alpha(const uint8_t *block, uint8_t out16[16]) {
    uint8_t a0 = block[0], a1 = block[1];
    uint8_t pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int i = 0; i < 6; i++) pal[2 + i] = (uint8_t)(((6 - i) * a0 + (1 + i) * a1) / 7);
    } else {
        for (int i = 0; i < 4; i++) pal[2 + i] = (uint8_t)(((4 - i) * a0 + (1 + i) * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)block[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out16[i] = pal[(bits >> (3 * i)) & 7];
}

void bc_decode_block(const uint8_t *block, int kind, uint8_t *out) {
    if (kind == 7) { bcdec_bc7(block, out, 16); return; }  /* BC7 -> 4x4 RGBA, row pitch 16 */
    if (kind == 1) {
        uint8_t pal[4][4];
        bc1_block_colors(block, pal);
        bc1_indices(out, block + 4, pal);
        return;
    }
    if (kind == 3) {
        uint8_t alpha[16];
        bc4_block_alpha(block, alpha);
        uint8_t pal[4][4];
        bc1_block_colors(block + 8, pal);
        uint8_t colors[64];
        bc1_indices(colors, block + 12, pal);
        for (int i = 0; i < 16; i++) {
            memcpy(out + i * 4, colors + i * 4, 3);
            out[i * 4 + 3] = alpha[i];
        }
        return;
    }
    if (kind == 4) {
        uint8_t r[16];
        bc4_block_alpha(block, r);
        for (int i = 0; i < 16; i++) {
            out[i * 4 + 0] = r[i];
            out[i * 4 + 1] = r[i];
            out[i * 4 + 2] = r[i];
            out[i * 4 + 3] = 255;
        }
        return;
    }
    uint8_t x[16], y[16];
    bc4_block_alpha(block, x);
    bc4_block_alpha(block + 8, y);
    for (int i = 0; i < 16; i++) {
        out[i * 4 + 0] = x[i];
        out[i * 4 + 1] = y[i];
        out[i * 4 + 2] = 0;
        out[i * 4 + 3] = 255;
    }
}

void bc_decode_ldr(const uint8_t *blocks, uint32_t width, uint32_t height, int kind, bool srgb,
                   uint8_t *rgba_out) {
    (void)srgb;
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    size_t stride = kind == 1 || kind == 4 ? 8u : 16u;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t tex[64];
            bc_decode_block(blocks + ((size_t)by * bw + bx) * stride, kind, tex);
            for (uint32_t ty = 0; ty < 4; ty++) {
                uint32_t y = by * 4 + ty;
                if (y >= height) break;
                uint32_t x0 = bx * 4;
                uint32_t n = (x0 + 4 <= width) ? 4 : (width - x0);
                memcpy(rgba_out + ((size_t)y * width + x0) * 4, tex + ty * 16, n * 4);
            }
        }
    }
}
