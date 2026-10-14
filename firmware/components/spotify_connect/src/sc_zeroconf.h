/*
 * Spotify Connect "Zeroconf" login endpoint.
 *
 *  - mDNS service _spotify-connect._tcp so that the Spotify app (same LAN)
 *    lists the device,
 *  - HTTP GET  <CPath>?action=getInfo -> device description + DH public key,
 *  - HTTP POST <CPath> (action=addUser) -> encrypted login blob of the user.
 *
 * See https://developer.spotify.com/documentation/commercial-hardware/implementation/guides/zeroconf
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "esp_err.h"

namespace cspot {
class LoginBlob;
}

namespace sc {

class Zeroconf {
 public:
  /**
   * Start the HTTP endpoint on `port` and announce it.
   *
   * @param blob     login blob of the session; receives the user's credentials
   * @param onLogin  called (on the HTTP server task, internal-RAM stack) after
   *                 a login blob was received and decoded into `blob`
   */
  esp_err_t start(std::shared_ptr<cspot::LoginBlob> blob, uint16_t port,
                  std::function<void()> onLogin);

  /** Withdraw the mDNS service and stop the HTTP server. Idempotent. */
  void stop();

  bool isRunning() const { return server_ != nullptr; }

  /* used by the C request handlers */
  std::shared_ptr<cspot::LoginBlob> blob() const { return blob_; }
  void notifyLogin() const {
    if (onLogin_) onLogin_();
  }

 private:
  void* server_ = nullptr;  // httpd_handle_t
  std::shared_ptr<cspot::LoginBlob> blob_;
  std::function<void()> onLogin_;
  bool serviceAdded_ = false;
};

}  // namespace sc
