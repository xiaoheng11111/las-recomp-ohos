#pragma once
#include <stdint.h>
typedef struct OpusDecoder OpusDecoder;
typedef struct OpusEncoder OpusEncoder;
OpusDecoder *opus_decoder_create(int32_t rate, int channels, int *error);
int opus_decode(OpusDecoder *dec, const unsigned char *data, int32_t len, int16_t *pcm, int32_t frame, int decode_fec);
void opus_decoder_destroy(OpusDecoder *dec);
int32_t opus_decoder_get_nb_samples(const OpusDecoder *dec, const unsigned char *data, int32_t len);
OpusEncoder *opus_encoder_create(int32_t rate, int channels, int application, int *error);
void opus_encoder_destroy(OpusEncoder *enc);
int opus_open(int32_t rate, int channels, int *error);
