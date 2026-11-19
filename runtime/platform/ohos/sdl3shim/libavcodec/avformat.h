#pragma once
#include "avcodec.h"
#define AVERROR_EOF (-541478725)
#define AVFMT_FLAG_CUSTOM_IO 0x80
#define AVSEEK_FLAG_BACKWARD 1
#define AVSEEK_FORCE 0x20000
#define AV_INPUT_BUFFER_PADDING_SIZE 64
void avformat_free_context(AVFormatContext *ctx);
int av_read_frame(AVFormatContext *ctx, AVPacket *pkt);
int av_seek_frame(AVFormatContext *ctx, int stream, int64_t ts, int flags);
int64_t av_rescale_q(int64_t a, AVRational bq, AVRational cq);
void av_packet_unref(AVPacket *pkt);
AVPacket *av_packet_alloc(void);
void av_packet_free(AVPacket **pkt);
AVFrame *av_frame_alloc(void);
AVFrame *av_frame_clone(const AVFrame *src);
void av_frame_free(AVFrame **frame);
void av_free(void *p);
void av_freep(void *p);
void *av_malloc(size_t size);
int64_t av_gettime_relative(void);
void av_channel_layout_default(void *layout, int nb);
