/*
 * spotify_connect.cpp - glue between cspot and the rest of the firmware.
 *
 * Structure (modelled on squeezelite-esp32's components/spotify/Shim.cpp, which
 * is the reference integration of the philippe44/cspot fork):
 *
 *   "sc_session" task (bell::Task, 32 kB stack in PSRAM)
 *       login (stored credentials or Zeroconf) -> cspot::SpircHandler ->
 *       loop { MercurySession::handlePacket(); local commands; track marks }
 *   cspot's own tasks (PSRAM stacks): "cspot_player" (download, decrypt, Vorbis
 *       decode, calls pcmWrite()), "mercury_dispatcher", TrackQueue
 *   "sc_events" task (internal-RAM stack)
 *       delivers events to the application callback and writes credentials to
 *       NVS. Needed because tasks with a PSRAM stack must never touch flash.
 *
 * Rule for everything in this file that runs on a cspot/bell task: no NVS, no
 * file system, no esp_partition/esp_flash calls.
 */
#include "spotify_connect.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <algorithm>

// cspot / bell
#include "BellLogger.h"
#include "BellTask.h"
#include "BellUtils.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "WrappedSemaphore.h"

#include "audio_out.h"

// Defined in cspot's TrackPlayer.cpp (overlay 7): a track could not be loaded.
extern std::atomic<bool> sc_track_load_failed;
// Same, but for the track after the current one (see skipFailedTrack()).
extern std::atomic<bool> sc_next_track_failed;
#include "AccessKeyFetcher.h"
#include "HTTPClient.h"
#include "MercurySession.h"
#include "cJSON.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sc_credentials.h"
#include "sc_zeroconf.h"

namespace {

const char* TAG = "spotify";

constexpr uint16_t kDefaultZeroconfPort = 7864;
constexpr int kSessionStackBytes = 32 * 1024;  // PSRAM; TLS handshakes run here
constexpr int kSessionCore = 1;
constexpr int kEventTaskStack = 6144;  // internal RAM; runs the app callback + NVS writes
constexpr int kEventTaskPrio = 5;
constexpr int kEventQueueLen = 16;
constexpr int kCmdQueueLen = 8;
constexpr TickType_t kPcmWriteTimeout = pdMS_TO_TICKS(20);

// ---------------------------------------------------------------------------
// bell/cspot log output -> ESP-IDF log, tag "cspot"
// ---------------------------------------------------------------------------
class EspLogger : public bell::AbstractLogger {
 public:
  void debug(std::string filename, int line, std::string submodule, const char* format,
             ...) override {
    va_list args;
    va_start(args, format);
    write(ESP_LOG_DEBUG, filename, line, format, args);
    va_end(args);
  }
  void error(std::string filename, int line, std::string submodule, const char* format,
             ...) override {
    va_list args;
    va_start(args, format);
    write(ESP_LOG_ERROR, filename, line, format, args);
    va_end(args);
  }
  void info(std::string filename, int line, std::string submodule, const char* format,
            ...) override {
    va_list args;
    va_start(args, format);
    write(ESP_LOG_INFO, filename, line, format, args);
    va_end(args);
  }

 private:
  static void write(esp_log_level_t level, const std::string& filename, int line,
                    const char* format, va_list args) {
    static const char* kTag = "cspot";
    if (esp_log_level_get(kTag) < level) return;
    // Small buffer on purpose: this also runs on cspot's smallest task,
    // "mercury_dispatcher" (4 kB stack upstream, 16 kB with our overlay).
    char msg[160];
    vsnprintf(msg, sizeof(msg), format, args);
    size_t slash = filename.find_last_of("/\\");
    const char* base = filename.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    ESP_LOG_LEVEL(level, kTag, "%s:%d: %s", base, line, msg);
  }
};

// ---------------------------------------------------------------------------
// Messages between tasks
// ---------------------------------------------------------------------------
enum class Cmd : uint8_t { Play, Pause, Toggle, Next, Prev, Volume };

struct CmdMsg {
  Cmd cmd;
  uint16_t arg;
};

/* Heap-allocated, a pointer travels through the event queue. */
struct QueuedEvent {
  enum class Kind { App, SaveCredentials } kind = Kind::App;
  spotify_event_id_t id = SPOTIFY_EVENT_INACTIVE;
  spotify_playback_t playback = {};
  uint16_t volume = 0;
  bool storedCredentials = false;
  uint32_t durationMs = 0;
  uint32_t positionMs = 0;
  std::string trackId, title, artist, album, coverUrl;
  std::string text;  // LOGGED_IN: user name; SaveCredentials: JSON
};

// ---------------------------------------------------------------------------
// The player
// ---------------------------------------------------------------------------
class Player : public bell::Task {
 public:
  Player(const spotify_connect_config_t& cfg, std::string storedCredentials)
      : bell::Task("sc_session", kSessionStackBytes, 0, kSessionCore),
        deviceName_(cfg.device_name),
        clientId_(cfg.client_id),
        clientSecret_(cfg.client_secret),
        bitrate_(cfg.bitrate),
        zeroconfPort_(cfg.zeroconf_port ? cfg.zeroconf_port : kDefaultZeroconfPort),
        eventCb_(cfg.event_cb),
        userCtx_(cfg.user_ctx),
        storedCredentials_(std::move(storedCredentials)) {
    volume_ = cfg.initial_volume > UINT16_MAX ? UINT16_MAX : (int)cfg.initial_volume;
  }

  esp_err_t begin() {
    audio_out_set_volume((uint16_t)volume_);
    cmdQueue_ = xQueueCreate(kCmdQueueLen, sizeof(CmdMsg));
    eventQueue_ = xQueueCreate(kEventQueueLen, sizeof(QueuedEvent*));
    if (!cmdQueue_ || !eventQueue_) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(eventTaskEntry, "sc_events", kEventTaskStack, this, kEventTaskPrio,
                                nullptr, tskNO_AFFINITY) != pdPASS) {
      return ESP_ERR_NO_MEM;
    }
    return startTask() ? ESP_OK : ESP_ERR_NO_MEM;
  }

