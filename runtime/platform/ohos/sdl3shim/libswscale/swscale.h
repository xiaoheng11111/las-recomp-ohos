#pragma once
#include "../libavcodec/avcodec.h"
typedef struct SwsContext SwsContext;
SwsContext *sws_getCachedContext(void *cache, int sw, int sh, int sfmt, int dw, int dh, int dfmt, int flags, void *a, void *b, void *c);
SwsContext *sws_getContext(int sw, int sh, int sfmt, int dw, int dh, int dfmt, int flags, void *a, void *b, void *c);
int sws_scale(SwsContext *ctx, const uint8_t *const srcSlice[], const int srcStride[], int srcSliceY,
              int srcSliceH, uint8_t *const dstSlice[], const int dstStride[]);
void sws_freeContext(SwsContext *ctx);
#define SWS_BILINEAR 2
