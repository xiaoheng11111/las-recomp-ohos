#pragma once
#include "SDL.h"
bool SDL_Vulkan_CreateSurface(SDL_Window *w, void *vkInstance, void *props, void *surface);
const char *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count);
