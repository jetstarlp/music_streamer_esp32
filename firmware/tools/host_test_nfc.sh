#!/usr/bin/env bash
# Host unit tests for components/nfc (no ESP-IDF needed): PN532 frame
# encoding/decoding and the Type 2 tag read/write procedures against a
# simulated NTAG21x.
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
CFLAGS=(-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
gcc "${CFLAGS[@]}" -I components/nfc \
    tests/host/nfc/test_pn532_frame.c components/nfc/pn532_frame.c \
    -o "$out/test_pn532_frame"
gcc "${CFLAGS[@]}" -I components/nfc -I components/nfc_tag/include \
    tests/host/nfc/test_t2t.c components/nfc/t2t.c components/nfc_tag/nfc_tag.c \
    -o "$out/test_t2t"
"$out/test_pn532_frame"
"$out/test_t2t"
