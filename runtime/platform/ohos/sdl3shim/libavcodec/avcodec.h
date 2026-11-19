#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int64_t pts_t;
typedef struct AVRational { int num, den; } AVRational;
typedef enum AVMediaType { AVMEDIA_TYPE_UNKNOWN = -1, AVMEDIA_TYPE_VIDEO = 0, AVMEDIA_TYPE_AUDIO = 1 } AVMediaType;
typedef enum AVPixelFormat { AV_PIX_FMT_NONE = -1, AV_PIX_FMT_YUV420P = 3, AV_PIX_FMT_RGBA = 28 } AVPixelFormat;
enum { AV_CODEC_ID_NONE = 0, AV_CODEC_ID_OPUS = 66048, AV_CODEC_ID_VP8 = 139, AV_CODEC_ID_VP9 = 167,
       AV_CODEC_ID_VORBIS = 66050, AV_CODEC_ID_H264 = 27 };
enum { AV_SAMPLE_FMT_FLTP = 8, AV_SAMPLE_FMT_S16 = 1 };
typedef struct AVCodec { int id; const char *name; } AVCodec;
typedef struct AVCodecParameters AVCodecParameters;
typedef struct AVIOContext AVIOContext;
typedef struct AVFormatContext AVFormatContext;
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame {
    uint8_t *data[8];
    struct { void *order; int nb_channels; } ch_layout; int linesize[8]; int width, height, sample_rate, nb_samples, format;
    int64_t pts, best_effort_timestamp; uint64_t channel_layout;
    void *opaque; AVRational sample_aspect_ratio, pkt_duration_dummy;
    int key_frame, pict_type, channels, stride_dummy;
} AVFrame;
typedef struct AVPacket { int64_t pts, dts; uint8_t *data; int size, stream_index, flags; } AVPacket;
struct AVCodecContext {
    AVMediaType codec_type; int sample_rate, channels, width, height, format;
    AVRational time_base, sample_aspect_ratio; uint64_t channel_layout;
    AVCodecParameters *codecpar; void *codec, *opaque, *hw_device_ctx;
    int thread_count, thread_type, extradata_size; uint8_t *extradata; int frame_size, codec_id;
    int64_t bit_rate;
    struct { void *order; int nb_channels; } ch_layout;
};
struct AVIOContext { int write_flag; unsigned char *buffer; };
struct AVFormatContext { AVIOContext *pb; unsigned int nb_streams; struct AVStream **streams; int flags; int64_t duration; };
void avcodec_free_context(AVCodecContext **ctx);
int avcodec_send_packet(AVCodecContext *ctx, const AVPacket *pkt);
int avcodec_receive_frame(AVCodecContext *ctx, AVFrame *frame);
int avcodec_parameters_to_context(AVCodecContext *ctx, AVCodecParameters *par);
void avcodec_flush_buffers(AVCodecContext *ctx);
AVCodec *avcodec_find_decoder(int id);
AVCodec *avcodec_find_decoder_by_name(const char *name);
AVCodecContext *avcodec_alloc_context3(const AVCodec *codec);
int avcodec_open2(AVCodecContext *ctx, const AVCodec *codec, void **options);

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

#define AVERROR_EOF (-541478725)
#define AVFMT_FLAG_CUSTOM_IO 0x80
#define AVSEEK_FLAG_BACKWARD 1
#define AVSEEK_SIZE 0x10000
#define AVSEEK_FLAG_SIZE 2
#define AVSEEK_FLAG_ANY 4
#define AVSEEK_FORCE 0x20000
#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define AVERROR(e) (-(e))
#define AV_LOG_ERROR 16
#define FF_THREAD_FRAME 1
#define FF_THREAD_SLICE 2
#define AV_PIX_FMT_NV12 23
typedef struct AVStream { int index; AVCodecParameters *codecpar; AVRational time_base, avg_frame_rate;
                          int64_t start_time, duration; int nb_frames; void *codec; } AVStream;
typedef struct AVCodecParameters {
    AVMediaType codec_type; int codec_id, codec_tag, format, sample_rate, channels, width, height, bits_per_coded_sample;
    uint64_t channel_layout; int extradata_size; uint8_t *extradata; int frame_size, block_align, bit_rate;
    struct { void *order; int nb_channels; } ch_layout;
} AVCodecParameters;
struct AVStream;
