#pragma once

/*
 * Compatibility shim for LVGL 9.2's SDL window driver.
 *
 * Include SDL before defining the alias so the macro cannot rewrite SDL's own
 * declarations on versions that still typedef SDL_PixelFormatEnum. The value
 * passed to SDL_CreateTexture is a 32-bit pixel-format code on the SDL2 API;
 * using uint32_t also works with packages that no longer expose the old enum
 * typedef.
 */
#include <stdint.h>
#include <SDL2/SDL.h>

#define SDL_PixelFormatEnum uint32_t
