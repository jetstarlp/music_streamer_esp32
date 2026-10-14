#!/usr/bin/env bash
# Host unit tests for the pure parts of components/net (no ESP-IDF build
# needed). The manifest tests compile cJSON from $IDF_PATH/components/json;
# without IDF_PATH they are skipped.
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
src=(tools/host_test_net.c components/net/src/pure/net_semver.c components/net/src/pure/net_wifiqr.c
     components/net/src/pure/net_scan.c components/net/src/pure/net_dns.c components/net/src/pure/net_text.c
     components/net/src/pure/net_pages.c)
flags=(-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
       -I components/net/src/pure)
cjson="${IDF_PATH:-}/components/json/cJSON"
if [[ -n "${IDF_PATH:-}" && -f "$cjson/cJSON.c" ]]; then
    gcc "${flags[@]}" -I "$cjson" "${src[@]}" components/net/src/pure/net_manifest.c "$cjson/cJSON.c" \
        -o "$out/host_test_net"
else
    gcc "${flags[@]}" -DNET_TEST_NO_CJSON "${src[@]}" -o "$out/host_test_net"
fi
"$out/host_test_net"
