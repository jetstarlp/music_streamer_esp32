#!/usr/bin/env python3
"""Regenerate generated/protobuf/*.pb.[ch] from cspot's .proto files.

The nanopb sources for cspot's protocol messages are checked in (see
generated/README.md) so that a normal firmware build needs neither protoc nor
the Python protobuf package. Run this script only after moving the cspot
submodule to another commit - the firmware build refuses to configure when the
.proto/.options files no longer match generated/protobuf/SOURCES.sha256.

Requirements (use a throw-away virtualenv, NOT the ESP-IDF one):

    python -m venv pbvenv
    pbvenv/bin/pip install "protobuf==3.20.3" "setuptools<81"      (Windows: pbvenv\\Scripts\\pip)
    + a protoc binary 3.20.x / 21.x on PATH (Linux: apt install protobuf-compiler;
      Windows: protoc-21.12-win64.zip from github.com/protocolbuffers/protobuf/releases)

    pbvenv/bin/python tools/gen_protos.py

Why these versions: the nanopb generator bundled with bell (0.4.6-dev) uses
google.protobuf.reflection.MakeClass and pkg_resources. Both are gone from
current releases (protobuf >= 5, setuptools >= 81).
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
COMPONENT = os.path.dirname(HERE)
CSPOT_DIR = os.path.join(COMPONENT, "third_party", "cspot", "cspot")
PROTO_DIR = os.path.join(CSPOT_DIR, "protobuf")
NANOPB_GENERATOR = os.path.join(CSPOT_DIR, "bell", "external", "nanopb", "generator")
OUT_DIR = os.path.join(COMPONENT, "generated")           # files land in generated/protobuf/
MANIFEST = os.path.join(OUT_DIR, "protobuf", "SOURCES.sha256")


def normalized_sha256(path):
    """SHA-256 of a text file with CRLF folded to LF (git autocrlf on Windows)."""
    with open(path, "rb") as f:
        data = f.read().replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def main():
    if not os.path.isdir(PROTO_DIR) or not os.path.isdir(NANOPB_GENERATOR):
        sys.exit("cspot submodule missing: run 'git submodule update --init --recursive'")

    protos = sorted(f for f in os.listdir(PROTO_DIR) if f.endswith(".proto"))
    inputs = sorted(f for f in os.listdir(PROTO_DIR) if f.endswith((".proto", ".options")))

    out_proto_dir = os.path.join(OUT_DIR, "protobuf")
    os.makedirs(out_proto_dir, exist_ok=True)
    for f in os.listdir(out_proto_dir):
        if f.endswith((".pb.c", ".pb.h")):
            os.remove(os.path.join(out_proto_dir, f))

    with tempfile.TemporaryDirectory() as tmp:
        # The generator compiles nanopb.proto into its own directory on first
        # use: work on a copy so the submodule stays clean.
        generator = os.path.join(tmp, "generator")
        shutil.copytree(NANOPB_GENERATOR, generator)
        for proto in protos:
            # Run from the cspot directory with the path "protobuf/<name>.proto":
            # this yields the include path "protobuf/<name>.pb.h" that cspot uses.
            cmd = [sys.executable, os.path.join(generator, "nanopb_generator.py"),
                   "-q", "-I", CSPOT_DIR, "-D", OUT_DIR, "protobuf/" + proto]
            print("generating", proto)
            subprocess.run(cmd, cwd=CSPOT_DIR, check=True)

    # nanopb writes with the platform's line endings; keep the repo at LF.
    for f in os.listdir(out_proto_dir):
        if f.endswith((".pb.c", ".pb.h")):
            p = os.path.join(out_proto_dir, f)
            with open(p, "rb") as fh:
                data = fh.read().replace(b"\r\n", b"\n")
            with open(p, "wb") as fh:
                fh.write(data)

    with open(MANIFEST, "w", newline="\n") as f:
        f.write("# sha256 (CRLF folded to LF) of the inputs the checked-in *.pb.[ch] were generated from.\n")
        f.write("# Written by tools/gen_protos.py, verified by CMakeLists.txt. Do not edit.\n")
        for name in inputs:
            f.write("%s  %s\n" % (normalized_sha256(os.path.join(PROTO_DIR, name)), name))
    print("wrote", os.path.relpath(MANIFEST, COMPONENT))


if __name__ == "__main__":
    main()
