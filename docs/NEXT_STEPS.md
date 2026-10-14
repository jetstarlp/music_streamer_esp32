# Übergabe und nächste Schritte

Stand: 12.10.2026. Zweck: Mit dieser Datei kann die Arbeit in einer neuen Sitzung ohne Vorwissen fortgesetzt werden. Verbindlich bleiben `docs/ARCHITECTURE.md` (Schnittstellen) und `docs/PLAN.md` (Entscheidungen).

## 1. Stand des Codes (12.10.2026)

| Branch | Inhalt | Zustand |
|---|---|---|
| `main` | Gerüst (`board`, `settings`, `audio_out`, `spotify_connect`), **Test-Firmware** (Konsole, Testton, WLAN aus NVS, Status-LED, Images für Waveshare und XIAO) und **Display** (CO5300, Touch, LVGL 9.3 quer, Test-App) | Baut ohne Warnungen; Host-Tests und Emulator-Konsolentest grün; auf Hardware ungetestet |
| `nfc` | `nfc_tag.h` (Schnittstelle) | **WIP**, nur Header |
| `spotify-api` | Schnittstelle, PKCE, Kodierung, JSON-Hilfen, Spotify-Verweise | **WIP**, ungebaut, ohne Tests |
| `net` | Reine Logik: Netzauswahl, Versionsvergleich, WLAN-QR | **WIP**, ungebaut, ohne Tests |
| `ui` | – | nicht begonnen |

Beim Nutzer zum Testen (Ordner `test-firmware`): `ms-waveshare-a3e6f87.bin`, `ms-bench-xiao-a3e6f87.bin` mit `BENCH_TEST.md`; `ms-display-test.bin` mit `DISPLAY.md`; Waveshare-Werksfirmware zum Vergleich. Rückmeldungen dazu bestimmen die nächsten Korrekturen.

Bauen der Images: `firmware/tools/build_dist.sh` (beide Boards), Display-Test: `idf.py build` in `firmware/components/display/test_app`. Tests: `firmware/tools/host_test_bench.sh`, `firmware/tools/qemu_smoke_test.sh`, `python firmware/tools/qemu_console_test.py`.

## 2. Entscheidungen der letzten Runde (09.10.)

