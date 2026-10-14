#!/usr/bin/env python3
"""
Prepare an OTA release from the last build of tools/build_dist.sh.

  python tools/make_ota_release.py --base-url <url> [--notes "Text"] [--min-version 1.0.0]

Writes to firmware/dist/ota/:
  ms-waveshare-<version>.bin   the application image (build-waveshare/music_streamer.bin;
                               NOT the merged USB image, OTA only replaces the app)
  manifest.json                {"version", "url", "sha256", "size", "notes"[, "min_version"]}

<version> is firmware/version.txt; the script refuses an image built with a
different version. --base-url is where the .bin will be downloadable, e.g.
  GitHub release:  https://github.com/<user>/<repo>/releases/download/v1.2.0
  GitHub Pages:    https://<user>.github.io/<repo>/ms-waveshare
Upload both files there (docs/NET.md, "Update veröffentlichen").
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import sys

FW = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def app_version(image: bytes) -> str:
    off = 24 + 8                                   # image header + first segment header
    if image[0] != 0xE9 or struct.unpack_from("<I", image, off)[0] != 0xABCD5432:
        raise SystemExit("error: not an ESP-IDF application image")
    return image[off + 16:off + 48].split(b"\0")[0].decode()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-url", required=True, help="https URL of the directory the .bin is served from")
    ap.add_argument("--notes", default="", help="release notes (German, short; shown on the device)")
    ap.add_argument("--min-version", default="", help="oldest version that may update to this one")
    ap.add_argument("--build", default=os.path.join(FW, "build-waveshare"), help="build directory")
    args = ap.parse_args()

    if not args.base_url.startswith("https://"):
        raise SystemExit("error: --base-url must start with https://")
    with open(os.path.join(FW, "version.txt")) as f:
        version = f.read().strip()
    src = os.path.join(args.build, "music_streamer.bin")
    with open(src, "rb") as f:
        image = f.read()
    built = app_version(image)
    if built != version:
        raise SystemExit(f"error: {src} has version {built}, version.txt says {version} - run tools/build_dist.sh")

    out = os.path.join(FW, "dist", "ota")
    os.makedirs(out, exist_ok=True)
    name = f"ms-waveshare-{version}.bin"
    shutil.copyfile(src, os.path.join(out, name))
    manifest = {
        "version": version,
        "url": args.base_url.rstrip("/") + "/" + name,
        "sha256": hashlib.sha256(image).hexdigest(),
        "size": len(image),
        "notes": args.notes,
    }
    if args.min_version:
        manifest["min_version"] = args.min_version
    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"dist/ota/{name}  ({len(image)} bytes)")
    print("dist/ota/manifest.json:")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
