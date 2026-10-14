# Changes to cspot / bell

The submodule `third_party/cspot` (and the nested `bell`) is used **unmodified**.
Nothing in this directory is applied to the submodule's working tree, so
`git status` inside it stays clean and updating it is a plain
`git submodule update`.

The few changes that are needed are *source overlays*: at CMake configure time
the affected file is copied to `build/sc_overlay/...` with one or two textual
replacements and the copy is compiled instead of the original
(`source_overlays.cmake`, included from `../cmake/cspot_bell.cmake`). If the
text to be replaced is not found, the configuration fails with an explicit
message - an overlay can not silently stop working after a submodule update.

Why not classic `.patch` files applied with `git apply`/`patch`? They would
either dirty the submodule or need `patch.exe` (not part of a Windows ESP-IDF
installation), and they are sensitive to line-ending conversion on Windows
checkouts. A textual replacement done by CMake (on LF-normalised content) has
neither problem.

## Overlays

| # | File | Kind | Change | Why |
|---|------|------|--------|-----|
| 1 | `cspot/bell/main/io/URLParser.cpp` | **build fix** | add `#include <cstring>` | The `BELL_DISABLE_REGEX` code path calls `strstr()` without including the header. GCC 14.2 (ESP-IDF 5.5) fails with `error: 'strstr' was not declared in this scope`; older toolchains (IDF 4.x) pulled the header in indirectly. This is the only compile error of cspot+bell under ESP-IDF 5.5.5. |
| 2 | `cspot/src/MercurySession.cpp` | robustness (not needed to build) | stack of the `mercury_dispatcher` task 4 kB -> 16 kB | After a connection loss this task runs the complete reconnect itself, including a TLS handshake to `apresolve.spotify.com`. 4 kB is very little for an mbedTLS 3.x handshake. The stack lives in PSRAM. **Not verified on hardware**; set `SC_MERCURY_TASK_STACK_KB` in `source_overlays.cmake` back to 4 to get upstream behaviour. |
| 3 | `cspot/src/ApResolve.cpp` | **crash fix** (not needed to build) | check the parsed JSON before using `ap_list[0]`, throw `std::runtime_error` otherwise | Upstream dereferences the answer of `https://apresolve.spotify.com/` unchecked. Anything that is not the expected JSON (captive portal, intercepting proxy, HTTP error page, empty body) is a NULL pointer access -> panic and reboot, at login and on every reconnect. **Reproduced in QEMU** (`LoadProhibited` in `ApResolve::fetchFirstApAddress`, ApResolve.cpp:35); with the overlay the same situation is logged and retried every 5 s with a stable heap. The successful path is unchanged but, like everything that needs a Spotify account, not run yet. |

The same changes are kept as ordinary patch files next to this README,
for reference and for submitting them upstream. They are **not** used by the
build; `git apply --check` confirms that they match the pinned commits:

| Patch file | Applies to | Check (run in `components/spotify_connect`) |
|------------|-----------|-----------------------------------------------|
| `0001-bell-URLParser-include-cstring.patch` | bell `cfbc120` | `git -C third_party/cspot/cspot/bell apply --check ../../../../patches/0001-bell-URLParser-include-cstring.patch` |
| `0002-cspot-MercurySession-dispatcher-stack-16k.patch` | cspot `60cf172` | `git -C third_party/cspot apply --check ../../patches/0002-cspot-MercurySession-dispatcher-stack-16k.patch` |
| `0003-cspot-ApResolve-check-json.patch` | cspot `60cf172` | `git -C third_party/cspot apply --check ../../patches/0003-cspot-ApResolve-check-json.patch` |

If you prefer a patched submodule over the overlays (e.g. to debug inside
cspot), apply the patches with `git apply` instead of `--check` and remove the
`sc_overlay_source(...)` calls from `source_overlays.cmake` - otherwise
CMake stops because the text an overlay looks for is gone.

Overlay 4 (TrackPlayer.cpp, 13.10.2026): the decoder's write loop also stops
when a seek is pending, so no pre-seek audio is played after the flush
(audible blip on every seek, found on hardware).

