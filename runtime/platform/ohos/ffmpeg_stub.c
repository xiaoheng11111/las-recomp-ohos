// ffmpeg/opus 第一版 stub：所有函数返回错误，movie/hwopus 优雅降级（跳过动画与语音）。
// 后续接真 ffmpeg-OHOS 移植后删除。
#include <stdint.h>
#include <stddef.h>
void *av_channel_layout_default(void) { return 0; }
void *av_frame_alloc(void) { return 0; }
void *av_frame_clone(void) { return 0; }
void av_frame_free(void *p) { (void)p; }
void av_free(void *p) { (void)p; }
void av_freep(void *p) { (void)p; }
void *av_gettime_relative(void) { return 0; }
void *av_malloc(void) { return 0; }
void *av_packet_alloc(void) { return 0; }
void av_packet_free(void *p) { (void)p; }
long av_packet_unref(void) { return -1; }
long av_read_frame(void) { return -1; }
void *av_rescale_q(void) { return 0; }
long av_seek_frame(void) { return -1; }
void *avcodec_find_decoder(void) { return 0; }
long avcodec_flush_buffers(void) { return -1; }
void *avcodec_free_context(void) { return 0; }
long avcodec_parameters_to_context(void) { return -1; }
long avcodec_receive_frame(void) { return -1; }
long avcodec_send_packet(void) { return -1; }
void *aved(void) { return 0; }
void *avformat_alloc_context(void) { return 0; }
long avformat_close_input(void) { return -1; }
long avformat_find_stream_info(void) { return -1; }
long avformat_open_input(void) { return -1; }
void *avio_alloc_context(void) { return 0; }
long avio_context_free(void) { return -1; }
// opus_* 由真 libopus（platform/ohos/opus-arm64/lib/libopus.a）提供，这里不再给假实现，避免符号重复
// swscale（movie.c 的像素格式转换）
void *sws_getContext(int a, int b, int c, int d, int e, int f, int g, void *h, void *i, void *j) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j; return 0;
}
void *sws_getCachedContext(void *ctx, int a, int b, int c, int d, int e, int f, int g, void *h, void *i, void *j) {
    (void)ctx;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j; return 0;
}
int sws_scale(void *ctx, const uint8_t *const a[], const int b[], int c, int d, uint8_t *const e[], const int f[]) {
    (void)ctx;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return -1;
}
void sws_freeContext(void *ctx) { (void)ctx; }

// 正确签名的两个（_movie.c 直接调用）
void *avcodec_alloc_context3(const void *codec) { (void)codec; return 0; }
long avcodec_open2(void *ctx, const void *codec, void **options) { (void)ctx; (void)codec; (void)options; return -1; }
void avformat_close_input_stub(void) {}
