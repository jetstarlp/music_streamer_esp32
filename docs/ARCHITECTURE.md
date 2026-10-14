# Architektur

Stand: 07.10.2026. Verbindlich für alle weiteren Arbeitspakete. Was schon existiert und baut, ist als **vorhanden** markiert; alles andere ist Vorgabe für die Umsetzung. Auf Hardware gelaufen ist noch nichts.

## 1. Überblick

```
 Handy (Spotify-App) ──Connect──┐                    ┌── Spotify Web API (HTTPS)
                                ▼                    ▼
 NFC-Tag ─▶ nfc ─▶ app (Zustandsautomat, Ereignisbus) ─▶ spotify_api
                    ▲   │            │
        Touch ─▶ ui ┘   │            └▶ spotify_connect (cspot) ─▶ audio_out ─▶ PCM5102A ─▶ AUX
                        ▼
                  net (WLAN, Portal, OTA)   settings (NVS)
```

Grundsatz: Komponenten kennen einander nicht. Sie melden Ereignisse an `app` und bekommen von dort Befehle. Nur `spotify_connect` schreibt direkt in `audio_out`.

## 2. Komponenten

| Komponente | Aufgabe | Stand |
|---|---|---|
| `board` | `board_pins.h`: einzige Stelle für GPIO-Nummern | vorhanden |
| `audio_out` | I2S-Ausgabe 44,1 kHz/16 Bit, Ringpuffer, Mute-Ablauf über XSMT | vorhanden, ungetestet |
| `spotify_connect` | cspot-Hülle: Anmeldung, 320 kbps, Ereignisse, lokale Steuerung | vorhanden, ungetestet |
| `settings` | Typsichere Einstellungen in NVS (Sprache, Helligkeit, Tag-Verhalten, Netze) | offen |
| `net` | WLAN mit bis zu 5 Netzen, Setup-Hotspot mit Portal, SNTP, OTA | offen (`wifi_sta.c` in `main` ist Übergang) |
| `spotify_api` | Web API: PKCE-Login, Token-Erneuerung, Wiedergabe starten, Playlists, Cover laden | offen |
| `nfc` | PN532 über UART, Tag-Anwesenheit, NDEF lesen/schreiben | offen |
| `nfc_tag` | Reiner C-Code ohne Hardware: NDEF-TLV und Spotify-Link parsen/erzeugen, mit PC-Tests | offen |
| `display` | CO5300 + FT3168 + LVGL-Port, Helligkeit, Einbrennschutz | offen |
| `ui` | LVGL-Bildschirme; hängt nur an `lvgl.h` und `ui_model.h`, läuft auch im PC-Simulator | offen |
| `app` (in `main`) | Zustandsautomat, Ereignisbus, Regeln (Tag aufgelegt → abspielen) | offen |

## 3. Verbindliche Schnittstellen

Vorhanden und unverändert zu nutzen: `spotify_connect.h`, `audio_out.h`, `board_pins.h`.

Neu anzulegen (Signaturen sind Vorgabe, Ergänzungen erlaubt, Umbenennungen nicht ohne Absprache):

```c
/* nfc_tag.h – ohne Hardware, auf dem PC testbar */
typedef enum { SPOTIFY_REF_TRACK, SPOTIFY_REF_ALBUM, SPOTIFY_REF_PLAYLIST,
               SPOTIFY_REF_ARTIST, SPOTIFY_REF_SHOW, SPOTIFY_REF_EPISODE } spotify_ref_type_t;
typedef struct { spotify_ref_type_t type; char id[23]; } spotify_ref_t;   /* 22 Zeichen Base62 */
bool   nfc_tag_parse_ndef(const uint8_t *mem, size_t len, spotify_ref_t *out);   /* Type-2-Speicher ab Seite 4 */
bool   nfc_tag_parse_link(const char *text, spotify_ref_t *out);                 /* https-Link oder spotify:-URI */
size_t nfc_tag_build_ndef(const spotify_ref_t *ref, uint8_t *out, size_t cap);   /* 0 bei Fehler */
void   spotify_ref_to_uri(const spotify_ref_t *ref, char *out, size_t cap);      /* spotify:playlist:<id> */

/* nfc.h */
typedef enum { NFC_EVT_TAG_PRESENT, NFC_EVT_TAG_REMOVED, NFC_EVT_READER_ERROR } nfc_event_id_t;
typedef struct { nfc_event_id_t id; uint8_t uid[7]; uint8_t uid_len;
                 bool has_ref; spotify_ref_t ref; } nfc_event_t;
esp_err_t nfc_start(void (*cb)(const nfc_event_t *, void *), void *ctx);
esp_err_t nfc_write_ref(const spotify_ref_t *ref, uint32_t timeout_ms);   /* wartet auf Tag, schreibt, liest zurück */
void      nfc_set_poll_interval_ms(uint32_t ms);                           /* 300 im Leerlauf, 750 bei Wiedergabe */

/* spotify_api.h */
esp_err_t spotify_api_init(const char *client_id, const char *redirect_uri);
bool      spotify_api_is_linked(void);
esp_err_t spotify_api_login_url(char *out, size_t cap);          /* für den QR-Code */
esp_err_t spotify_api_login_finish(const char *code, const char *state);
int32_t   spotify_api_days_until_relink(void);                   /* Refresh-Token: 6 Monate */
esp_err_t spotify_api_play(const spotify_ref_t *ref, const char *device_name, bool shuffle);
esp_err_t spotify_api_fetch_cover(const char *url, uint8_t **jpeg, size_t *len);   /* Puffer im PSRAM */
esp_err_t spotify_api_list_playlists(/* seitenweise, Rückruf je Eintrag */);

/* ui_model.h – einzige Verbindung zwischen ui und dem Rest */
typedef struct { char title[96]; char artist[96]; char context[64];
                 uint32_t duration_ms, position_ms; bool playing, tag_present, wifi_ok, linked;
                 const uint16_t *cover_rgb565; uint16_t cover_w, cover_h; } ui_model_t;
void ui_model_publish(const ui_model_t *m);      /* app → ui, kopiert unter Sperre */
typedef enum { UI_CMD_TOGGLE, UI_CMD_NEXT, UI_CMD_PREV, UI_CMD_SHUFFLE,
               UI_CMD_WRITE_TAG, UI_CMD_OPEN_SETUP } ui_cmd_t;
void ui_set_command_handler(void (*cb)(ui_cmd_t, void *), void *ctx);   /* ui → app */
```

