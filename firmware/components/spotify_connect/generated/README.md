# Generated sources

`protobuf/*.pb.c` / `*.pb.h` are the nanopb output for the five `.proto` files
in `third_party/cspot/cspot/protobuf` (plus their `.options` files), produced
with the nanopb generator bundled in bell (`nanopb-0.4.6-dev`). They are
byte-identical to what cspot's own CMake build generates.

They are checked in because generating them needs `protoc` and an *old* Python
`protobuf` package (3.20.x) together with `setuptools < 81`; the bundled
generator does not run with current versions. That is a fragile thing to ask of
an ESP-IDF Python environment, particularly on Windows.

`protobuf/SOURCES.sha256` records the hashes of the inputs. The firmware build
compares them with the submodule and stops with an error if they differ.

To regenerate (only after updating the cspot submodule):

    python components/spotify_connect/tools/gen_protos.py

See the docstring of that script and `docs/BUILD.md` for the required tools.
Do not edit the files by hand.
