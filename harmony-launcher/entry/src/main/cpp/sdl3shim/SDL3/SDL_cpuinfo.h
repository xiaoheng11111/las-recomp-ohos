#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int SDL_GetNumLogicalCPUCores(void);
int SDL_GetCPUCacheLineSize(void);
const char *SDL_GetCPUModelName(void);
#ifdef __cplusplus
}
#endif
