// hwopus (nn::codec::HardwareOpusDecoder): Opus packets -> interleaved s16 PCM, decoded on the host with FFmpeg.
// Let's Go's music and sound effects are Opus; with the logging stub GetWorkBufferSize returned 0 and the game gave up
// on every decoder (its 6-channel audio out played silence).
//
// IHardwareOpusDecoderManager: 0 Open(params {u32 rate, u32 channels}, u32 work size, transfer memory),
//   1 GetWorkBufferSize(params), 2 OpenForMultiStream, 3 GetWorkBufferSizeForMultiStream (multistream: not seen yet).
// IHardwareOpusDecoder: 0 DecodeInterleavedOld, 4 DecodeInterleavedWithPerfOld, 6 DecodeInterleaved(u8 reset).
//   Input buffer: {u32 size BE, u32 final range BE} + packet. Output: s16 interleaved at the opened rate.
//   Returns u32 bytes consumed, u32 samples per channel (+ u64 time in microseconds for 4 / 6).
#include <opus/opus.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

typedef struct {
    uint32_t rate, channels;
    OpusDecoder *dec;
    pthread_mutex_t lock;
} OpusDec;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool hw_opus_open(OpusDec *d) {
    // 用 libopus（鸿蒙 arm64 预编译静态库）直接解码；游戏硬性要求解码器存在，否则 svcBreak 退出。
    int err = 0;
    d->dec = opus_decoder_create(48000, (int)d->channels, &err);  // Opus 内部总是 48 kHz
    if (err != 0 || !d->dec) { LOG("[hwopus] opus_decoder_create failed err=%d ch=%u\n", err, d->channels); return false; }
    LOG("[hwopus] opus decoder ready: %u Hz, %u ch\n", d->rate, d->channels);
    return true;
}

static int16_t to_s16(float s) {
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    return (int16_t)(s * 32767.0f);
}

// One packet -> out (interleaved s16 at d->rate). Returns samples per channel, or -1.
static int hw_opus_decode(OpusDec *d, const uint8_t *data, uint32_t size, int16_t *out, uint64_t out_cap) {
    if (!d->dec) return -1;
    int max_per_ch = 5760;  // Opus 最大帧 120ms @48k
    if (out_cap < (uint64_t)max_per_ch * d->channels * 2) max_per_ch = (int)(out_cap / (d->channels * 2));
    if (max_per_ch <= 0) return -1;
    // libopus 直接输出 s16 interleaved（48000 Hz）
    int n = opus_decode(d->dec, data, (int)size, out, max_per_ch, 0);
    if (n < 0) return -1;
    // 若目标采样率低于 48k，做简单抽取（Opus 采样率整除 48k）
    uint32_t step = d->rate && d->rate < 48000 ? 48000 / d->rate : 1;
    if (step > 1) {
        int total = 0;
        for (int i = 0; i < n; i += (int)step) {
            for (uint32_t c = 0; c < d->channels; c++)
                out[total * d->channels + c] = out[i * d->channels + c];
            total++;
        }
        return total;
    }
    return n;
}

static void opus_decoder(Service *s, IpcReq *req, IpcResp *resp) {
    OpusDec *d = s->state;
    switch (req->cmd) {
    case 0: case 4: case 6: {  // DecodeInterleaved*(in packet buffer, out PCM buffer)
        IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
        if (!in || !out || in->size < 8) { resp->result = 111 | (6 << 9); return; }  // (module 111 codec: bad input)
        const uint8_t *p = (const uint8_t *)(uintptr_t)in->addr;
        uint32_t size = be32(p);
        if (8 + (uint64_t)size > in->size) { resp->result = 111 | (6 << 9); return; }
        if (req->cmd == 6 && req->data_size >= 1 && IN(uint8_t, 0)) {
            pthread_mutex_lock(&d->lock);
            if (d->dec) opus_decoder_ctl(d->dec, 4028 /*OPUS_RESET_STATE*/);  // reset: a new stream
            pthread_mutex_unlock(&d->lock);
        }
        uint64_t t0 = plat_now_ns();
        pthread_mutex_lock(&d->lock);
        int n = hw_opus_decode(d, p + 8, size, (int16_t *)(uintptr_t)out->addr, out->size);
        pthread_mutex_unlock(&d->lock);
        if (n < 0) {
            static int nlog;
            if (nlog++ < 10) LOG("[hwopus] packet of %u bytes failed to decode\n", size);
            n = 0;
        }
        OUT(uint32_t, 0) = 8 + size;
        OUT(uint32_t, 4) = (uint32_t)n;
        if (req->cmd != 0) { OUT(uint64_t, 8) = (plat_now_ns() - t0) / 1000; resp_size(resp, 16); }
        else resp_size(resp, 8);
        break;
    }
    case 1: break;  // SetContext
    default: LOG("[hwopus] IHardwareOpusDecoder cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void opus_decoder_close(Service *s) {
    OpusDec *d = s->state;
    if (!d) return;
    if (d->dec) opus_decoder_destroy(d->dec);
    pthread_mutex_destroy(&d->lock);
    free(d);
    s->state = NULL;
}

// What a decoder of this many channels needs on the console (any non-zero size works: the work buffer is unused).
static uint32_t work_size(uint32_t channels) { return 0x4000 + channels * 0x8000; }

SERVICE("hwopus", hwopus) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // OpenHardwareOpusDecoder(params, work size, transfer memory)
        OpusDec *d = calloc(1, sizeof *d);
        d->rate = IN(uint32_t, 0);
        d->channels = IN(uint32_t, 4);
        pthread_mutex_init(&d->lock, NULL);
        if (d->channels < 1 || d->channels > 2 || !hw_opus_open(d)) {
            LOG("[hwopus] cannot open a decoder for %u Hz, %u ch\n", d->rate, d->channels);
            free(d);
            resp->result = 111 | (1 << 9);
            return;
        }
        static int nlog;
        if (nlog++ < 8) LOG("[hwopus] decoder opened: %u Hz, %u ch\n", d->rate, d->channels);
        Service *dec = service_new("IHardwareOpusDecoder", opus_decoder, d);
        dec->on_close = opus_decoder_close;
        ipc_return_object(resp, dec);
        break;
    }
    case 1:  // GetWorkBufferSize(params)
        OUT(uint32_t, 0) = work_size(IN(uint32_t, 4));
        resp_size(resp, 4);
        break;
    default: LOG("[hwopus] IHardwareOpusDecoderManager cmd %u (not implemented)\n", req->cmd); resp_size(resp, 0x10);
    }
}
