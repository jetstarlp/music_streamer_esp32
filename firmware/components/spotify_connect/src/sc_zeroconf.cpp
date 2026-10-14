#include "sc_zeroconf.h"

#include <cctype>
#include <cstring>
#include <exception>
#include <map>
#include <string>

#include "LoginBlob.h"  // cspot

#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"

static const char* TAG = "sc_zeroconf";

namespace {

constexpr const char* kPath = "/spotify_info";
constexpr const char* kServiceType = "_spotify-connect";
constexpr const char* kServiceProto = "_tcp";
constexpr size_t kMaxPostBody = 8 * 1024;  // real requests are 1..2 kB

// application/x-www-form-urlencoded value -> raw bytes
std::string urlDecode(const char* s, size_t len) {
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < len && isxdigit((unsigned char)s[i + 1]) &&
               isxdigit((unsigned char)s[i + 2])) {
      const char hex[3] = {s[i + 1], s[i + 2], '\0'};
      out.push_back((char)strtol(hex, nullptr, 16));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::map<std::string, std::string> parseForm(const std::string& body) {
  std::map<std::string, std::string> result;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t amp = body.find('&', pos);
    if (amp == std::string::npos) amp = body.size();
    size_t eq = body.find('=', pos);
    if (eq != std::string::npos && eq < amp) {
      result[urlDecode(body.data() + pos, eq - pos)] =
          urlDecode(body.data() + eq + 1, amp - eq - 1);
    }
    pos = amp + 1;
  }
  return result;
}

esp_err_t handleGet(httpd_req_t* req) {
  auto* self = static_cast<sc::Zeroconf*>(req->user_ctx);
  std::string body;
  try {
    body = self->blob()->buildZeroconfInfo();
  } catch (const std::exception& e) {
    ESP_LOGE(TAG, "getInfo failed: %s", e.what());
  }
  if (body.empty()) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no info");
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t handlePost(httpd_req_t* req) {
  auto* self = static_cast<sc::Zeroconf*>(req->user_ctx);

  if (req->content_len == 0 || req->content_len > kMaxPostBody) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
  }
  std::string body(req->content_len, '\0');
  size_t received = 0;
  while (received < body.size()) {
    int n = httpd_req_recv(req, body.data() + received, body.size() - received);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return ESP_FAIL;  // connection closed/failed: httpd cleans up
    received += n;
  }

  bool ok = false;
  try {
    auto form = parseForm(body);
    // cspot slices the decoded blob without bounds checks (IV 16 bytes, MAC 20
    // bytes, DH key 96 bytes): reject anything that is obviously too short
    // before it gets there. Sizes are those of the base64 text.
    if (form.count("blob") && form.count("clientKey") && form.count("userName") &&
        form["blob"].size() >= 64 && form["clientKey"].size() >= 128) {
      // Decrypts the blob with the DH shared secret and fills blob->authData.
      self->blob()->loadZeroconfQuery(form);
      ok = true;
    } else {
      ESP_LOGW(TAG, "addUser: blob/clientKey/userName missing or too short");
    }
  } catch (const std::exception& e) {
    ESP_LOGE(TAG, "addUser failed: %s", e.what());
  }

  // Status codes as in Spotify's Zeroconf API: 101 = OK, 202 = login failed.
  static const char kOk[] = "{\"status\":101,\"statusString\":\"OK\",\"spotifyError\":0}";
  static const char kFail[] =
      "{\"status\":202,\"statusString\":\"ERROR-LOGIN-FAILED\",\"spotifyError\":0}";
  httpd_resp_set_type(req, "application/json");
  esp_err_t err = ok ? httpd_resp_send(req, kOk, sizeof(kOk) - 1)
                     : httpd_resp_send(req, kFail, sizeof(kFail) - 1);
  if (ok) {
    ESP_LOGI(TAG, "login blob received");
    self->notifyLogin();
  }
  return err;
}

// mDNS host names: letters, digits, '-'
std::string makeHostname(const std::string& deviceName) {
  std::string host;
  for (char c : deviceName) {
    if (isalnum((unsigned char)c)) {
      host.push_back((char)tolower((unsigned char)c));
    } else if (!host.empty() && host.back() != '-') {
      host.push_back('-');
    }
  }
  while (!host.empty() && host.back() == '-') host.pop_back();
  return host.empty() ? "music-streamer" : host;
}

}  // namespace

namespace sc {

esp_err_t Zeroconf::start(std::shared_ptr<cspot::LoginBlob> blob, uint16_t port,
                          std::function<void()> onLogin) {
  if (server_ != nullptr) return ESP_ERR_INVALID_STATE;
  blob_ = std::move(blob);
  onLogin_ = std::move(onLogin);

  // --- HTTP endpoint ---------------------------------------------------------
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port;
  // Each httpd instance needs its own control port; leave the default (32768)
  // to the application's main web server.
  cfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
  // loadZeroconfQuery() runs DH + PBKDF2 + AES on this task.
  cfg.stack_size = 8192;
  cfg.max_open_sockets = 3;
  cfg.lru_purge_enable = true;
  cfg.max_uri_handlers = 2;

  httpd_handle_t server = nullptr;
  esp_err_t err = httpd_start(&server, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start(port %u): %s", (unsigned)port, esp_err_to_name(err));
    return err;
  }
  httpd_uri_t uri = {};
  uri.uri = kPath;
  uri.user_ctx = this;
  uri.method = HTTP_GET;
  uri.handler = handleGet;
  httpd_register_uri_handler(server, &uri);
  uri.method = HTTP_POST;
  uri.handler = handlePost;
  httpd_register_uri_handler(server, &uri);
  server_ = server;

  // --- mDNS announcement -------------------------------------------------------
  // mdns_init() is a no-op when another module already initialised mDNS. Only
  // set a host name if nobody has done so (services cannot be added without).
  err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
    stop();
    return err;
  }
  char hostname[MDNS_NAME_BUF_LEN] = "";
  if (mdns_hostname_get(hostname) != ESP_OK || hostname[0] == '\0') {
    mdns_hostname_set(makeHostname(blob_->getDeviceName()).c_str());
    mdns_instance_name_set(blob_->getDeviceName().c_str());
  }

  mdns_txt_item_t txt[] = {
      {"VERSION", "1.0"},
      {"CPath", kPath},
      {"Stack", "SP"},
  };
  err = mdns_service_add(blob_->getDeviceName().c_str(), kServiceType, kServiceProto, port, txt,
                         sizeof(txt) / sizeof(txt[0]));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_service_add: %s", esp_err_to_name(err));
    stop();
    return err;
  }
  serviceAdded_ = true;

  ESP_LOGI(TAG, "advertising \"%s\" (%s.%s, port %u)", blob_->getDeviceName().c_str(), kServiceType,
           kServiceProto, (unsigned)port);
  return ESP_OK;
}

void Zeroconf::stop() {
  if (serviceAdded_) {
    mdns_service_remove(kServiceType, kServiceProto);
    serviceAdded_ = false;
  }
  if (server_ != nullptr) {
    httpd_stop(static_cast<httpd_handle_t>(server_));
    server_ = nullptr;
  }
}

}  // namespace sc
