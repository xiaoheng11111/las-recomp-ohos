#pragma once
#include "../libavcodec/avcodec.h"
AVFormatContext *avformat_alloc_context(void);
void avformat_free_context(AVFormatContext *ctx);
void avformat_close_input(AVFormatContext **ctx);
int avformat_find_stream_info(AVFormatContext *ctx, void **options);
int avformat_open_input(AVFormatContext **ctx, const char *url, void *fmt, void **options);
AVIOContext *avio_alloc_context(unsigned char *buffer, int size, int write, void *opaque,
                                void *r, void *w, void *seek);
void avio_context_free(AVIOContext **ctx);
