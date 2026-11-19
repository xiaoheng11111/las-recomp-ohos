#pragma once
#include <stdint.h>
typedef uint64_t Uint64; typedef uint32_t Uint32; typedef int64_t Sint64;
typedef int32_t Sint32; typedef uint16_t Uint16; typedef int16_t Sint16; typedef uint8_t Uint8;
#ifdef __cplusplus
extern "C" {
#endif
Uint64 SDL_GetTicksNS(void);
Uint64 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);
void SDL_DelayNS(Uint64 ns);
void SDL_DelayPrecise(Uint64 ns);
int SDL_GetNumLogicalCPUCores(void);
int SDL_GetCPUCacheLineSize(void);
const char *SDL_GetCPUModelName(void);
int SDL_GetSystemRAM(void);
#ifdef __cplusplus
}
#endif
