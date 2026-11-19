// BC1/BC3/BC4/BC5 (DXT1/DXT5/RGTC1/RGTC2) decoders to RGBA8, for GPUs without BC support
// (HiSilicon Maleoon: no BC in Vulkan; the game's UI/font/button atlases are BC).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// kind: 1 = BC1, 3 = BC3, 4 = BC4, 5 = BC5. blocks: 4x4 blocks, 8 bytes (BC1/BC4) or 16 bytes
// (BC3/BC5), row-major, covering ceil(width/4) x ceil(height/4) blocks.
// rgba_out: width*height*4 bytes, tightly packed rows, 8-bit UNORM (no sRGB->linear conversion:
// the SRGB image format does that in hardware).
void bc_decode_ldr(const uint8_t *blocks, uint32_t width, uint32_t height, int kind, bool srgb,
                   uint8_t *rgba_out);

// One 4x4 block (8 or 16 bytes, by kind) -> 4x4 RGBA (64 bytes, rows of 4 texels).
void bc_decode_block(const uint8_t *block, int kind, uint8_t *rgba_out);
