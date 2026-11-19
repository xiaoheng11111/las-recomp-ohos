#pragma once
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
void SDL_Log(const char *fmt, ...);
void SDL_LogError(int cat, const char *fmt, ...);

#ifdef __cplusplus
}
#endif