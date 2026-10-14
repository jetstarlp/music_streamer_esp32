#!/usr/bin/env python3
"""
Copy an ESP-IDF application image with a different version string in its
esp_app_desc_t, fixing the image checksum and the appended SHA-256.

Only for tests (tools/qemu_net_test.py): it produces "newer" firmware images
from one build without rebuilding. Releases are built normally with the new
version in firmware/version.txt.

Usage: ota_set_version.py <in.bin> <out.bin> <version>
"""
import hashlib
import struct
import sys

APP_DESC_MAGIC = 0xABCD5432
HEADER_LEN = 24          # common header (8) + extended header (16)
SEG_HDR_LEN = 8


def set_version(image: bytes, version: str) -> bytes:
    img = bytearray(image)
    if img[0] != 0xE9:
        raise ValueError("not an ESP image")
    if img[23] != 1:
        raise ValueError("image without appended SHA-256 is not supported")
    # esp_app_desc_t is at the start of the first segment's data
    desc = HEADER_LEN + SEG_HDR_LEN
    if struct.unpack_from("<I", img, desc)[0] != APP_DESC_MAGIC:
        raise ValueError("esp_app_desc_t not found")
    ver = version.encode()
    if len(ver) >= 32:
        raise ValueError("version too long")
    off = desc + 16                         # magic, secure_version, reserv1[2], version[32]
    old = bytes(img[off:off + 32])
    new = ver + b"\0" * (32 - len(ver))
    img[off:off + 32] = new
    # checksum: XOR of all segment data bytes, stored in the byte before the digest
    x = 0
    for a, b in zip(old, new):
        x ^= a ^ b
    img[-33] ^= x
    img[-32:] = hashlib.sha256(bytes(img[:-32])).digest()
    return bytes(img)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    with open(sys.argv[2], "wb") as f:
        f.write(set_version(data, sys.argv[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
