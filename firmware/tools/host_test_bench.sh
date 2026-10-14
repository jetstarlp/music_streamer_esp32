#!/usr/bin/env bash
# Host unit tests for bench_util.c and tone_gen.c (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# settings.h needs esp_err.h; a two-line stand-in is enough for the types used here.
mkdir -p "$out/inc"
printf '#pragma once\ntypedef int esp_err_t;\n' > "$out/inc/esp_err.h"
gcc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I main -I components/audio_out -I components/settings/include -I "$out/inc" \
    tools/host_test_bench.c main/bench_util.c components/audio_out/tone_gen.c components/audio_out/fade.c components/audio_out/pcm_ring.c components/audio_out/pcm_engine.c \
    -lm -o "$out/host_test_bench"
"$out/host_test_bench"
