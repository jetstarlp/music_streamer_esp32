/*
 * app_ui - the application state machine behind the touch UI: turns events of
 * Spotify Connect, the NFC reader, the setup portal and the OTA updater into
 * the ui_model (components/ui) and executes the commands of the UI.
 *
 * All event functions only copy the event into a queue and return; the work
 * happens on the "app_ctl" task. They may be called before app_ui_start()
 * (events are then dropped) and on boards without a display (the rules for
 * NFC still apply, only nothing is drawn).
 */
#pragma once

#include "net_ota.h"
#include "net_portal.h"
#include "nfc.h"
#include "spotify_connect.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Display + UI (if the board has a display) and the controller task. Call
 *  after settings_init(), before WiFi and Spotify are started. */
void app_ui_start(void);

void app_ui_spotify_event(const spotify_event_t *ev);
void app_ui_nfc_event(const nfc_event_t *ev);
void app_ui_portal_event(const net_portal_event_t *ev);
void app_ui_ota_event(const net_ota_status_t *st);

#ifdef __cplusplus
}
#endif
