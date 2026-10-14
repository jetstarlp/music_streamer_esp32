# Source overlays for cspot/bell.
#
# The submodule is never modified. A file that needs a change is copied to
# ${CMAKE_BINARY_DIR}/sc_overlay/ with a textual replacement applied, and the
# copy is compiled instead of the original (includes still resolve through the
# normal include directories). If the text to replace is no longer present -
# typically after moving the submodule to a newer commit - configuration stops
# with an error, so an overlay can never silently stop having an effect.
#
# Every overlay is listed, with its reason, in patches/README.md. Keep the two
# in sync.

# sc_overlay_source(<list variable holding the sources> <absolute source path>
#                   <search text> <replacement text> [<search> <replacement>]...)
#
# Each search text must occur in the file. Line endings are normalised to LF
# before searching, so multi-line search texts also match in a Windows checkout
# with core.autocrlf=true. Use bracket arguments ([==[ ... ]==]) for texts that
# contain quotes or semicolons.
function(sc_overlay_source sources_var source)
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "spotify_connect overlay: ${source} does not exist")
    endif()
    math(EXPR n_pair_args "${ARGC} - 2")
    math(EXPR odd "${n_pair_args} % 2")
    if(n_pair_args LESS 2 OR odd)
        message(FATAL_ERROR "spotify_connect overlay: ${source}: expected <search> <replacement> pairs")
    endif()

    file(READ "${source}" content)
    string(REPLACE "\r\n" "\n" content "${content}")

    # The pairs are read through ARGV<n> (not ARGN): ARGN is a list and would
    # split texts at every semicolon.
    set(i 2)
    while(i LESS ARGC)
        math(EXPR j "${i} + 1")
        set(search "${ARGV${i}}")
        set(replace "${ARGV${j}}")
        string(FIND "${content}" "${search}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR
                "spotify_connect overlay: text not found in ${source}:\n${search}\n"
                "The cspot/bell submodule probably changed. Check whether the overlay in "
                "patches/source_overlays.cmake is still needed and update or remove it.")
        endif()
        string(REPLACE "${search}" "${replace}" content "${content}")
        math(EXPR i "${i} + 2")
    endwhile()

    file(RELATIVE_PATH rel "${CSPOT_DIR}" "${source}")
    set(overlay "${CMAKE_BINARY_DIR}/sc_overlay/${rel}")
    # Only touch the copy when its content changes, otherwise every CMake run
    # would trigger a recompilation.
    set(old "")
    if(EXISTS "${overlay}")
        file(READ "${overlay}" old)
    endif()
    if(NOT "${old}" STREQUAL "${content}")
        file(WRITE "${overlay}" "${content}")
    endif()
    # re-run CMake when the original changes
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")

    set(sources "${${sources_var}}")
    list(FIND sources "${source}" idx)
    if(idx EQUAL -1)
        message(FATAL_ERROR "spotify_connect overlay: ${source} is not part of ${sources_var}")
    endif()
    list(REMOVE_ITEM sources "${source}")
    list(APPEND sources "${overlay}")
    set(${sources_var} "${sources}" PARENT_SCOPE)
endfunction()

# --- 1. bell: URLParser.cpp needs <cstring> (build fix, GCC 14) ----------------
sc_overlay_source(_bell_sources "${BELL_DIR}/main/io/URLParser.cpp"
    "#include \"URLParser.h\""
    "#include \"URLParser.h\"\n#include <cstring>  // overlay: strstr() in the BELL_DISABLE_REGEX path")