  esp_err_t postCommand(Cmd cmd, uint16_t arg = 0) {
    {
      // Commands are executed by the session loop, which only runs while
      // logged in; do not let them pile up in the queue otherwise.
      std::scoped_lock lock(statusMutex_);
      if (!loggedIn_) return ESP_ERR_INVALID_STATE;
    }
    CmdMsg msg = {cmd, arg};
    return xQueueSend(cmdQueue_, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
  }

  esp_err_t playUri(const char* uri) {
    {
      std::scoped_lock lock(statusMutex_);
      // authenticated_: SPOTIFY_EVENT_LOGGED_IN is emitted just before the
      // session loop starts; a request made in reaction to it is kept and
      // handled as soon as the loop runs (tag placed before login).
      if (!loggedIn_ && !authenticated_) return ESP_ERR_INVALID_STATE;
    }
    std::scoped_lock lock(playMutex_);
    pendingUri_ = uri;
    return ESP_OK;
  }

  void getStatus(spotify_connect_status_t* out) {
    std::scoped_lock lock(statusMutex_);
    out->logged_in = loggedIn_;
    out->active = active_;
    out->paused = paused_;
    out->duration_ms = durationMs_;
    out->position_ms = positionNowLocked();
  }

 protected:
  void runTask() override;

 private:
  // --- configuration (immutable after construction) ---
  const std::string deviceName_, clientId_, clientSecret_;
  const spotify_bitrate_t bitrate_;
  const uint16_t zeroconfPort_;
  const spotify_event_cb_t eventCb_;
  void* const userCtx_;
  std::string storedCredentials_;

  // --- infrastructure ---
  QueueHandle_t cmdQueue_ = nullptr;
  QueueHandle_t eventQueue_ = nullptr;
  bell::WrappedSemaphore loginReady_;  // given by the Zeroconf HTTP handler
  sc::Zeroconf zeroconf_;

  // --- cspot objects, owned by the session task ---
  std::shared_ptr<cspot::LoginBlob> blob_;
  std::unique_ptr<cspot::SpircHandler> spirc_;

  // --- track bookkeeping (see pcmWrite/pollPlayback) ---
  std::mutex trackMutex_;
  std::string streamTrackId_;  // track whose PCM is currently being written
  std::string playTrackId_;    // track that is audible
  std::atomic<bool> trackMarkPending_{false};
  std::atomic<uint64_t> trackMarkFrame_{0};
  std::atomic<bool> queueDepleted_{false};
  std::atomic<bool> flushed_{false};
  std::atomic<bool> notifyNext_{true};
  std::atomic<int> startOffsetMs_{0};
  bool trackInfoFresh_ = false;  // session task only
  cspot::TrackInfo trackInfo_;   // session task only
  std::atomic<int> volume_{UINT16_MAX};  // applied with audio_out_set_volume()

  // --- status snapshot for spotify_connect_get_status() ---
  std::mutex statusMutex_;
  bool loggedIn_ = false, active_ = false, paused_ = false;
  bool authenticated_ = false;  // guarded by statusMutex_, see playUri()
  uint32_t durationMs_ = 0;
  uint32_t posBaseMs_ = 0;   // position at posBaseUs_
  int64_t posBaseUs_ = 0;    // esp_timer time of the last position update

  uint32_t positionNowLocked() const {
    if (!active_) return 0;
    uint32_t pos = posBaseMs_;
    if (!paused_) pos += (uint32_t)((esp_timer_get_time() - posBaseUs_) / 1000);
    if (durationMs_ && pos > durationMs_) pos = durationMs_;
    return pos;
  }
  void setPositionLocked(uint32_t ms) {
    posBaseMs_ = ms;
    posBaseUs_ = esp_timer_get_time();
  }

  // --- helpers ---
  bool login(std::shared_ptr<cspot::Context>& ctx, bool usingStored);
  void runSession(std::shared_ptr<cspot::Context> ctx);
  void startZeroconf();
  void forgetStreamingTrack();
  // Device-side context start (NFC), see spotify_connect_play_uri()
  void processPlayRequests();
  void resolveContext(const std::string& uri);
  void startWebResolve(const std::string& uri, uint32_t seq);
  void resolveFallback(const std::string& uri, uint32_t seq, std::vector<std::string> sources);
  static void webResolveTask(void* arg);
  bool collectTrackUris(cJSON* root, std::string& spec, int limit);
  std::mutex playMutex_;
  std::string pendingUri_;     // requested by spotify_connect_play_uri()
  std::string pendingLoad_;    // resolved "<context>\n<track>..." to load
  std::string resolvingUri_;   // session task only
  uint32_t resolveSeq_ = 0;    // guarded by playMutex_
  int64_t resolveStartedUs_ = 0;
  bool webStarted_ = false;
  std::shared_ptr<cspot::Context> sessionCtx_;
  void skipFailedTrack();
  void onSpircEvent(std::unique_ptr<cspot::SpircHandler::Event> event);
  size_t pcmWrite(uint8_t* pcm, size_t bytes, std::string_view trackId);
  void pollPlayback();
  void handleCommands();

  void emit(QueuedEvent* ev) {
    if (xQueueSend(eventQueue_, &ev, 0) != pdTRUE) {
      ESP_LOGW(TAG, "event queue full, dropping event %d", (int)ev->id);
      delete ev;
    }
  }
  void emitSimple(spotify_event_id_t id) {
    auto* ev = new QueuedEvent();
    ev->id = id;
    emit(ev);
  }
  void emitPlayback(spotify_event_id_t id, bool paused, uint32_t positionMs) {
    auto* ev = new QueuedEvent();
    ev->id = id;
    ev->playback.paused = paused;
    ev->playback.position_ms = positionMs;
    emit(ev);
  }

  static void eventTaskEntry(void* arg) { static_cast<Player*>(arg)->eventTask(); }
  void eventTask();
};

// ---------------------------------------------------------------------------
// Event task: internal-RAM stack, the only place where we touch NVS and where
// application code is called.
// ---------------------------------------------------------------------------
void Player::eventTask() {
  for (;;) {
    QueuedEvent* ev = nullptr;
    if (xQueueReceive(eventQueue_, &ev, portMAX_DELAY) != pdTRUE || ev == nullptr) continue;

    if (ev->kind == QueuedEvent::Kind::SaveCredentials) {
      sc_credentials_save(ev->text.c_str());
    } else if (eventCb_ != nullptr) {
      spotify_event_t out = {};
      out.id = ev->id;
      switch (ev->id) {
        case SPOTIFY_EVENT_LOGGED_IN:
          out.data.session.username = ev->text.c_str();
          out.data.session.stored_credentials = ev->storedCredentials;
          break;
        case SPOTIFY_EVENT_PLAYBACK_STARTED:
        case SPOTIFY_EVENT_PLAYBACK_PAUSED:
        case SPOTIFY_EVENT_PLAYBACK_RESUMED:
        case SPOTIFY_EVENT_SEEK:
          out.data.playback = ev->playback;
          break;
        case SPOTIFY_EVENT_TRACK_CHANGED:
          out.data.track.track_id = ev->trackId.c_str();
          out.data.track.title = ev->title.c_str();
          out.data.track.artist = ev->artist.c_str();
          out.data.track.album = ev->album.c_str();
          out.data.track.cover_url = ev->coverUrl.c_str();
          out.data.track.duration_ms = ev->durationMs;
          out.data.track.position_ms = ev->positionMs;
          break;
        case SPOTIFY_EVENT_VOLUME:
          out.data.volume = ev->volume;
          break;
        default:
          break;
      }
      eventCb_(&out, userCtx_);
    }
    delete ev;
  }
}

// ---------------------------------------------------------------------------
// cspot -> us: PCM data. Runs on cspot's "cspot_player" task.
// Returning less than `bytes` (even 0) is fine, cspot retries after a sleep.
// ---------------------------------------------------------------------------
size_t Player::pcmWrite(uint8_t* pcm, size_t bytes, std::string_view trackId) {
  /* The ring buffer in audio_out holds a few seconds, so the decoder is ahead
   * of what is audible: at any time there is one "playing" and at most one
   * "streaming" track. A third one is held back until the track change has
   * been noticed by pollPlayback() (same logic as squeezelite/cspot CLI). */
  if (streamTrackId_ != trackId) {
    std::unique_lock lock(trackMutex_);
    if (playTrackId_ != trackId && playTrackId_ != streamTrackId_) return 0;
    streamTrackId_ = trackId;
    lock.unlock();

    // Remember where in the output stream the new track starts.
    audio_out_stats_t st;
    audio_out_get_stats(&st);
    trackMarkFrame_ = st.frames_written;
    trackMarkPending_ = true;
    ESP_LOGI(TAG, "stream switched to track %.*s", (int)trackId.size(), trackId.data());
  }
  return audio_out_write(pcm, bytes, kPcmWriteTimeout);
}

// ---------------------------------------------------------------------------
// cspot -> us: events. Called on the session task (remote commands, our own
// commands) or on the cspot_player task (PLAYBACK_START, PLAY_PAUSE after a
// track was loaded, DEPLETED).
// ---------------------------------------------------------------------------
// The queue changed under us (skip, Load, Replace): the track that is being
// streamed into the buffer will never be heard, so its "reached playback" mark
// must not fire. Otherwise the flush below would satisfy the mark (dropped
// frames count as passed) and pollPlayback() would announce a track to
// Spotify and to cspot's queue that never played -> device and app drift apart.
void Player::forgetStreamingTrack() {
  trackMarkPending_ = false;
  std::scoped_lock lock(trackMutex_);
  streamTrackId_.clear();
}

void Player::onSpircEvent(std::unique_ptr<cspot::SpircHandler::Event> event) {
  using EventType = cspot::SpircHandler::EventType;

  switch (event->eventType) {
    case EventType::PLAYBACK_START: {
      // A controller loaded something new (or we skipped): whatever is still
      // buffered belongs to the past.
      {
        std::scoped_lock lock(trackMutex_);
        playTrackId_.clear();
        streamTrackId_.clear();
      }
      trackMarkPending_ = false;
      queueDepleted_ = false;
      startOffsetMs_ = std::get<int>(event->data);
      notifyNext_ = !flushed_;
      flushed_ = false;
      audio_out_flush();
      // Spotify's servers do not send the volume when a device becomes active.
      spirc_->setRemoteVolume(volume_);
      {
        std::scoped_lock lock(statusMutex_);
        active_ = true;
        setPositionLocked(startOffsetMs_);
      }
      ESP_LOGI(TAG, "playback start at %d ms", startOffsetMs_.load());
      emitPlayback(SPOTIFY_EVENT_PLAYBACK_STARTED, false, startOffsetMs_);
      break;
    }
    case EventType::PLAY_PAUSE: {
      bool paused = std::get<bool>(event->data);
      uint32_t pos;
      {
        std::scoped_lock lock(statusMutex_);
        setPositionLocked(positionNowLocked());  // freeze/restart extrapolation
        paused_ = paused;
        pos = posBaseMs_;
      }
      audio_out_set_paused(paused);
      emitPlayback(paused ? SPOTIFY_EVENT_PLAYBACK_PAUSED : SPOTIFY_EVENT_PLAYBACK_RESUMED, paused,
                   pos);
      break;
    }
    case EventType::TRACK_INFO:
      // Emitted synchronously from notifyAudioReachedPlayback(), see pollPlayback().
      trackInfo_ = std::get<cspot::TrackInfo>(event->data);
      trackInfoFresh_ = true;
      break;
    case EventType::FLUSH:
      // The queue was replaced and the current track gets re-loaded: the next
      // PLAYBACK_START must not advance cspot's queue again.
      flushed_ = true;
      [[fallthrough]];
    case EventType::NEXT:   // also sent for a Load frame (overlay 5)
    case EventType::PREV:
      ESP_LOGI(TAG, "spirc %s: dropping buffered audio",
               event->eventType == EventType::FLUSH  ? "FLUSH"
               : event->eventType == EventType::NEXT ? "NEXT/LOAD"
                                                     : "PREV");
      forgetStreamingTrack();
      audio_out_flush();
      break;
    case EventType::SEEK: {
      int pos = std::get<int>(event->data);
      bool paused;
      audio_out_flush();
      {
        std::scoped_lock lock(statusMutex_);
        setPositionLocked(pos);
        paused = paused_;
      }
      emitPlayback(SPOTIFY_EVENT_SEEK, paused, pos);
      break;
    }
    case EventType::DEPLETED:
      // Last track fully decoded; pollPlayback() reports the end once the
      // buffer has been played out.
      queueDepleted_ = true;
      break;
    case EventType::VOLUME: {
      volume_ = std::get<int>(event->data);
      audio_out_set_volume((uint16_t)volume_);
      auto* ev = new QueuedEvent();
      ev->id = SPOTIFY_EVENT_VOLUME;
      ev->volume = (uint16_t)volume_;
      emit(ev);
      break;
    }
    case EventType::DISC:
      // Another device took over.
      audio_out_flush();
      audio_out_set_paused(false);
      trackMarkPending_ = false;
      queueDepleted_ = false;
      {
        std::scoped_lock lock(statusMutex_);
        active_ = false;
        paused_ = false;
      }
      emitSimple(SPOTIFY_EVENT_INACTIVE);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Session task, ~5x per second: has the audio of a new track reached the DAC?
// ---------------------------------------------------------------------------
void Player::pollPlayback() {
  if (!trackMarkPending_ && !queueDepleted_) return;

  audio_out_stats_t st;
  audio_out_get_stats(&st);

  if (trackMarkPending_ && st.frames_played + st.frames_dropped >= trackMarkFrame_) {
    trackMarkPending_ = false;
    int startOffset = startOffsetMs_.exchange(0);

    trackInfoFresh_ = false;
    {
      std::scoped_lock lock(trackMutex_);
      playTrackId_ = streamTrackId_;
      // Tells Spotify (and cspot's queue) that this track is now playing; cspot
      // answers with a TRACK_INFO event before the call returns.
      if (notifyNext_) {
        spirc_->notifyAudioReachedPlayback(playTrackId_);
      } else {
        notifyNext_ = true;
      }
    }
    spirc_->updatePositionMs(startOffset);

    {
      std::scoped_lock lock(statusMutex_);
      setPositionLocked(startOffset);
      if (trackInfoFresh_) durationMs_ = trackInfo_.duration;
    }
    if (trackInfoFresh_) {
      ESP_LOGI(TAG, "now playing: %s - %s (%s), %u ms", trackInfo_.artist.c_str(),
               trackInfo_.name.c_str(), trackInfo_.album.c_str(), (unsigned)trackInfo_.duration);
      auto* ev = new QueuedEvent();
      ev->id = SPOTIFY_EVENT_TRACK_CHANGED;
      ev->trackId = trackInfo_.trackId;
      ev->title = trackInfo_.name;
      ev->artist = trackInfo_.artist;
      ev->album = trackInfo_.album;
      ev->coverUrl = trackInfo_.imageUrl;
      ev->durationMs = trackInfo_.duration;
      ev->positionMs = startOffset;
      emit(ev);
    }
  }

  if (queueDepleted_ && !trackMarkPending_ && st.buffered_bytes == 0) {
    queueDepleted_ = false;
    ESP_LOGI(TAG, "last track finished");
    spirc_->notifyAudioEnded();
    {
      std::scoped_lock lock(statusMutex_);
      paused_ = true;
      setPositionLocked(0);
    }
    emitSimple(SPOTIFY_EVENT_PLAYBACK_STOPPED);
  }
}

// ---------------------------------------------------------------------------
// Session task: execute local transport commands
// ---------------------------------------------------------------------------
void Player::handleCommands() {
  CmdMsg msg;
  while (xQueueReceive(cmdQueue_, &msg, 0) == pdTRUE) {
    bool active, paused;
    {
      std::scoped_lock lock(statusMutex_);
      active = active_;
      paused = paused_;
    }
    if (msg.cmd == Cmd::Volume) {
      // Does not come back as an event, so report it ourselves.
      volume_ = msg.arg;
      audio_out_set_volume(msg.arg);
      spirc_->setRemoteVolume(msg.arg);
      auto* ev = new QueuedEvent();
      ev->id = SPOTIFY_EVENT_VOLUME;
      ev->volume = msg.arg;
      emit(ev);
      continue;
    }
    if (!active) {
      ESP_LOGW(TAG, "command %d ignored: device is not the active player", (int)msg.cmd);
      continue;
    }
    switch (msg.cmd) {
      // setPause() comes back as a PLAY_PAUSE event
      case Cmd::Play:
        spirc_->setPause(false);
        break;
      case Cmd::Pause:
        spirc_->setPause(true);
        break;
      case Cmd::Toggle:
        spirc_->setPause(!paused);
        break;
      // a successful skip re-loads the player -> PLAYBACK_START event
      case Cmd::Next:
        if (spirc_->nextSong()) {
          forgetStreamingTrack();
          audio_out_flush();
        }
        break;
      case Cmd::Prev:
        if (spirc_->previousSong()) {
          forgetStreamingTrack();
          audio_out_flush();
        }
        break;
      default:
        break;
    }
  }
}

void Player::startZeroconf() {
  if (zeroconf_.isRunning()) return;
  esp_err_t err = zeroconf_.start(blob_, zeroconfPort_, [this]() { loginReady_.give(); });
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Zeroconf could not be started: %s", esp_err_to_name(err));
  }
}

// Connect to an access point and authenticate. Returns false if Spotify
// rejected the login; throws on network errors.
bool Player::login(std::shared_ptr<cspot::Context>& ctx, bool usingStored) {
  ctx = cspot::Context::createFromBlob(blob_);
  ctx->config.clientId = clientId_;
  ctx->config.clientSecret = clientSecret_;
  switch (bitrate_) {
    case SPOTIFY_BITRATE_320:
      ctx->config.audioFormat = AudioFormat_OGG_VORBIS_320;
      break;
    case SPOTIFY_BITRATE_96:
      ctx->config.audioFormat = AudioFormat_OGG_VORBIS_96;
      break;
    default:
      ctx->config.audioFormat = AudioFormat_OGG_VORBIS_160;
      break;
  }

  ctx->session->connectWithRandomAp();
  ctx->config.authData = ctx->session->authenticate(blob_);
  if (ctx->config.authData.empty()) return false;

  // Reusable credentials: persist them (on the event task, see file header).
  auto* save = new QueuedEvent();
  save->kind = QueuedEvent::Kind::SaveCredentials;
  save->text = ctx->getCredentialsJson();
  emit(save);

  {
    std::scoped_lock lock(statusMutex_);
    authenticated_ = true;
  }
  auto* ev = new QueuedEvent();
  ev->id = SPOTIFY_EVENT_LOGGED_IN;
  ev->text = ctx->config.username;
  ev->storedCredentials = usingStored;
  emit(ev);
  return true;
}

// Runs until the session has to be torn down (exception).
// A track could not be loaded (key refused, CDN unreachable): skip it the way
// a "next" from the app would, so that Spotify and the app follow.
void Player::skipFailedTrack() {
  bool now = sc_track_load_failed.exchange(false);
  if (!now && sc_next_track_failed) {
    // The next track could not be loaded: let the current one play to the
    // end, then skip (to the failed one, which is then skipped as head).
    audio_out_stats_t st;
    audio_out_get_stats(&st);
    bool paused;
    {
      std::scoped_lock lock(statusMutex_);
      paused = paused_;
    }
    if (paused || st.buffered_bytes > 0) return;
    sc_next_track_failed = false;
    now = true;
  }
  if (!now) return;
  bool active;
  {
    std::scoped_lock lock(statusMutex_);
    active = active_;
  }
  if (!active) return;
  ESP_LOGW(TAG, "track not loadable, skipping to the next one");
  if (spirc_->nextSong()) {
    forgetStreamingTrack();
    audio_out_flush();
  }
}


// ---------------------------------------------------------------------------
// Device-side context start (NFC tag): resolve the context to a track list,
// then let SpircHandler::loadTrackFromURI() (overlay 11) act as if the app had
// sent a Load frame. Session task only, except webResolveTask.
// ---------------------------------------------------------------------------
static constexpr int kMaxContextTracks = 200;
static constexpr int64_t kMercuryResolveTimeoutUs = 6 * 1000 * 1000;

// Appends the track/episode URIs found in a context-resolve or Web API
// answer to spec (one per line); returns true if at least one was found.
static void collectUris(cJSON* node, std::string& spec, int& count, int limit) {
  if (node == nullptr || count >= limit) return;
  if (cJSON_IsObject(node)) {
    cJSON* uri = cJSON_GetObjectItemCaseSensitive(node, "uri");
    if (cJSON_IsString(uri) && uri->valuestring &&
        (strncmp(uri->valuestring, "spotify:track:", 14) == 0 ||
         strncmp(uri->valuestring, "spotify:episode:", 16) == 0)) {
      spec += "\n";
      spec += uri->valuestring;
      count++;
      return;
    }
  }
  for (cJSON* child = node->child; child != nullptr && count < limit; child = child->next) {
    collectUris(child, spec, count, limit);
  }
}

bool Player::collectTrackUris(cJSON* root, std::string& spec, int limit) {
  int count = 0;
  collectUris(root, spec, count, limit);
  return count > 0;
}

// Finds "spotify:track:<22 base62>" / "spotify:episode:..." anywhere in a
// binary answer (protobuf playlist, JSON of unknown shape). Consecutive
// duplicates are skipped.
static bool isBase62(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int scanUris(const uint8_t* data, size_t len, std::string& spec, int limit) {
  static const char* kPrefixes[] = {"spotify:track:", "spotify:episode:"};
  std::string_view all(reinterpret_cast<const char*>(data), len);
  std::string last;
  int count = 0;
  size_t pos = 0;
  while (count < limit) {
    size_t best = std::string_view::npos, plen = 0;
    for (const char* p : kPrefixes) {
      size_t f = all.find(p, pos);
      if (f < best) { best = f; plen = strlen(p); }
    }
    if (best == std::string_view::npos) break;
    size_t idStart = best + plen;
    size_t n = 0;
    // ids are always 22 characters; a protobuf field tag right behind the
    // string may itself be a printable character, so do not require a stop
    while (idStart + n < len && isBase62(all[idStart + n]) && n < 22) n++;
    pos = idStart + n;
    if (n != 22) continue;
    std::string uri(all.substr(best, plen + 22));
    if (uri == last) continue;
    last = uri;
    spec += "\n";
    spec += uri;
    count++;
  }
  return count;
}

// Collects "page_url"/"next_page_url" values (hm://...) of a context-resolve
// answer: large or personalised contexts come as references to pages.
static void collectPageUrls(cJSON* node, std::vector<std::string>& out) {
  if (node == nullptr || out.size() >= 4) return;
  if (cJSON_IsObject(node)) {
    for (const char* key : {"page_url", "next_page_url"}) {
      cJSON* u = cJSON_GetObjectItemCaseSensitive(node, key);
      if (cJSON_IsString(u) && u->valuestring && strncmp(u->valuestring, "hm://", 5) == 0) {
        out.emplace_back(u->valuestring);
      }
    }
  }
  for (cJSON* child = node->child; child != nullptr; child = child->next) {
    collectPageUrls(child, out);
  }
}

// Second chance over Mercury (the user's own session, so it also works for
// Spotify's personalised playlists that the Web API refuses for third-party
// apps): the page references of the context-resolve answer, then the
// playlist itself. Each answer is scanned for track URIs. Only when all of
// them fail the Web API is asked. Session task (Mercury callbacks run there).
void Player::resolveFallback(const std::string& uri, uint32_t seq, std::vector<std::string> sources) {
  {
    std::scoped_lock lock(playMutex_);
    if (seq != resolveSeq_) return;   // superseded or already resolved
  }
  if (sources.empty()) {
    if (!webStarted_) startWebResolve(uri, seq);
    return;
  }
  std::string src = sources.front();
  sources.erase(sources.begin());
  ESP_LOGI(TAG, "resolving %s via %s", uri.c_str(), src.c_str());
  sessionCtx_->session->execute(
      cspot::MercurySession::RequestType::GET, src,
      [this, uri, seq, src, sources](cspot::MercurySession::Response& res) {
        std::string spec = uri;
        int n = 0;
        if (!res.fail) {
          for (auto& part : res.parts) {
            n += scanUris(part.data(), part.size(), spec, kMaxContextTracks - n);
          }
        }
        {
          std::scoped_lock lock(playMutex_);
          if (seq != resolveSeq_) return;
          if (n > 0) {
            ESP_LOGI(TAG, "%s: %d tracks via %s", uri.c_str(), n, src.c_str());
            pendingLoad_ = spec;
            resolveSeq_++;            // late answers (Web API) must not load again
            return;
          }
        }
        ESP_LOGW(TAG, "%s: nothing usable from %s (fail=%d)", uri.c_str(), src.c_str(), (int)res.fail);
        resolveFallback(uri, seq, sources);
      });
}

void Player::processPlayRequests() {
  std::string uri, load;
  uint32_t seq;
  {
    std::scoped_lock lock(playMutex_);
    uri.swap(pendingUri_);
    load.swap(pendingLoad_);
    if (!uri.empty()) resolveSeq_++;   // a newer request wins
    seq = resolveSeq_;
  }
  if (!load.empty()) {
    resolvingUri_.clear();
    ESP_LOGI(TAG, "starting %s on this device", load.substr(0, load.find('\n')).c_str());
    forgetStreamingTrack();
    audio_out_flush();
    audio_out_set_paused(false);
    spirc_->loadTrackFromURI(load);
  }
  if (!uri.empty()) {
    resolveContext(uri);
    return;
  }
  // context-resolve did not answer in time: try the Web API
  if (!resolvingUri_.empty() && !webStarted_ &&
      esp_timer_get_time() - resolveStartedUs_ > kMercuryResolveTimeoutUs) {
    ESP_LOGW(TAG, "context-resolve timed out for %s", resolvingUri_.c_str());
    startWebResolve(resolvingUri_, seq);
  }
}

void Player::resolveContext(const std::string& uri) {
  if (uri.rfind("spotify:track:", 0) == 0 || uri.rfind("spotify:episode:", 0) == 0) {
    std::scoped_lock lock(playMutex_);
    pendingLoad_ = uri + "\n" + uri;
    resolvingUri_.clear();
    return;
  }
  ESP_LOGI(TAG, "resolving %s", uri.c_str());
  resolvingUri_ = uri;
  resolveStartedUs_ = esp_timer_get_time();
  webStarted_ = false;
  uint32_t seq;
  {
    std::scoped_lock lock(playMutex_);
    seq = resolveSeq_;
  }
  sessionCtx_->session->execute(
      cspot::MercurySession::RequestType::GET, "hm://context-resolve/v1/" + uri,
      [this, uri, seq](cspot::MercurySession::Response& res) {
        {
          std::scoped_lock lock(playMutex_);
          if (seq != resolveSeq_) return;   // superseded
        }
        std::string spec = uri;
        bool ok = false;
        std::vector<std::string> sources;
        if (!res.fail && !res.parts.empty() && !res.parts[0].empty()) {
          cJSON* root = cJSON_ParseWithLength((const char*)res.parts[0].data(), res.parts[0].size());
          if (root) {
            ok = collectTrackUris(root, spec, kMaxContextTracks);
            if (!ok) collectPageUrls(root, sources);
            cJSON_Delete(root);
          }
          if (!ok) {
            // what did we get? (first bytes, for the next round of fixes)
            std::string head((const char*)res.parts[0].data(),
                             std::min<size_t>(res.parts[0].size(), 160));
            for (char& c : head) if (c < 0x20 || c > 0x7e) c = '.';
            ESP_LOGW(TAG, "context-resolve answer (%u bytes): %s", (unsigned)res.parts[0].size(),
                     head.c_str());
          }
        }
        if (ok) {
          ESP_LOGI(TAG, "context-resolve: %s ok", uri.c_str());
          std::scoped_lock lock(playMutex_);
          pendingLoad_ = spec;
          resolveSeq_++;              // a Web API lookup started on timeout must not load again
        } else {
          ESP_LOGW(TAG, "context-resolve gave no tracks for %s (fail=%d), trying other sources",
                   uri.c_str(), (int)res.fail);
          if (uri.rfind("spotify:playlist:", 0) == 0) {
            sources.push_back("hm://playlist/v2/playlist/" + uri.substr(17));
          }
          resolveFallback(uri, seq, std::move(sources));
        }
      });
}

struct WebResolveJob {
  Player* player;
  std::string uri;
  uint32_t seq;
  std::shared_ptr<cspot::Context> ctx;
};

void Player::startWebResolve(const std::string& uri, uint32_t seq) {
  webStarted_ = true;
  auto* job = new WebResolveJob{this, uri, seq, sessionCtx_};
  if (xTaskCreatePinnedToCoreWithCaps(webResolveTask, "sc_resolve", 12 * 1024, job, 3, nullptr,
                                      tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
    delete job;
    ESP_LOGE(TAG, "no memory for the Web API lookup");
    emitSimple(SPOTIFY_EVENT_PLAY_URI_FAILED);
  }
}

void Player::webResolveTask(void* arg) {
  auto* job = static_cast<WebResolveJob*>(arg);
  Player* self = job->player;
  std::string spec = job->uri;
  bool ok = false;
  try {
    // spotify:<type>:<id>
    size_t a = job->uri.find(':');
    size_t b = job->uri.find(':', a + 1);
    std::string type = job->uri.substr(a + 1, b - a - 1);
    std::string id = job->uri.substr(b + 1);
    std::string url;
    if (type == "album") {
      url = "https://api.spotify.com/v1/albums/" + id + "/tracks?limit=50";
    } else if (type == "playlist") {
      url = "https://api.spotify.com/v1/playlists/" + id + "/tracks?limit=100&fields=items(track(uri))";
    } else if (type == "artist") {
      url = "https://api.spotify.com/v1/artists/" + id + "/top-tracks?market=DE";
    } else if (type == "show") {
      url = "https://api.spotify.com/v1/shows/" + id + "/episodes?limit=50&market=DE";
    }
    if (!url.empty()) {
      cspot::AccessKeyFetcher keys(job->ctx);
      std::string token = keys.getAccessKey();
      auto resp = bell::HTTPClient::get(
          url, {bell::HTTPClient::ValueHeader({"Authorization", "Bearer " + token})});
      std::string_view body = resp->body();
      cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
      if (root) {
        ok = self->collectTrackUris(root, spec, kMaxContextTracks);
        if (!ok) {
          cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
          cJSON* msg = err ? cJSON_GetObjectItemCaseSensitive(err, "message") : nullptr;
          ESP_LOGW(TAG, "Web API: no tracks for %s (%s)", job->uri.c_str(),
                   cJSON_IsString(msg) ? msg->valuestring : "unexpected answer");
        }
        cJSON_Delete(root);
      }
    }
  } catch (const std::exception& e) {
    ESP_LOGW(TAG, "Web API lookup failed: %s", e.what());
  } catch (...) {
    ESP_LOGW(TAG, "Web API lookup failed");
  }
  {
    std::scoped_lock lock(self->playMutex_);
    if (job->seq == self->resolveSeq_) {
      if (ok) {
        ESP_LOGI(TAG, "Web API: %s ok", job->uri.c_str());
        self->pendingLoad_ = spec;
        self->resolveSeq_++;
      } else {
        self->emitSimple(SPOTIFY_EVENT_PLAY_URI_FAILED);
      }
    }
  }
  delete job;
  vTaskDeleteWithCaps(nullptr);
}

void Player::runSession(std::shared_ptr<cspot::Context> ctx) {
  spirc_ = std::make_unique<cspot::SpircHandler>(ctx);

  spirc_->getTrackPlayer()->setDataCallback(
      [this](uint8_t* data, size_t bytes, std::string_view trackId) {
        return pcmWrite(data, bytes, trackId);
      });
  spirc_->setEventHandler([this](std::unique_ptr<cspot::SpircHandler::Event> event) {
    onSpircEvent(std::move(event));
  });

  sessionCtx_ = ctx;
  // Starts cspot's receive task; from here on packets arrive in the queue that
  // handlePacket() drains.
  ctx->session->startTask();
  {
    std::scoped_lock lock(statusMutex_);
    loggedIn_ = true;
  }

  try {
    for (;;) {
      ctx->session->handlePacket();  // blocks for at most ~200 ms
      handleCommands();
      processPlayRequests();
      skipFailedTrack();
      pollPlayback();
    }
  } catch (const std::exception& e) {
    ESP_LOGE(TAG, "session aborted: %s", e.what());
  } catch (...) {
    ESP_LOGE(TAG, "session aborted: unknown exception");
  }

  {
    std::scoped_lock lock(statusMutex_);
    loggedIn_ = false;
    authenticated_ = false;
    active_ = false;
  }
  audio_out_flush();
  audio_out_set_paused(false);
  spirc_->disconnect();
  spirc_.reset();
  {
    std::scoped_lock lock(playMutex_);
    resolveSeq_++;            // a running web lookup must not load anything
    pendingUri_.clear();
    pendingLoad_.clear();
  }
  resolvingUri_.clear();
  sessionCtx_.reset();
  emitSimple(SPOTIFY_EVENT_INACTIVE);
}

void Player::runTask() {
  blob_ = std::make_shared<cspot::LoginBlob>(deviceName_);
  ESP_LOGI(TAG, "device \"%s\", id %s", blob_->getDeviceName().c_str(), blob_->getDeviceId().c_str());

  bool haveLogin = false;
  bool usingStored = false;
  if (!storedCredentials_.empty()) {
    try {
      blob_->loadJson(storedCredentials_);
      haveLogin = usingStored = true;
      ESP_LOGI(TAG, "using stored credentials of user \"%s\"", blob_->getUserName().c_str());
    } catch (const std::exception& e) {
      ESP_LOGE(TAG, "stored credentials unusable: %s", e.what());
    }
    storedCredentials_.clear();
  }

  for (;;) {
    if (!haveLogin) {
      // Wait until somebody picks the device in the Spotify app.
      startZeroconf();
      while (loginReady_.twait(0) == 0) {
        // discard signals of earlier, already handled login attempts
      }
      emitSimple(SPOTIFY_EVENT_ZEROCONF_WAITING);
      loginReady_.wait();
      haveLogin = true;
      usingStored = false;
    }

    std::shared_ptr<cspot::Context> ctx;
    bool accepted = false;
    bool networkError = false;
    try {
      accepted = login(ctx, usingStored);
    } catch (const std::exception& e) {
      ESP_LOGE(TAG, "connecting to Spotify failed: %s", e.what());
      networkError = true;
    } catch (...) {
      ESP_LOGE(TAG, "connecting to Spotify failed: unknown exception");
      networkError = true;
    }
    if (networkError) {
      // Network problem: keep the login material and try again.
      ctx.reset();
      BELL_SLEEP_MS(5000);
      continue;
    }

    if (!accepted) {
      ESP_LOGE(TAG, "login rejected by Spotify%s", usingStored ? " (stored credentials)" : "");
      emitSimple(SPOTIFY_EVENT_LOGIN_FAILED);
      ctx.reset();
      haveLogin = false;
      BELL_SLEEP_MS(1000);
      continue;
    }

    ESP_LOGI(TAG, "logged in as \"%s\"", ctx->config.username.c_str());
    // From now on the device is reachable through Spotify's servers and the
    // credentials are stored; the LAN login endpoint is no longer needed.
    zeroconf_.stop();
    usingStored = true;  // a re-login after an error uses the same blob

    runSession(ctx);

    ctx.reset();  // release sockets and buffers before reconnecting
    BELL_SLEEP_MS(2000);
  }
}

Player* s_player = nullptr;

// cspot::LoginBlob::loadJson() dereferences the JSON members without checking
// them, so make sure a (possibly corrupted) NVS value has the expected shape.
bool credentialsJsonLooksValid(const char* json) {
  cJSON* root = cJSON_Parse(json);
  if (root == nullptr) return false;
  const cJSON* type = cJSON_GetObjectItem(root, "authType");
  const cJSON* user = cJSON_GetObjectItem(root, "username");
  const cJSON* data = cJSON_GetObjectItem(root, "authData");
  bool ok = cJSON_IsNumber(type) && cJSON_IsString(user) && cJSON_IsString(data) &&
            data->valuestring[0] != '\0';
  cJSON_Delete(root);
  return ok;
}

}  // namespace

// ===========================================================================
// C API
// ===========================================================================
extern "C" {

esp_err_t spotify_connect_start(const spotify_connect_config_t* config) {
  if (config == nullptr || config->device_name == nullptr || config->device_name[0] == '\0' ||
      config->client_id == nullptr || config->client_id[0] == '\0' ||
      config->client_secret == nullptr || config->client_secret[0] == '\0') {
    ESP_LOGE(TAG, "device name, client id and client secret are required");
    return ESP_ERR_INVALID_ARG;
  }
  if (config->bitrate != SPOTIFY_BITRATE_96 && config->bitrate != SPOTIFY_BITRATE_160 &&
      config->bitrate != SPOTIFY_BITRATE_320) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_player != nullptr) return ESP_ERR_INVALID_STATE;

  // bell::Task allocates its stacks with MALLOC_CAP_SPIRAM and does not check
  // the result.
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    ESP_LOGE(TAG, "no PSRAM available: cspot cannot run");
    return ESP_ERR_NOT_SUPPORTED;
  }

  // Must exist before the first bell/cspot call: their log macros dereference
  // the global logger unconditionally.
  if (bell::bellGlobalLogger == nullptr) bell::bellGlobalLogger = new EspLogger();

  // We are still on the caller's task (internal-RAM stack): NVS is allowed here.
  std::string stored;
  char* json = nullptr;
  esp_err_t err = sc_credentials_load(&json);
  if (err == ESP_OK && json != nullptr) {
    if (credentialsJsonLooksValid(json)) {
      stored = json;
    } else {
      ESP_LOGW(TAG, "stored credentials are malformed, ignoring them");
    }
    free(json);
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "reading stored credentials: %s", esp_err_to_name(err));
  }

  auto* player = new Player(*config, std::move(stored));
  err = player->begin();
  if (err != ESP_OK) {
    // Tasks may already be running: do not delete the object.
    ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
    return err;
  }
  s_player = player;
  ESP_LOGI(TAG, "started, %d kbit/s", (int)config->bitrate);
  return ESP_OK;
}

static esp_err_t post(Cmd cmd, uint16_t arg = 0) {
  return s_player ? s_player->postCommand(cmd, arg) : ESP_ERR_INVALID_STATE;
}

esp_err_t spotify_connect_play(void) { return post(Cmd::Play); }
esp_err_t spotify_connect_pause(void) { return post(Cmd::Pause); }
esp_err_t spotify_connect_toggle(void) { return post(Cmd::Toggle); }
esp_err_t spotify_connect_next(void) { return post(Cmd::Next); }

esp_err_t spotify_connect_play_uri(const char* uri) {
  if (uri == nullptr || strncmp(uri, "spotify:", 8) != 0 || strlen(uri) > 100) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_player) return ESP_ERR_INVALID_STATE;
  return s_player->playUri(uri);
}
esp_err_t spotify_connect_prev(void) { return post(Cmd::Prev); }
esp_err_t spotify_connect_set_volume(uint16_t volume) { return post(Cmd::Volume, volume); }

void spotify_connect_get_status(spotify_connect_status_t* out) {
  if (out == nullptr) return;
  memset(out, 0, sizeof(*out));
  if (s_player) s_player->getStatus(out);
}

esp_err_t spotify_connect_forget_credentials(void) { return sc_credentials_erase(); }

}  // extern "C"
