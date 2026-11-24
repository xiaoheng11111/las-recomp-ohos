// Runtime BC -> ASTC 4x4 transcode for GPUs without BC (HiSilicon Maleoon).
// The game ships BC1/3/4/5/7 textures; Maleoon has no BC in Vulkan but does sample ASTC, so the
// upload path decodes the blocks on the CPU and re-encodes them as ASTC 4x4 with astcenc.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Enabled unless LARECOMP_NO_RT_ASTC is set (falls back to the RGBA8 decode path).
bool astc_rt_enabled(void);

// Lazily creates the encoder contexts (linear + sRGB). Returns false if astcenc is unavailable.
bool astc_rt_init(void);

// bc: kind 1/3/4/5/7 blocks, tightly packed (4x4 blocks, 8 or 16 bytes each), row-major.
// out: ASTC 4x4 blocks, ceil(w/4)*ceil(h/4)*16 bytes. Returns 0 on success.
int astc_rt_encode_bc(const uint8_t *bc, uint32_t w, uint32_t h, int bc_kind, bool srgb,
                      uint8_t *out, size_t out_cap);