Overlay 5 (SpircHandler.cpp, 13.10.2026): a "Load" frame (how the Spotify app
skips) also sends `NEXT`, so the PCM buffer is flushed at once instead of the
old track playing on until the new one is fetched (found on hardware).

Overlay 6 (TrackQueue.cpp, 13.10.2026, reworked the same day): Spotify
throttles audio keys after quick skipping - every key is refused (code 00 02)
for 20-30 s. A refusal now blocks all key requests for a global back-off
(2/4/8/16/30 s, reset by the next granted key) and the track is only given up
after 120 s of refusals; the next track's key is requested only once the
current one has been READY for 8 s; only one track is preloaded ahead; a failed
storage-resolve (CDN URL) request is retried after 1/2 s. The error code of a
refused key is logged (MercurySession.cpp). Overlay 9 (other qualities after a
refusal) was removed again: refusals were never track-specific and the extra
requests prolonged the throttling.

Overlay 7 (TrackPlayer.cpp, 13.10.2026): the player waits up to 150 s for a
track that is still loading instead of skipping it after 5 s; only a track
that really FAILED is skipped (the queue head through our session task and
`nextSong()`, so Spotify and the app follow the skip). The 5 s limit made the device skip tracks on
its own with a weak WiFi signal (found on hardware). Overlay 4 is applied in
the same call (one overlay call per file).

Overlay 8 (bell TLSSocket.cpp, 13.10.2026): 15 s receive/send timeouts on TLS
sockets, so a dead connection fails instead of blocking cspot's queue task for
minutes.

Overlay 10 (SpircHandler.cpp, TrackQueue.cpp, 13.10.2026, diagnostics only):
logs every frame from the app with its track index, the outcome of each
"track reached playback" notification and the decision of a "previous".

Overlay 11 (SpircHandler.cpp, 14.10.2026): `loadTrackFromURI()` (an empty
stub upstream) starts a context from the device itself (NFC tag): it takes
"<context uri>\n<track uri>..." resolved by our wrapper and acts as if the app
had sent a Load frame. See `spotify_connect_play_uri()`.

Overlay 12 (TrackQueue.cpp, 14.10.2026): "previous" on the device restarts
the track (after 3 s, as before); a second "previous" within 6 s goes to the
previous track even if the restarted one has played longer than 3 s.

Overlays 5 to 12 have no patch file; `source_overlays.cmake` holds the exact
text.

The patch files carry the exact diffs and the reasoning in their headers.

## Known upstream weaknesses that are *not* patched

Found while reading the code for the runtime review; none of them could be
triggered here. They are listed so that nobody is surprised on hardware:

* `AccessKeyFetcher::updateAccessKey()` and `QueuedTrack::stepLoadCDNUrl()`
  parse HTTP answers with the same unchecked pattern as `ApResolve` (overlay 3).
* `AccessKeyFetcher` runs inside `TrackQueue::runTask()` without a `try`: a
  network error during the token request ends in `std::terminate()` -> reboot.
* `LoginBlob::decodeBlob()` only logs a failed MAC check and continues with the
  garbage; `loadZeroconf()` slices the result without bounds checks. Our HTTP
  handler rejects obviously malformed requests and the endpoint is closed after
  a successful login, but a device on the LAN can still send crafted input
  while the device waits for its first login.
* TLS connections of bell (`apresolve`, `accounts.spotify.com`, audio CDN) are
  not certificate-verified unless `bell::X509Bundle::init()` is called.

## Things that are *not* patched but handled in our build

* **nanopb code generation** - upstream generates `protobuf/*.pb.[ch]` during
  the build. We compile the checked-in copies in `../generated/protobuf`
  (see `../generated/README.md`).
* **upstream CMakeLists.txt** of cspot and bell are not used at all; see the
  header of `../cmake/cspot_bell.cmake`.
* **bell's ESP mDNS wrapper** (`main/platform/esp/MDNSService.cpp`) is left out
  of the build; `../src/sc_zeroconf.cpp` talks to the ESP-IDF mdns component
  directly.
