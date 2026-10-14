#!/usr/bin/env bash
# Smoke test without hardware: boot the firmware in Espressif's QEMU and check
# that Spotify Connect comes up as far as the Zeroconf login endpoint.
#
# What it proves: boot with our partition table, PSRAM heap, NVS, audio_out
# init, the cspot session task (PSRAM stack) starts, mbedTLS DH key generation,
# HTTP endpoint + event delivery to the application; then, with a FAKE login
# blob (tools/zeroconf_fake_login.py): blob decryption, the first step of the
# Spotify login from the PSRAM-stack task, and that a failing login is retried
# or rejected without a crash.
# What it cannot prove: WiFi, I2S/DAC output, a successful login, playback.
#
# Requirements (Linux/macOS/WSL): ESP-IDF environment exported, QEMU installed
# with  python $IDF_PATH/tools/idf_tools.py install qemu-xtensa  (re-run
# export.sh afterwards), curl.
#
# Usage: firmware/tools/qemu_smoke_test.sh        (from anywhere)
#
# By default the emulated machine has no route to the outside world
# (QEMU "restrict=on"): the test is deterministic and nothing is ever sent to
# Spotify. Set QEMU_NET_OPEN=1 to let the firmware out through the host; the
# fake login then reaches whatever answers for Spotify's host names.
set -euo pipefail

FW_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$FW_DIR/build-qemu"
PORT=7864
LOG="$BUILD/qemu_uart.log"

: "${IDF_PATH:?ESP-IDF environment not exported}"
command -v qemu-system-xtensa >/dev/null || { echo "qemu-system-xtensa not on PATH"; exit 2; }

cd "$FW_DIR"
rm -f "$BUILD/sdkconfig"   # always regenerate from the profiles
idf.py -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" \
       -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu" build

# Flash image (bootloader + partition table + otadata + app) and default eFuses
(cd "$BUILD" && python -m esptool --chip=esp32s3 merge_bin --output=qemu_flash.bin \
    --fill-flash-size=16MB @flash_args >/dev/null)
python - "$BUILD/qemu_efuse.bin" <<'PY'
import os, sys
sys.path.insert(0, os.path.join(os.environ['IDF_PATH'], 'tools'))
from idf_py_actions.qemu_ext import QEMU_TARGETS
with open(sys.argv[1], 'wb') as f:
    f.write(QEMU_TARGETS['esp32s3'].default_efuse)
PY

if [ "${QEMU_NET_OPEN:-0}" = "1" ]; then NET_RESTRICT=""; else NET_RESTRICT="restrict=on,"; fi

rm -f "$LOG"
# -m 8M: same PSRAM size as the ESP32-S3R8. (idf.py qemu's default of 32M uses
# up the whole data address space and breaks flash mmap.)
qemu-system-xtensa -M esp32s3 -m 8M \
    -drive "file=$BUILD/qemu_flash.bin,if=mtd,format=raw" \
    -drive "file=$BUILD/qemu_efuse.bin,if=none,format=raw,id=efuse" \
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
    -nic "user,${NET_RESTRICT}model=open_eth,hostfwd=tcp:127.0.0.1:$PORT-:$PORT" \
    -display none -monitor none -serial "file:$LOG" </dev/null >/dev/null 2>&1 &
QEMU_PID=$!
trap 'kill $QEMU_PID 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1"; echo "--- last UART output ($LOG):"; tail -n 25 "$LOG" 2>/dev/null || true; exit 1; }

# Wait (max 60 s) until the firmware reports that it waits for a login.
for _ in $(seq 60); do
    grep -q "sc_zeroconf: advertising" "$LOG" 2>/dev/null && break
    kill -0 $QEMU_PID 2>/dev/null || fail "QEMU exited"
    sleep 1
done
grep -q "sc_zeroconf: advertising" "$LOG" || fail "Zeroconf endpoint did not come up"
grep -q "Backtrace\|abort()\|Guru Meditation" "$LOG" && fail "firmware crashed"

INFO="$(curl -sS --noproxy '*' -m 10 "http://127.0.0.1:$PORT/spotify_info?action=getInfo")" || fail "getInfo request"
echo "$INFO" | grep -q '"publicKey":"' || fail "getInfo answer has no publicKey: $INFO"
BAD="$(curl -sS --noproxy '*' -m 10 -d 'action=addUser&userName=x&blob=x&clientKey=x' "http://127.0.0.1:$PORT/spotify_info")" || fail "addUser request"
echo "$BAD" | grep -q '"status":202' || fail "malformed addUser was not rejected: $BAD"
sleep 2
grep -q "Backtrace\|abort()" "$LOG" && fail "firmware crashed after the HTTP requests"
grep -q "app: spotify: open the Spotify app" "$LOG" || fail "application did not receive the ZEROCONF_WAITING event"

echo "PASS (1/2): firmware boots in QEMU, Spotify Connect waits for a Zeroconf login"
echo "      getInfo: ${INFO:0:100}..."

# Stage 2: a well-formed login blob with made-up credentials. The device must
# decrypt it and start logging in. Without a route to Spotify (the default
# here) that fails with a network error and is retried every 5 s; with
# QEMU_NET_OPEN=1 and real internet access Spotify declines the credentials.
# Both are fine - a crash or a failed MAC check of the blob is not.
python "$FW_DIR/tools/zeroconf_fake_login.py" --port "$PORT" || fail "fake Zeroconf login was not accepted"
OUTCOME=""
for _ in $(seq 40); do
    sleep 1
    grep -q "Backtrace\|abort()\|Guru Meditation" "$LOG" && fail "firmware crashed during the login attempt"
    if grep -q "spotify: login rejected by Spotify" "$LOG"; then
        OUTCOME="login rejected by Spotify"; break
    fi
    if [ "$(grep -c "spotify: connecting to Spotify failed" "$LOG")" -ge 2 ]; then
        OUTCOME="network error, retried: $(grep -m1 "spotify: connecting to Spotify failed" "$LOG" | sed 's/.*failed: //; s/\x1b.*//')"; break
    fi
done
grep -q "sc_zeroconf: login blob received" "$LOG" || fail "login blob was not processed"
grep -q "Mac doesn't match" "$LOG" && fail "the device could not verify the login blob (DH/HMAC)"
[ -n "$OUTCOME" ] || fail "no login attempt seen within 40 s"
sleep 6
grep -q "Backtrace\|abort()\|Guru Meditation" "$LOG" && fail "firmware crashed after the login attempt"

echo "PASS (2/2): fake login blob decrypted, login attempt handled without a crash ($OUTCOME)"