- **UI-Richtung C** (Querformat 456 × 280, Messing-Leiste unten). Sorge: Bedienung per Touch auf dem kleinen Display. Gegenmaßnahmen: wirksame Trefferflächen ≥ 76 × 76 px (≈ 6 mm), Wischen links/rechts = Titel weiter/zurück, Tippen aufs Cover = Play/Pause, langes Drücken = Menü, nichts Bedienbares näher als 8 px am Rand. Handy und NFC bleiben die Hauptbedienung.
- **ESP-IDF installiert der Nutzer selbst**: ESP-IDF Installation Manager (`eim-gui-windows-x64.exe`, 34,2 MB, von https://dl.espressif.com/dl/eim/), benutzerdefinierte Installation, v5.5.5, Ziel esp32s3, Pfad `C:\esp`.
- **Testboard bis zum Waveshare: Seeed XIAO ESP32-S3 Plus** (ESP32-S3R8, 8 MB Octal-PSRAM, 16 MB Flash). DAC und PN532 sind beim Nutzer angekommen, der Waveshare noch nicht.
- Raspberry Pi als Ausweich-Hardware: unbekannt, Nutzer schaut nach.
- Keine Bildschirmsteuerung und keine Downloads auf dem PC ohne ausdrückliche Zustimmung.

## 3. Verdrahtung XIAO ESP32-S3 Plus (in `board_pins.h`, Profil `MS_BOARD_XIAO_ESP32S3_PLUS`)

| XIAO | GPIO | Ziel |
|---|---|---|
| D4 | 5 | PCM5102A BCK |
| D5 | 6 | PCM5102A LCK |
| D8 | 7 | PCM5102A DIN |
| D9 | 8 | PCM5102A XSMT (10 kΩ nach GND) |
| D10 | 9 | PN532 SCL/RX (HSU) |
| D0 | 1 | PN532 SDA/TX (HSU) |
| D1 | 2 | PN532 RSTPD_N (Pad auf der Rückseite) |
| 3V3 / GND | – | Versorgung DAC und PN532 |

PN532: DIP-Schalter beide OFF (HSU). DAC-Lötbrücken: SCK → GND, FLT/DEMP/FMT = L.

## 4. Nächste Arbeitspakete

Parallel möglich, jedes auf eigenem Branch, nur im eigenen Ordner; Änderungen an gemeinsamen Dateien (`main`, `board`, `settings`, `sdkconfig.defaults`, `partitions.csv`) nur als Vorschlag im Bericht. Abnahme: sauberer Build ohne Warnungen, Host-Tests grün, Liste des Ungetesteten in `docs/`.

1. **bench** – erledigt am 12.10. (siehe Abschnitt 1). Ursprüngliche Vorgabe: Profil `sdkconfig.xiao` über `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.xiao"`; Konsole über USB-Serial-JTAG mit `wifi add|list|del`, `spotify creds` (Secret nie ausgeben), `name`, `tone [hz] [dbfs] [s]`, `tone noise-test` (1 kHz, −60 dBFS, 60 s), `status`, `reboot`, `factory-reset`; Status-LED GPIO21 (aktiv low); Spotify nur mit gespeicherten Zugangsdaten; zusammengeführte Images (`esptool merge_bin`, Offset 0x0) für Flashen per https://espressif.github.io/esptool-js/; Emulator-Test; `docs/BENCH_TEST.md` mit Verdrahtung, Flashen, Testablauf (Ton, Pegel 0 dBFS bei 60 Hz/1 kHz, Masseschleifen-Test Lautsprecher-USB gegen Powerbank, Spotify 15 min).
2. **nfc**: `nfc_tag` als reines C mit Host-Tests (TLV, NDEF kurz/lang, URI-Präfixe, Text-Datensatz, `open.spotify.com/[intl-xx/]<typ>/<id>`, `spotify:`-URI, `spotify.link` ablehnen, Fuzzing); `nfc` als eigener PN532-HSU-Treiber (Rahmen als reine Funktionen getestet, Wake-up, SAMConfiguration, RF aus zwischen Abfragen, 300 ms, Entfernen nach 3 Fehlabfragen, Neustart über RSTPD_N nach Fehlerserie, sicheres Schreiben mit Rücklesen); Test-App für beide Boards; `docs/NFC.md`.
3. **spotify-api**: auf dem WIP-Stand aufbauen. Fehlend: HTTP-Client (esp_http_client + Zertifikatsbündel), Token in NVS-Namensraum `sp_api` (neuen Refresh-Token zuerst speichern), Wiedergabe per Gerätename mit Übergabe und einem Neuversuch, Retry-After beachten, Cover ≤ 256 kB ins PSRAM, `/login` und `/cb` am gemeinsamen HTTP-Server, Host-Tests (RFC-7636-Testvektor), `relay/index.html` (nur private IPv4, keine externen Skripte) mit Node-Test, `docs/SPOTIFY_LOGIN.md`.
4. **display** – erledigt am 12.10. (Touch direkt statt über `esp_lcd_touch_ft5x06`). Ursprüngliche Vorgabe: CO5300 über `esp_lcd_co5300` (≥ 2.2.0) mit Waveshare-Init-Tabelle, Versatz 20, Rounder; Touch FT3168 über `esp_lcd_touch_ft5x06` mit dem neuen I2C-Treiber; `esp_lvgl_port`; LVGL **v9.3.0**; Querformat per Hardware-Drehung, Software-Drehung als Ausweich; API `display_init`, `display_set_brightness`, `display_set_on` (kein Deep-Sleep wegen gemeinsamem Reset), `display_lock/unlock`, `display_idle_ms`, `display_i2c_bus`; `sdkconfig.lvgl`; Test-App mit Testmuster; `docs/DISPLAY.md`.
5. **ui**: Richtung C mit LVGL v9.3.0, Barlow-Schriften (OFL) per `lv_font_conv`, eigene Icons, Bildschirme Wiedergabe, Leerlauf, „Tag erkannt“, WLAN-QR, Spotify-QR, Menü, Tag beschreiben, Hinweisbanner, Bildschirmschoner; Gesten wie in Abschnitt 2; PC-Simulator mit PNG-Bildschirmfotos und Übersichtsbild; `docs/UI.md`.
6. **net**: auf dem WIP-Stand aufbauen. Bis zu 5 Netze, Auswahl nach Signalstärke, Neuverbindung mit Wartezeit; Setup-Hotspot mit Captive-DNS und deutschen Seiten; gemeinsamer HTTP-Server; SNTP; ein gemeinsames mDNS mit `spotify_connect`; OTA per Manifest von GitHub plus Upload-Seite mit PIN, Rollback; Host-Tests; `docs/NETZ.md`.
7. **app** (danach): Zustandsautomat in `main` gemäß ARCHITECTURE §4, alle Komponenten verbinden, OTA-Rollback bestätigen.

Hinweise für die Umgebung in der Cloud: ESP-IDF unter `/home/claude/esp` (geht beim Zurücksetzen verloren; Neuinstallation siehe `docs/BUILD.md` Abschnitt 9). Gesperrt sind `dl.espressif.com` und die ESP Component Registry; Komponenten daher als `git:`-Abhängigkeiten holen.

## 5. Offene Aufgaben beim Nutzer

- ESP-IDF v5.5.5 installieren (siehe Abschnitt 2) und `git config --global core.longpaths true`.
- Repository aus `music-streamer.bundle` nach `C:\dev\music-streamer` klonen, Submodule laut `docs/BUILD.md` Abschnitt 3.
- Spotify-Entwickler-App anlegen (Redirect-URI vorerst `http://127.0.0.1:8888/callback`, „Web API“), Client-ID und Secret nur lokal ablegen.
- Teile bestellen: 3,5-mm-Kabel, kurzes USB-C-Kabel, LiPo-Vorschlag (EREMIT 2500 mAh, JST-PH auf MX1.25 umlöten), isolierter DC/DC 5 V→5 V als Vorrat.
- Nachsehen, ob ein Raspberry Pi vorhanden ist.

## Nachtrag 13.10.2026 (Bench-Test Audio)

- Ab jetzt nur noch Waveshare: `tools/build_dist.sh` baut das XIAO-Image nur mit `--xiao`.
- Audio: DMA-Puffer werden im I2S-Interrupt befüllt (`pcm_ring`, `pcm_engine`); Seek knackfrei bestätigt (Image 0675947).
- NVS liegt jetzt am Ende des Flash (0xFF0000): Einstellungen überleben das Flashen des Gesamt-Images. Beim Wechsel auf ffeb856 einmalig neu eingeben.
- Overlays 5/6: Skip aus der App leert den Puffer sofort; abgelehnte Audio-Keys werden 3× wiederholt.
- Offen: 10-Minuten-Dauertest, schnelles Skippen erneut prüfen (Sync App ↔ Gerät).
- QEMU-Konsolentests sind gelegentlich beim ersten Lauf nach einem Build flakey (erster Befehl ohne Antwort); zweiter Lauf besteht.

## Nachtrag 13.10.2026 abends: Grenzen von cspot (Spirc-Protokoll)

- Spotify drosselt Audio-Schlüssel nach schnellem Skippen: 20–30 s lang wird *jeder* Schlüssel abgelehnt (Code 00 02). Firmware wartet jetzt mit globalem Back-off statt die Playlist durchzuskippen (Overlay 6).
- Die App schickt in ihren Load-Frames Listen, deren Teil *vor* dem aktuellen Song nicht in Playlist-Reihenfolge ist (81/82/84/85 Einträge, Reihenfolge wechselt; belegt mit Track-IDs im Log 9d9e981). „Zurück“ aus der App landet deshalb ab und zu beim falschen Song. Das Gerät spielt korrekt list[index]; beheben lässt sich das nur mit dem neuen Connect-Protokoll (Dealer/Connect-State, wie librespot ≥ 0.7) – großer Umbau, offen zur Entscheidung.
