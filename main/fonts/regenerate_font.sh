#!/bin/sh
# Regenerates dm_sans_semibold_26.c from the static DM Sans SemiBold TTF.
#
# The variable DMSans[opsz,wght].ttf cannot be used: lv_font_conv has no way to
# select a weight from a variable font. The static SemiBold face is committed
# in googlefonts/dm-fonts under Sans/fonts/ttf. The commit below is the one the
# committed output was generated from; lv_font_conv is pinned too so the same
# command produces the same file.
set -eu

cd "$(dirname "$0")"

COMMIT="4412393b7d2de9fe7a92064c2dce9b5af5d7fd26"
FONT_URL="https://raw.githubusercontent.com/googlefonts/dm-fonts/${COMMIT}/Sans/fonts/ttf"
FONT_CONV_VERSION="1.5.3"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

curl -sSL -o "$WORK_DIR/DMSans-SemiBold.ttf" "${FONT_URL}/DMSans-SemiBold.ttf"
curl -sSL -o "$WORK_DIR/DMSans-Italic.ttf" "${FONT_URL}/DMSans-Italic.ttf"

generate() {
    font="$1"
    size="$2"
    name="$3"
    npx --yes "lv_font_conv@${FONT_CONV_VERSION}" \
        --font "$WORK_DIR/${font}" \
        --size "$size" \
        --bpp 4 \
        --format lvgl \
        --lv-font-name "$name" \
        --lv-include lvgl.h \
        -r 0x20-0x7E \
        -r 0xA0-0xFF \
        -r 0x2013-0x2014 \
        -r 0x2018-0x2019 \
        -r 0x201C-0x201D \
        -r 0x2026 \
        --no-compress \
        --output "${name}.c"
}

generate DMSans-SemiBold.ttf 26 dm_sans_semibold_26
generate DMSans-Italic.ttf 24 dm_sans_italic_24
generate DMSans-Italic.ttf 18 dm_sans_italic_18
