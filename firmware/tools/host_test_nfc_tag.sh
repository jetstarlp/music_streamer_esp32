#!/usr/bin/env bash
# Host unit tests for components/nfc_tag (no ESP-IDF needed), incl. a fuzz loop.
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I components/nfc_tag/include \
    tests/host/nfc_tag/test_nfc_tag.c components/nfc_tag/nfc_tag.c \
    -o "$out/test_nfc_tag"
"$out/test_nfc_tag"
