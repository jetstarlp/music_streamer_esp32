#!/usr/bin/env bash
# Builds the firmware and writes merged single-file images (flash offset 0x0)
# to firmware/dist/:
#   ms-waveshare-<sha>.bin   Waveshare ESP32-S3-Touch-AMOLED-1.64 (final device)
#   ms-bench-xiao-<sha>.bin  Seeed XIAO ESP32-S3 Plus, only with --xiao
#                            (bench board, no longer built by default since 13.10.2026)
#
# Images that leave the build machine must not contain credentials: the script
# refuses to run when firmware/sdkconfig.secrets exists, and checks the
# generated sdkconfig files for empty credential options.
#
# Requires an ESP-IDF v5.5.x environment (export.sh / ESP-IDF PowerShell).
set -euo pipefail

cd "$(dirname "$0")/.."
if [[ -f sdkconfig.secrets ]]; then
    echo "error: sdkconfig.secrets exists - move it away before building images for others" >&2
    exit 1
fi

sha=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
if [[ -n "$(git status --porcelain --untracked-files=no . 2>/dev/null)" ]]; then
    sha="${sha}-dirty"
fi
mkdir -p dist

build_one() {
    local name=$1 defaults=$2 dir=$3
    echo "=== $name ($defaults)"
    rm -f "$dir/sdkconfig"     # always regenerate from the profile
    idf.py -B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaults" build
    for opt in CONFIG_MS_WIFI_SSID CONFIG_MS_WIFI_PASSWORD CONFIG_MS_SPOTIFY_CLIENT_ID CONFIG_MS_SPOTIFY_CLIENT_SECRET; do
        if ! grep -qx "${opt}=\"\"" "$dir/sdkconfig"; then
            echo "error: $opt is not empty in $dir/sdkconfig" >&2
            exit 1
        fi
    done
    local out="dist/ms-${name}-${sha}.bin"
    (cd "$dir" && python -m esptool --chip esp32s3 merge_bin -o "../$out" @flash_args)
    echo "-> $out ($(stat -c %s "$out" 2>/dev/null || wc -c <"$out") bytes)"
}

build_one waveshare "sdkconfig.defaults" build-waveshare
if [[ "${1:-}" == "--xiao" ]]; then
    build_one bench-xiao "sdkconfig.defaults;sdkconfig.xiao" build-xiao
fi