# --- 2. cspot: larger stack for the "mercury_dispatcher" task (robustness) -----
# 4 kB is what upstream uses. After a connection loss this task itself runs
# MercurySession::reconnect(): an HTTPS request to apresolve.spotify.com (TLS
# handshake) plus the Spotify key exchange. The stack is allocated in PSRAM, so
# a generous size costs no internal RAM.
set(SC_MERCURY_TASK_STACK_KB 16)
sc_overlay_source(_cspot_sources "${CSPOT_DIR}/src/MercurySession.cpp"
    "bell::Task(\"mercury_dispatcher\", 4 * 1024, 3, 1)"
    "bell::Task(\"mercury_dispatcher\", ${SC_MERCURY_TASK_STACK_KB} * 1024, 3, 1)  /* overlay: was 4 * 1024 */"
# overlay 6 (diagnostics): log the error code of a refused audio key
[==[      if (this->audioKeyCallbacks.count(seqId) > 0) {
        auto success = static_cast<RequestType>(packet.command) ==
                       RequestType::AUDIO_KEY_SUCCESS_RESPONSE;]==]
[==[      if (static_cast<RequestType>(packet.command) ==
              RequestType::AUDIO_KEY_FAILURE_RESPONSE &&
          packet.data.size() >= 6) {
        CSPOT_LOG(error, "audio key refused by Spotify, code %02x %02x",
                  packet.data[4], packet.data[5]);  // overlay 6
      }
      if (this->audioKeyCallbacks.count(seqId) > 0) {
        auto success = static_cast<RequestType>(packet.command) ==
                       RequestType::AUDIO_KEY_SUCCESS_RESPONSE;]==])

# --- 3. cspot: ApResolve must not dereference a failed JSON parse (robustness) -
# fetchFirstApAddress() takes the answer of https://apresolve.spotify.com/ and
# dereferences json["ap_list"][0] without any check. Anything that is not the
# expected JSON (captive portal, intercepting proxy, HTTP error page, empty
# body) is a NULL pointer access: the firmware panics and reboots, at boot and
# on every reconnect, for as long as the condition lasts. Reproduced in QEMU,
# see docs/SPIKE_REPORT.md. With the overlay it throws instead; the callers
# (our login loop, MercurySession::reconnect) already handle exceptions by
# retrying after 5 s.
sc_overlay_source(_cspot_sources "${CSPOT_DIR}/src/ApResolve.cpp"
[==[#include "HTTPClient.h"  // for HTTPClient, HTTPClient::Response]==]
[==[#include <stdexcept>     // overlay: runtime_error

#include "HTTPClient.h"  // for HTTPClient, HTTPClient::Response]==]
[==[  auto ap_string = std::string(
      cJSON_GetArrayItem(cJSON_GetObjectItem(json, "ap_list"), 0)->valuestring);
  cJSON_Delete(json);
  return ap_string;]==]
[==[  // overlay: was an unchecked ...->valuestring (NULL pointer access)
  cJSON* ap = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "ap_list"), 0);
  if (!cJSON_IsString(ap) || ap->valuestring == nullptr) {
    cJSON_Delete(json);
    throw std::runtime_error("apresolve: unexpected response");
  }
  auto ap_string = std::string(ap->valuestring);
  cJSON_Delete(json);
  return ap_string;]==])

# --- 4. cspot: stop writing pre-seek audio as soon as a seek is pending --------
# TrackPlayer finishes writing the PCM block it decoded before the seek and only
# then executes the seek. Our SEEK handler flushes the output buffer in the
# meantime, so that stale block (and the rest of it) was played right before
# the new position: an audible blip on every seek. With this overlay the write
# loop leaves the stale block as soon as a seek is pending; audio_out_write()
# additionally returns 0 when a flush happened while it waited for space.
# (applied together with overlay 7 below: one overlay call per file)

# --- 5. cspot: drop buffered audio as soon as the app loads another track ------
# Skipping in the Spotify app arrives as a "Load" frame (not as "Next"). cspot
# only resets its player; the PCM buffer (up to ~12 s) kept playing the old
# track until the new one had been fetched, which felt like a slow, laggy skip.
# Sending NEXT makes our event handler flush the buffer right away (NEXT is
# handled as "flush" only, nothing else reacts to it).
# (The A/B switch SC_FLUSH_ON_LOAD is gone: variant A was kept, 13.10.2026.)
# Overlay 10 (same file, diagnostics): log every frame from the app with its
# track index, and what became of each "track reached playback" notification.
sc_overlay_source(_cspot_sources "${CSPOT_DIR}/src/SpircHandler.cpp"
[==[#include <cstdint>      // for uint8_t]==]
[==[#include <cstdint>      // for uint8_t
#include <cstdio>       // overlay 10: snprintf
#include <cstdlib>      // overlay 11: free
#include <cstring>      // overlay 11: strdup]==]
[==[void SpircHandler::loadTrackFromURI(const std::string& uri) {}]==]
[==[// overlay 11: start a context from the device itself (NFC tag), acting as
// if a Load frame had arrived from the app. `spec` is
// "<context uri>\n<track uri>\n<track uri>..." (the caller resolved the
// context). The track GIDs are computed from the base62 ids (16 bytes).
static std::vector<uint8_t> sc_base62_to_gid(const std::string& id) {
  static const char* alphabet =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::vector<uint8_t> gid(16, 0);
  for (char c : id) {
    const char* p = strchr(alphabet, c);
    if (p == nullptr || c == 0) return {};
    unsigned carry = (unsigned)(p - alphabet);
    for (int i = 15; i >= 0; i--) {
      unsigned v = gid[i] * 62u + carry;
      gid[i] = (uint8_t)(v & 0xff);
      carry = v >> 8;
    }
    if (carry) return {};
  }
  return gid;
}

void SpircHandler::loadTrackFromURI(const std::string& spec) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= spec.size()) {
    size_t end = spec.find('\n', start);
    if (end == std::string::npos) end = spec.size();
    if (end > start) lines.push_back(spec.substr(start, end - start));
    start = end + 1;
  }
  if (lines.size() < 2) {
    CSPOT_LOG(error, "loadTrackFromURI: nothing to play");
    return;
  }
  std::vector<TrackReference> tracks;
  for (size_t i = 1; i < lines.size(); i++) {
    const std::string& u = lines[i];
    bool episode = u.rfind("spotify:episode:", 0) == 0;
    if (!episode && u.rfind("spotify:track:", 0) != 0) continue;
    TrackReference ref;
    ref.uri = u;
    ref.gid = sc_base62_to_gid(u.substr(u.find_last_of(':') + 1));
    if (ref.gid.empty()) continue;
    if (episode) ref.type = TrackReference::Type::EPISODE;
    tracks.push_back(std::move(ref));
  }
  if (tracks.empty()) {
    CSPOT_LOG(error, "loadTrackFromURI: no playable track in %s", lines[0].c_str());
    return;
  }
  CSPOT_LOG(info, "local load of %s: %d tracks", lines[0].c_str(), (int)tracks.size());

  this->trackPlayer->start();
  playbackState->remoteTracks = std::move(tracks);
  playbackState->remoteFrame.state.playing_track_index = 0;
  playbackState->remoteFrame.state.has_playing_track_index = true;
  free(playbackState->remoteFrame.state.context_uri);
  playbackState->remoteFrame.state.context_uri = strdup(lines[0].c_str());

  playbackState->setActive(true);
  playbackState->updatePositionMs(0);
  playbackState->setPlaybackState(PlaybackState::State::Playing);
  playbackState->syncWithRemote();
  trackQueue->updateTracks(0, true);
  this->notify();
  trackPlayer->resetState();
  sendEvent(EventType::NEXT);  // flush the old audio (overlay 5 semantics)
}]==]
[==[  playbackState->decodeRemoteFrame(data);
]==]
[==[  playbackState->decodeRemoteFrame(data);
  if (playbackState->remoteFrame.typ != MessageType_kMessageTypeNotify) {
    // overlay 10: who sent it (first chars of the sender's device id) and the
    // tracks around the requested index, to compare lists between frames
    const char* ident = playbackState->remoteFrame.ident ? playbackState->remoteFrame.ident : "?";
    CSPOT_LOG(info, "frame from app %.8s: type %d, track index %d of %d", ident,
              (int)playbackState->remoteFrame.typ,
              (int)playbackState->remoteFrame.state.playing_track_index,
              (int)playbackState->remoteTracks.size());
    int idx = (int)playbackState->remoteFrame.state.playing_track_index;
    for (int i = idx - 2; i <= idx + 2; i++) {
      if (i < 0 || i >= (int)playbackState->remoteTracks.size()) continue;
      const auto& t = playbackState->remoteTracks[i];
      char hex[17] = {0};
      for (size_t k = 0; k < 8 && k < t.gid.size(); k++) {
        snprintf(hex + 2 * k, 3, "%02x", t.gid[k]);
      }
      CSPOT_LOG(info, "  list[%d]%s gid %s%s%s", i, i == idx ? "*" : " ", hex,
                t.queued.value_or(false) ? " (queued)" : "",
                t.uri.empty() ? "" : (" " + t.uri).c_str());
    }
  }
]==]
[==[    case TrackQueue::Reached::IGNORED:
      return;]==]
[==[    case TrackQueue::Reached::IGNORED:
      CSPOT_LOG(info, "track reached: notification IGNORED, index stays %d",  // overlay 10
                (int)playbackState->innerFrame.state.playing_track_index);
      return;]==]
[==[  this->notify();

  sendEvent(EventType::TRACK_INFO, currentTrack->trackInfo);]==]
[==[  CSPOT_LOG(info, "track reached: index now %d",  // overlay 10
            (int)playbackState->innerFrame.state.playing_track_index);
  this->notify();

  sendEvent(EventType::TRACK_INFO, currentTrack->trackInfo);]==]
[==[      this->notify();

      // Stop the current track, if any
      trackPlayer->resetState();
      break;]==]
[==[      this->notify();

      // Stop the current track, if any
      trackPlayer->resetState();
      sendEvent(EventType::NEXT);  // overlay 5: flush the audio of the old track now
      break;]==])

# --- 6. cspot: ride out Spotify's audio key throttling ------------------------
# Bench logs (13.10.2026): after quick skipping Spotify refuses EVERY audio key
# (packet 0x0e, code 00 02) for 20-30 s - all tracks, all qualities. cspot
# marked each track FAILED, the player skipped on and on through the playlist.
# Now:
#  - a refusal blocks all key requests for a global back-off (2, 4, 8, 16,
#    30 s, reset by the next successful key); the track stays KEY_REQUIRED and
#    is only given up after 120 s of refusals;
#  - the next track's key is requested only after the current one has been
#    READY for 8 s (rapid skipping costs one key per skip, not two);
#  - only one track is preloaded ahead (was MAX_TRACKS_PRELOAD = 3);
#  - a failed storage-resolve (CDN URL) request is retried after 1/2 s.
# Overlay 9 (other qualities after a refusal) was dropped: the refusals were
# never track-specific and the extra requests prolonged the throttling.
# Bookkeeping lives in file-local maps keyed by the QueuedTrack pointer so that
# no header has to change.
sc_overlay_source(_cspot_sources "${CSPOT_DIR}/src/TrackQueue.cpp"
[==[using namespace cspot;
namespace TrackDataUtils {]==]
[==[using namespace cspot;

// ---- overlay 6: audio key throttling / retry bookkeeping ----
#include <algorithm>
#include <chrono>
#include <map>
namespace {
using ScClock = std::chrono::steady_clock;
std::mutex sc_key_mutex;
ScClock::time_point sc_keys_blocked_until{};    // global back-off after a refusal
int sc_key_backoff_ms = 0;                      // 0 = no refusal since the last success
std::map<const void*, ScClock::time_point> sc_first_refusal;  // per track
std::map<const void*, int> sc_cdn_attempts;     // per track
std::map<const void*, ScClock::time_point> sc_cdn_due;
const void* sc_ready_track = nullptr;           // last track that became READY
ScClock::time_point sc_ready_at{};

// A key was refused: block all key requests for a while. Returns false when
// this track has been refused for too long and should be given up.
bool sc_key_refused(const void* track) {
  std::scoped_lock lock(sc_key_mutex);
  auto now = ScClock::now();
  sc_key_backoff_ms = sc_key_backoff_ms == 0 ? 2000 : std::min(sc_key_backoff_ms * 2, 30000);
  sc_keys_blocked_until = now + std::chrono::milliseconds(sc_key_backoff_ms);
  auto it = sc_first_refusal.find(track);
  if (it == sc_first_refusal.end()) {
    sc_first_refusal[track] = now;
    return true;
  }
  return now - it->second < std::chrono::seconds(120);
}

void sc_key_granted() {
  std::scoped_lock lock(sc_key_mutex);
  sc_key_backoff_ms = 0;
}

int sc_key_backoff() {
  std::scoped_lock lock(sc_key_mutex);
  return sc_key_backoff_ms;
}

// May a key be requested for this track now?
bool sc_key_request_allowed(const void* track, bool isHead, const void* head) {
  std::scoped_lock lock(sc_key_mutex);
  auto now = ScClock::now();
  if (now < sc_keys_blocked_until) {
    return false;
  }
  if (isHead) {
    return true;
  }
  // next track: only once the current one has settled (no rapid skipping)
  return sc_ready_track == head && now - sc_ready_at >= std::chrono::seconds(8);
}

void sc_track_ready(const void* track) {
  std::scoped_lock lock(sc_key_mutex);
  sc_ready_track = track;
  sc_ready_at = ScClock::now();
}

bool sc_cdn_retry_schedule(const void* track) {
  std::scoped_lock lock(sc_key_mutex);
  int& n = sc_cdn_attempts[track];
  if (n >= 2) {
    return false;
  }
  n++;
  sc_cdn_due[track] = ScClock::now() + std::chrono::milliseconds(1000 << (n - 1));
  return true;
}

bool sc_cdn_retry_due(const void* track) {
  std::scoped_lock lock(sc_key_mutex);
  auto it = sc_cdn_due.find(track);
  return it == sc_cdn_due.end() || ScClock::now() >= it->second;
}

void sc_key_retry_forget(const void* track) {
  std::scoped_lock lock(sc_key_mutex);
  sc_first_refusal.erase(track);
  sc_cdn_attempts.erase(track);
  sc_cdn_due.erase(track);
  if (sc_ready_track == track) {
    sc_ready_track = nullptr;
  }
}
}  // namespace
// ---- end overlay 6 ----

namespace TrackDataUtils {]==]
[==[QueuedTrack::~QueuedTrack() {
  state = State::FAILED;]==]
[==[QueuedTrack::~QueuedTrack() {
  sc_key_retry_forget(this);  // overlay 6
  state = State::FAILED;]==]
[==[        if (success) {
          CSPOT_LOG(info, "Got audio key");]==]
[==[        if (success) {
          sc_key_granted();  // overlay 6
          CSPOT_LOG(info, "Got audio key");]==]
[==[        } else {
          CSPOT_LOG(error, "Failed to get audio key");
          state = State::FAILED;
          loadedSemaphore->give();
        }]==]
[==[        } else if (sc_key_refused(this)) {  // overlay 6
          CSPOT_LOG(error, "Audio key refused (Spotify throttling?), waiting %d ms before any new key request",
                    sc_key_backoff());
          state = State::KEY_REQUIRED;
        } else {
          CSPOT_LOG(error, "Failed to get audio key (refused for 120 s)");
          state = State::FAILED;
          loadedSemaphore->give();
        }]==]
[==[    CSPOT_LOG(info, "Received CDN URL, %s", cdnUrl.c_str());
    state = State::READY;
    loadedSemaphore->give();
  } catch (...) {
    CSPOT_LOG(error, "Cannot fetch CDN URL");
    state = State::FAILED;
    loadedSemaphore->give();
  }]==]
[==[    CSPOT_LOG(info, "Received CDN URL, %s", cdnUrl.c_str());
    sc_track_ready(this);  // overlay 6
    state = State::READY;
    loadedSemaphore->give();
  } catch (...) {
    if (sc_cdn_retry_schedule(this)) {  // overlay 6
      CSPOT_LOG(error, "Cannot fetch CDN URL, trying again shortly");
      state = State::CDN_REQUIRED;
      return;
    }
    CSPOT_LOG(error, "Cannot fetch CDN URL");
    state = State::FAILED;
    loadedSemaphore->give();
  }]==]
[==[    case QueuedTrack::State::KEY_REQUIRED:
      track->stepLoadAudioFile(tracksMutex, processSemaphore);
      break;]==]
[==[    case QueuedTrack::State::KEY_REQUIRED: {
      // overlay 6: global back-off after refusals; next track waits until the
      // current one has settled
      const void* head = nullptr;
      {
        std::scoped_lock lock(tracksMutex);
        if (!preloadedTracks.empty()) head = preloadedTracks[0].get();
      }
      if (sc_key_request_allowed(track.get(), track.get() == head, head)) {
        track->stepLoadAudioFile(tracksMutex, processSemaphore);
      }
      break;
    }]==]
[==[    case QueuedTrack::State::CDN_REQUIRED:
      track->stepLoadCDNUrl(accessKey);
]==]
[==[    case QueuedTrack::State::CDN_REQUIRED:
      if (!sc_cdn_retry_due(track.get())) {  // overlay 6: retry delay
        break;
      }
      track->stepLoadCDNUrl(accessKey);
]==]
[==[    CSPOT_LOG(info, "Track name: %s", pbTrack->name);]==]
[==[    CSPOT_LOG(info, "Track name: %s (gid %02x%02x%02x%02x%02x%02x%02x%02x)",  // overlay 10
              pbTrack->name, ref.gid.size() > 7 ? ref.gid[0] : 0, ref.gid.size() > 7 ? ref.gid[1] : 0,
              ref.gid.size() > 7 ? ref.gid[2] : 0, ref.gid.size() > 7 ? ref.gid[3] : 0,
              ref.gid.size() > 7 ? ref.gid[4] : 0, ref.gid.size() > 7 ? ref.gid[5] : 0,
              ref.gid.size() > 7 ? ref.gid[6] : 0, ref.gid.size() > 7 ? ref.gid[7] : 0);]==]
[==[    if (currentTracksIndex > 0 && position < 3000) {
      queueNextTrack(-1);]==]
[==[    // overlay 12: a second "previous" within 6 s of the last one goes to the
    // previous track even when the restarted track already played > 3 s
    // (first press restarts, second press goes back - like a CD player).
    static auto sc_last_prev = std::chrono::steady_clock::time_point{};
    auto sc_now = std::chrono::steady_clock::now();
    bool sc_repeat = sc_last_prev.time_since_epoch().count() != 0 &&
                     sc_now - sc_last_prev < std::chrono::seconds(6);
    sc_last_prev = sc_now;
    bool sc_back = currentTracksIndex > 0 && (position < 3000 || sc_repeat);
    CSPOT_LOG(info, "prev: position %u ms, index %d%s -> %s",  // overlay 10
              (unsigned)position, (int)currentTracksIndex, sc_repeat ? ", repeated" : "",
              sc_back ? "previous track" : "restart");
    if (sc_back) {
      queueNextTrack(-1);]==]
[==[        if (preloadedTracks.size() < MAX_TRACKS_PRELOAD) {]==]
[==[        if (preloadedTracks.size() < 2) {  // overlay 6: preload 1 track ahead (was MAX_TRACKS_PRELOAD = 3)]==])

# --- 7. cspot: wait for a loading track instead of skipping it after 5 s --------
# TrackPlayer gave every track 5 s to become READY and otherwise skipped it on
# its own ("Track failed to load, skipping it") - even when it was still being
# loaded. With a weak WiFi signal the HTTPS requests for a track (storage-resolve,
# CDN) regularly take longer, so the device skipped tracks the app had never
# skipped: device and app out of step. Now it waits until the track is READY or
# really FAILED (or the user picks something else), at most 150 s (Spotify's
# key throttling, overlay 6, can last a while).
sc_overlay_source(_cspot_sources "${CSPOT_DIR}/src/TrackPlayer.cpp"
    "#include <mutex>        // for mutex, scoped_lock"
    "#include <atomic>       // overlay 7\n#include <chrono>       // overlay 7\n#include <mutex>        // for mutex, scoped_lock\n// overlay 7: set when a track cannot be loaded; spotify_connect.cpp skips it\n// through SpircHandler so that Spotify and the app learn about the skip\nstd::atomic<bool> sc_track_load_failed{false};\nstd::atomic<bool> sc_next_track_failed{false};"
    "while (!eof && currentSongPlaying && !pendingReset && toWrite > 0) {"
    "while (!eof && currentSongPlaying && !pendingReset && pendingSeekPositionMs == 0 &&\n                   toWrite > 0) {  // overlay 4: drop stale audio on seek"
[==[    if (track->state != QueuedTrack::State::READY) {
      track->loadedSemaphore->twait(5000);

      if (track->state != QueuedTrack::State::READY) {]==]
[==[    if (track->state != QueuedTrack::State::READY) {
      // overlay 7: wait while the track is still loading (was: 5 s, then skip)
      auto waitStart = std::chrono::steady_clock::now();
      while (track->state != QueuedTrack::State::READY &&
             track->state != QueuedTrack::State::FAILED && !pendingReset &&
             std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(150)) {
        track->loadedSemaphore->twait(250);
      }
      if (pendingReset) {
        continue;
      }

      if (track->state != QueuedTrack::State::READY) {
        CSPOT_LOG(error, "Track %s not loadable (state %d)", track->identifier.c_str(),
                  (int)track->state);
        // Ask our session task for a proper skip (it calls nextSong(), which
        // reloads the player and tells Spotify). Playing the next queue entry
        // silently left Spotify/app on the failed track. Fallback after 2 s.
        // Only for the queue head: a failing *next* track must not cut off the
        // one that is still playing from the buffer.
        if (trackOffset == 0) {
          sc_track_load_failed = true;
          for (int i = 0; i < 40 && !pendingReset; i++) {
            BELL_SLEEP_MS(50);
          }
          if (pendingReset) {
            continue;
          }
          sc_track_load_failed = false;
        } else {
          // The *next* track failed while the current one still plays from
          // the buffer. Do not jump over it silently (the app would stay
          // behind): idle until the buffer has played out; then the session
          // task skips properly (nextSong), which makes the failed track the
          // head and skips it with notification. Fallback after 15 min.
          sc_next_track_failed = true;
          for (int i = 0; i < 18000 && !pendingReset && sc_next_track_failed; i++) {
            BELL_SLEEP_MS(50);
          }
          sc_next_track_failed = false;
          if (pendingReset) {
            continue;
          }
        }]==])

# --- 8. bell: time out stalled TLS connections ---------------------------------
# TLS sockets had no timeouts: when a packet got lost for good (weak WiFi), a
# request hung until TCP gave up minutes later, and with it cspot's queue task
# that loads every track. 15 s for every receive/send; the callers already
# treat a failed read as end of data and the queue retries (overlay 6).
sc_overlay_source(_bell_sources "${BELL_DIR}/main/io/TLSSocket.cpp"
[==[#include <stdexcept>              // for runtime_error]==]
[==[#include <stdexcept>              // for runtime_error
#include <sys/socket.h>             // overlay 8: SO_RCVTIMEO/SO_SNDTIMEO
#include <sys/time.h>]==]
[==[    BELL_LOG(error, "http_tls", "failed! connect returned %d\n", ret);
  }
]==]
[==[    BELL_LOG(error, "http_tls", "failed! connect returned %d\n", ret);
  } else {
    // overlay 8: never block forever on a dead connection
    struct timeval tv = {15, 0};
    setsockopt(server_fd.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(server_fd.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
]==])
