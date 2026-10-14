#!/usr/bin/env bash
# Regenerates assets/ui_font_barlow_*.c from the Barlow typeface
# (SIL Open Font License 1.1, see assets/OFL-Barlow.txt) with lv_font_conv.
#
# Fonts come from the npm package @fontsource/barlow (WOFF files; lv_font_conv
# reads TTF/WOFF, not WOFF2). Needs Node.js + npm and network access to npm.
#
#   firmware/components/ui/tools/gen_fonts.sh [workdir]
#
# Character set: ASCII, Latin-1 (umlauts, accents, ß, «», ·), Latin Extended-A
# (Polish, Czech, Turkish, ... names), typographic quotes, dashes, ellipsis,
# bullet, euro, trade mark. Glyphs outside the set are not drawn.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:-$(mktemp -d)}
mkdir -p "$work"
if [[ ! -d "$work/node_modules/lv_font_conv" ]]; then
    (cd "$work" && npm init -y >/dev/null && npm install --no-audit --no-fund lv_font_conv@1.5.3 @fontsource/barlow@5.3.0)
fi
fdir="$work/node_modules/@fontsource/barlow/files"
conv="$work/node_modules/.bin/lv_font_conv"

latin="0x20-0x7E,0xA0-0xFF,0x152-0x153,0x2013-0x2014,0x2018-0x201E,0x2022,0x2026,0x20AC,0x2122"
latin_ext="0x100-0x151,0x154-0x17F"

# name size weight
for spec in "14 500" "18 500" "24 600" "32 600"; do
    set -- $spec
    "$conv" --no-compress --no-prefilter --bpp 4 --size "$1" --format lvgl --lv-include lvgl.h \
        --font "$fdir/barlow-latin-$2-normal.woff" -r "$latin" \
        --font "$fdir/barlow-latin-ext-$2-normal.woff" -r "$latin_ext" \
        --lv-font-name "ui_font_barlow_$1" -o "$here/assets/ui_font_barlow_$1.c"
done
# large digits for clock and update percentage only
"$conv" --no-compress --no-prefilter --bpp 4 --size 56 --format lvgl --lv-include lvgl.h \
    --font "$fdir/barlow-latin-500-normal.woff" -r "0x20,0x25,0x2D,0x2E,0x30-0x3A" \
    --lv-font-name ui_font_barlow_56 -o "$here/assets/ui_font_barlow_56.c"

# very large, light digits for the screensaver clock (nothing playing)
"$conv" --no-compress --no-prefilter --bpp 4 --size 104 --format lvgl --lv-include lvgl.h \
    --font "$fdir/barlow-latin-300-normal.woff" -r "0x20,0x30-0x3A" \
    --lv-font-name ui_font_barlow_104 -o "$here/assets/ui_font_barlow_104.c"

# lv_font_conv writes its own command line (with absolute paths) into the header
sed -i "s|$work/node_modules/@fontsource/barlow/files/||g; s|$here/||g" "$here"/assets/ui_font_barlow_*.c
cp "$work/node_modules/@fontsource/barlow/LICENSE" "$here/assets/OFL-Barlow.txt"
echo "fonts written to $here/assets"