## 4. Abläufe

**Tag aufgelegt:** `nfc` meldet `TAG_PRESENT` mit Verweis → `app` prüft Einstellung des Tags (NVS, nach UID) → `spotify_api_play()` mit dem Gerätenamen → cspot bekommt die Wiedergabe von Spotify zugewiesen → `spotify_connect` meldet `TRACK_CHANGED` → `app` lädt Cover, füllt `ui_model`.

**Tag abgenommen:** `TAG_REMOVED` (nach 3 Fehlabfragen) → je nach Einstellung `spotify_connect_pause()`, weiterspielen oder nach Titelende stoppen.

**Zustand der Wiedergabe** kommt immer aus `spotify_connect`, nie durch Abfragen der Web API. Die Web API wird nur bei Ereignissen angesprochen (Tag, Playlist-Wahl, Cover).

**Spotify-Login:** Gerät zeigt QR-Code auf `http://<geräte-ip>/login` → Weiterleitung zu Spotify → Spotify leitet auf die HTTPS-Weiterleitungsseite → diese schickt den Code an `http://<geräte-ip>/cb` → `spotify_api_login_finish()`.

## 5. Tasks, Prioritäten, Speicher

| Task | Kern | Priorität | Stack |
|---|---|---|---|
| I2S-Zubringer (`audio_out`) | 1 | hoch (über allem außer WLAN) | intern |
| cspot-Tasks | 1 | mittel | PSRAM (bell) |
| `app` | 0 | mittel | intern, 6 kB |
| LVGL | 0 | niedrig, unter Audio | intern, 8 kB |
| `nfc` | 0 | niedrig | intern, 4 kB |
| HTTP-Arbeiten (`spotify_api`, Cover, OTA) | 0 | niedrig | intern, 8 kB |

Regeln:
- Interner RAM ist die knappe Ressource. Alles über 4 kB kommt ins PSRAM, außer LVGL-Zeichenpuffer (2 × 280×56 Pixel) und DMA-Puffer.
- Freien internen Heap und Tiefststand alle 10 s loggen (vorhanden in `app_main.c`). Untergrenze im Betrieb: 60 kB.
- Kein Task blockiert länger als 100 ms in einem Rückruf einer anderen Komponente.
- Fehlerfälle enden nie in `abort()`: Netzfehler führen zu Neuversuch mit Wartezeit.

## 6. Geheimnisse und Konfiguration

- WLAN-Daten und Spotify-Client-ID/-Secret stehen während der Entwicklung in `firmware/sdkconfig.secrets` (nicht in Git).
- Im fertigen Gerät liegen sie im NVS; das Firmware-Image enthält kein Secret, damit es für OTA öffentlich abgelegt werden kann.
- Lizenz: Durch cspot steht die gesamte Firmware unter GPL-3.0.

## 7. Arbeitspakete für die parallele Umsetzung

Jedes Paket arbeitet nur in seinem Ordner und gegen die Schnittstellen aus Abschnitt 3.

| Paket | Ordner | Ohne Hardware prüfbar durch |
|---|---|---|
| P1 Tag-Format | `components/nfc_tag`, `tests/host` | PC-Tests mit echten NTAG215-Speicherabbildern |
| P2 NFC-Dienst | `components/nfc` | Build; PN532-Rahmen gegen aufgezeichnete Antworten |
| P3 Web API | `components/spotify_api`, `relay/` | PC-Tests für PKCE und JSON; Build |
| P4 Anzeige | `components/display` | Build |
| P5 Oberfläche | `components/ui`, `simulator/` | PC-Simulator mit Bildschirmfotos |
| P6 Netz | `components/net`, `components/settings` | Build; Portal im Emulator |
| P7 Ablaufsteuerung | `main` | Build; Emulator-Test |

Abnahme je Paket: sauberer Build aus frischem Klon ohne Warnungen, Tests grün, ein Absatz in `docs/` zu dem, was nicht getestet werden konnte.
