#!/usr/bin/env python3
"""
Reflash test in Espressif's QEMU: settings must survive flashing a new
single-file image at 0x0 (the way the device is updated over USB).

1. boot the QEMU build on a fresh flash copy, store a WiFi network,
2. stop QEMU, write a merged image of the same build at offset 0x0 over the
   flash (exactly what esptool-js does with the dist/*.bin files),
3. boot again: the network must still be listed.

Usage: python tools/qemu_reflash_test.py   (after tools/qemu_smoke_test.sh)
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_console_test as qct  # noqa: E402


def boot(flash, efuse):
    qemu = subprocess.Popen(
        ["qemu-system-xtensa", "-M", "esp32s3", "-m", "8M",
         "-drive", f"file={flash},if=mtd,format=raw",
         "-drive", f"file={efuse},if=none,format=raw,id=efuse",
         "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
         "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
         "-nic", "user,restrict=on,model=open_eth",
         "-display", "none", "-monitor", "none",
         "-serial", f"tcp:127.0.0.1:{qct.PORT},server=on,wait=on"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sock = None
    for _ in range(50):
        try:
            sock = qct.socket.create_connection(("127.0.0.1", qct.PORT), timeout=1)
            break
        except OSError:
            qct.time.sleep(0.2)
    if sock is None:
        qemu.kill()
        print("FAIL: could not connect to the QEMU serial port")
        sys.exit(1)
    sock.settimeout(0.5)
    c = qct.Console(sock)
    if not c.read_until("serial console ready", 120):
        qemu.kill()
        qct.fail("console did not start", c.log)
    qct.time.sleep(2)
    return qemu, c


def stop(qemu, c):
    c.sock.close()
    qemu.kill()
    qemu.wait()


def main():
    flash_src = os.path.join(qct.BUILD, "qemu_flash.bin")
    efuse = os.path.join(qct.BUILD, "qemu_efuse.bin")
    if not (os.path.exists(flash_src) and os.path.exists(efuse)):
        print("run tools/qemu_smoke_test.sh once first")
        return 2
    tmpdir = tempfile.mkdtemp(prefix="ms_reflash_")
    flash = os.path.join(tmpdir, "flash.bin")
    shutil.copy(flash_src, flash)
    merged = os.path.join(tmpdir, "merged.bin")
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge_bin", "-o", merged,
                    "@flash_args"], cwd=qct.BUILD, check=True, stdout=subprocess.DEVNULL)

    qemu, c = boot(flash, efuse)
    try:
        c.cmd('wifi add "Keep Me" geheim123', 'stored "Keep Me"')
    finally:
        stop(qemu, c)

    with open(merged, "rb") as f:
        image = f.read()
    with open(flash, "r+b") as f:
        f.seek(0)
        f.write(image)
    print("wrote %d bytes at 0x0 (up to 0x%x)" % (len(image), len(image)))

    qemu, c = boot(flash, efuse)
    try:
        c.cmd("wifi list", '"Keep Me"')
    finally:
        stop(qemu, c)
    shutil.rmtree(tmpdir, ignore_errors=True)
    print("PASS: settings survive flashing a single-file image at 0x0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
