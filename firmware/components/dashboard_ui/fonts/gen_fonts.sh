#!/usr/bin/env bash
#
# Regenerates the dashboard's fonts. Needs node on PATH; lv_font_conv itself is
# pulled by npx. Run it only when a size is added or the face changes -- the
# generated .c files are checked in, so a normal build never needs this.
#
# The dashboard is a table of numbers that change every frame (delta, lap
# clock, best/last), and in a proportional face they twitch sideways as digits
# swap width. Every label is therefore rendered in JetBrains Mono, where all
# glyphs share one advance.
set -euo pipefail

cd "$(dirname "$0")"

readonly kFont=JetBrainsMono-Medium.ttf
# LV_SYMBOL_* glyphs are FontAwesome, which the LVGL component carries along
# for its own built-in fonts. Only LV_SYMBOL_GPS (0xF124, the "Nearest (auto)"
# track tile) is used, and only at 32 px.
readonly kAwesome=../../../managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff

if [[ ! -f "$kAwesome" ]]; then
  echo "missing $kAwesome -- run idf.py reconfigure first" >&2
  exit 1
fi

# gen <size> [extra lv_font_conv args...]
gen() {
  local size=$1
  shift
  echo "pacer_font_mono_${size}.c"
  npx --yes lv_font_conv@1.5.3 \
    --format lvgl --bpp 4 --no-compress --no-prefilter --no-kerning \
    --lv-include lvgl.h \
    --size "$size" --font "$kFont" --range '0x20-0x7F' "$@" \
    --output "pacer_font_mono_${size}.c"
}

gen 14
gen 20
gen 24
gen 32 --font "$kAwesome" --range 0xF124
gen 48
