#pragma once
#include "../libavcodec/avcodec.h"
int av_image_get_buffer_size(int pix_fmt, int width, int height, int align);
