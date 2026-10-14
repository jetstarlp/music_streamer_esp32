#!/usr/bin/env python3
"""
Console test in Espressif's QEMU: boots the QEMU build (build-qemu, made by
tools/qemu_smoke_test.sh or `idf.py -B build-qemu ...`), then drives the serial
console over a TCP socket and checks the replies:
  help, status, wifi add/list/del (incl. quoted SSID with a space),
  spotify creds (secret never echoed), name, tone (+ stop), factory-reset yes
  (device reboots and the settings are gone).

No WiFi and no I2S exist in QEMU: this checks the console, NVS round trips and
that the tone path runs without crashing, nothing audible.

Usage: python tools/qemu_console_test.py   (ESP-IDF environment exported)
"""
import os
import shutil
import socket
import tempfile
import subprocess
import sys
import time

FW = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD = os.path.join(FW, "build-qemu")
PORT = 5560
SECRET = "0123456789abcdef0123456789abcdef"
CLIENT_ID = "fedcba9876543210fedcba9876543210"


def fail(msg, log):
    print("FAIL:", msg)
    print("---- console output (last 3000 chars) ----")
    print(log[-3000:])
    sys.exit(1)


class Console:
    def __init__(self, sock):
        self.sock = sock
        self.log = ""

    def read_until(self, token, timeout, start=0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                data = self.sock.recv(4096)
                if data:
                    self.log += data.decode("utf-8", "replace")
            except socket.timeout:
                pass
            if self.log.find(token, start) >= 0:
                return True
        return False

    def cmd(self, line, expect=None, timeout=15):
        mark = len(self.log)
        self.sock.sendall((line + "\n").encode())
        ok = self.read_until("streamer>", timeout, start=mark + len(line))
        out = self.log[mark:]
        if not ok:
            fail(f"no prompt after '{line}'", self.log)
        if expect and expect not in out:
            fail(f"'{expect}' missing in reply to '{line}'", self.log)
        return out


def main():
    flash = os.path.join(BUILD, "qemu_flash.bin")
    efuse = os.path.join(BUILD, "qemu_efuse.bin")
    if not (os.path.exists(flash) and os.path.exists(efuse)):
        print("run tools/qemu_smoke_test.sh once first (it builds build-qemu and the flash image)")
        return 2
    # QEMU writes NVS changes into the image file: work on a fresh copy.
    tmpdir = tempfile.mkdtemp(prefix="ms_console_")
    shutil.copy(flash, os.path.join(tmpdir, "flash.bin"))
    flash = os.path.join(tmpdir, "flash.bin")
    qemu = subprocess.Popen(
        ["qemu-system-xtensa", "-M", "esp32s3", "-m", "8M",
         "-drive", f"file={flash},if=mtd,format=raw",
         "-drive", f"file={efuse},if=none,format=raw,id=efuse",
         "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
         "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
         "-nic", "user,restrict=on,model=open_eth",
         "-display", "none", "-monitor", "none",
         "-serial", f"tcp:127.0.0.1:{PORT},server=on,wait=on"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sock = None
        for _ in range(50):
            try:
                sock = socket.create_connection(("127.0.0.1", PORT), timeout=1)
                break
            except OSError:
                time.sleep(0.2)
        if sock is None:
            print("FAIL: could not connect to the QEMU serial port")
            return 1
        sock.settimeout(0.5)
        c = Console(sock)
        if not c.read_until("serial console ready", 120):
            fail("console did not start", c.log)
        time.sleep(1)
        c.cmd("help", "factory-reset")
        c.cmd("status", "board")
        c.cmd("wifi list", "no networks stored")
        c.cmd('wifi add "My Home" geheim123', 'stored "My Home"')
        c.cmd("wifi add Test short", "at least 8")
        out = c.cmd("wifi list", '"My Home"')
        if "geheim123" in out:
            fail("password printed by wifi list", c.log)
        c.cmd('wifi del "My Home"', "removed")
        c.cmd("wifi list", "no networks stored")
        out = c.cmd(f"spotify creds {CLIENT_ID} {SECRET}", "stored")
        out += c.cmd("spotify", "****cdef")
        out += c.cmd("status", CLIENT_ID)
        # The typed command line itself is echoed once; any further occurrence
        # would be the firmware printing the secret.
        if c.log.count(SECRET) > 1:
            fail("client secret printed by the firmware", c.log)
        c.cmd("name Wohnzimmer Streamer", 'set to "Wohnzimmer Streamer"')
        c.cmd("name", "Wohnzimmer Streamer")
        c.cmd("tone 440 -12 1", "440 Hz")
        c.cmd("tone 5", "frequency must be")
        c.cmd("tone noise-test", "-60.0 dBFS")
        c.cmd("tone stop", "tone stopped")
        c.cmd("factory-reset", "factory-reset yes")
        c.sock.sendall(b"factory-reset yes\n")
        if not c.read_until("all settings erased", 20, start=len(c.log) - 200 if len(c.log) > 200 else 0):
            fail("factory reset not confirmed", c.log)
        mark = len(c.log)
        if not c.read_until("serial console ready", 120, start=mark):
            fail("no reboot after factory reset", c.log)
        time.sleep(1)
        c.cmd("name", "build-time default")
        out = c.cmd("spotify")
        if CLIENT_ID in out:
            fail("credentials survived the factory reset", c.log)
        if "Guru Meditation" in c.log or "abort()" in c.log:
            fail("crash in the log", c.log)
        print("PASS: console test (%d bytes of output)" % len(c.log))
        return 0
    finally:
        qemu.kill()
        qemu.wait()
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
