#pragma once

#include "lvgl.h"

// The dashboard's faces, generated from JetBrains Mono by gen_fonts.sh. Every
// label on screen uses one of these rather than the built-in Montserrat: the
// numbers that matter (delta, lap clock, best/last) are redrawn several times
// a second, and in a proportional face they shuffle sideways whenever a "1"
// becomes an "8". A monospaced face pins each column in place.
//
// All of them cover ASCII 0x20-0x7F. The 32 px face also carries
// LV_SYMBOL_GPS, the only LVGL symbol the UI draws.

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t pacer_font_mono_14;
extern const lv_font_t pacer_font_mono_20;
extern const lv_font_t pacer_font_mono_24;
extern const lv_font_t pacer_font_mono_32;
extern const lv_font_t pacer_font_mono_48;

#ifdef __cplusplus
}
#endif
