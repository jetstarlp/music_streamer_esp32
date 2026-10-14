# Netz: WLAN, Einrichtungs-Hotspot, Updates, Uhrzeit

Stand: 13.10.2026. Komponente `firmware/components/net` (Branch `net`). Auf Hardware ist davon noch nichts gelaufen; was geprüft ist und was nicht, steht in Abschnitt 7.

## 1. Überblick

| Datei | Aufgabe |
|---|---|
| `include/net_wifi.h` | WLAN-Station mit bis zu 5 gespeicherten Netzen (ersetzt `main/wifi_sta.c`, gleiches Verhalten) |
| `include/net_portal.h` | Einrichtungs-Hotspot mit Captive-DNS und deutscher Webseite, Ereignisse für die App |
| `include/net_ota.h` | Updates über HTTPS-Manifest, SHA-256-Prüfung, Rollback |
| `include/net_time.h` | SNTP und Zeitzone Europe/Berlin |
| `src/pure/` | Reine C-Logik ohne ESP-IDF (DNS-Antwort, Manifest, Seiten, Versionen, QR-Text), PC-Tests in `tools/host_test_net.sh` |

Einstellungen: menuconfig → „Music Streamer network (net)“. Die Firmware-Version steht in `firmware/version.txt` (Semver, z. B. `1.0.0`).

## 2. WLAN

- Netze kommen aus `settings` (NVS, höchstens 5, zuletzt benutzt zuerst). Jeder Versuch durchsucht alle Kanäle und nimmt den stärksten Zugangspunkt (Mesh, Repeater), Treiber wiederholt 3×, Pausen nach einer erfolglosen Runde 1/3/5/10/15 s, Stromsparen aus. Unverändert gegenüber dem auf Hardware getesteten `wifi_sta.c`.
- Status für Anzeige und Konsole: `net_wifi_get_info()` (Zustand, SSID, RSSI, Kanal, IP, letzter Trennungsgrund); deutsche Fehlertexte: `net_wifi_reason_text_de()`.

## 3. Einrichtungs-Hotspot (Portal)

**Wann:** sofort, wenn kein Netz gespeichert ist; nach 60 s ohne Verbindung (nach dem Start oder nach Verbindungsverlust); auf Wunsch über `net_portal_start()` (UI-Menü, Konsole `wifi portal`).

**Ablauf für die Empfängerin:**
1. Display zeigt QR-Code `net_portal_get_qr_payload()` = `WIFI:T:nopass;S:Music-Streamer;;` und den Text `http://192.168.4.1/`.
2. Handy-Kamera auf den QR-Code → Handy verbindet sich mit dem offenen WLAN „Music-Streamer“. Android meldet „Im Netzwerk anmelden“, iPhone öffnet die Seite von selbst (Captive-Portal-Erkennung: jede DNS-Anfrage wird mit 192.168.4.1 beantwortet, `/generate_204`, `/hotspot-detect.html`, `/connecttest.txt` und alles andere leiten auf die Seite um; zusätzlich DHCP-Option 114).
3. Seite (schwarz/Messing): gefundene WLANs nach Signalstärke mit Schloss-Symbol, Feld für den Namen, Passwort, „Verbinden“.
4. Das Gerät probiert das Netz sofort (AP+STA). Erfolg: Netz wird gespeichert, Seite zeigt „Verbunden! … Das Gerät startet neu …“, 8 s später Neustart, Hotspot weg. Fehler: Formular kommt mit Hinweis zurück („Passwort falsch? …“, „WLAN nicht gefunden …“); gespeichert wird nur, was funktioniert hat.

**Weitere Regeln:** Der Hotspot läuft auf dem Kanal des stärksten gespeicherten (sonst des stärksten) WLANs, damit er beim Verbinden den Kanal nicht wechseln muss und das Handy nicht verliert. Solange ein Handy verbunden ist, sucht das Gerät gespeicherte Netze nur auf diesem Kanal. Ein automatisch gestarteter Hotspot geht aus, sobald ein gespeichertes Netz wieder da ist und kein Handy verbunden ist; ein angeforderter nach 15 min ohne Handy. Die Seite antwortet nur über den Hotspot, nie im Heimnetz. Kein Neustart, solange ein frisches Update auf Bestätigung wartet (er würde es zurückrollen) – dann geht nur der Hotspot aus.

**Für die App (`net_portal_set_event_cb`):** `STARTED`, `STOPPED`, `CLIENT_JOINED`, `CLIENT_LEFT`, `CREDENTIALS`, `CONNECT_OK`, `CONNECT_FAILED` (mit Grund), `RESTARTING` (Ausgang stummschalten). Rückrufe kommen aus fremden Tasks: nur kopieren, nicht blockieren.

**RAM:** nur während der Hotspot läuft, zwei Tasks (3,5 + 4 kB intern) und die AP-Schnittstelle; Seitenpuffer im PSRAM. Nicht während der Wiedergabe starten.

## 4. Updates (OTA)

**Ablauf im Gerät:** 2 min nach dem ersten Netzzugang und dann alle 24 h lädt das Gerät das Manifest (HTTPS, Zertifikatsprüfung mit dem ESP-IDF-Zertifikatsbündel). Ist die Version neuer (Semver-Vergleich mit `version.txt` der laufenden Firmware), meldet es nur „Update verfügbar“ (`NET_OTA_AVAILABLE` + Ereignis). Die UI fragt die Empfängerin und ruft `net_ota_install()`: Download in die freie OTA-Partition (Weiterleitungen werden verfolgt), SHA-256 des geschriebenen Abbilds gegen das Manifest, Projektname und Version im Abbild müssen passen, dann Neustart (`NET_OTA_REBOOTING` → App schaltet stumm). Fortschritt in Prozent über das Ereignis. Läuft Musik, warten geplante Prüfungen.

**Rollback:** Die neue Firmware startet „unbestätigt“. Die App muss `net_ota_mark_valid()` aufrufen, sobald WLAN und Spotify stehen (in `main/app_main.c`: Netz seit 30 s und Spotify angemeldet/wartet auf Anmeldung, spätestens nach 90 s Netz). Ohne Bestätigung innerhalb von 10 min, bei Absturz oder Neustart vorher startet der Bootloader wieder die alte Firmware; die zurückgerollte Version wird gemerkt und angezeigt. Wirkt erst, wenn der neue Bootloader einmal per USB geflasht wurde (OTA tauscht den Bootloader nie).

**Konsole:** `ota` (Status), `ota check`, `ota install`, `ota url <https-URL>` / `ota url default`.

## 5. Update veröffentlichen

1. `firmware/version.txt` erhöhen (z. B. `1.0.1`), committen.
2. `tools/build_dist.sh` (baut `build-waveshare`).
3. `python tools/make_ota_release.py --base-url <URL> --notes "Kurzer Text"` schreibt `dist/ota/ms-waveshare-<version>.bin` (nur das App-Abbild, nicht das USB-Gesamtabbild) und `dist/ota/manifest.json`:
   ```json
   {"version": "1.0.1",
    "url": "https://github.com/<user>/<repo>/releases/download/v1.0.1/ms-waveshare-1.0.1.bin",
    "sha256": "<64 Hex-Zeichen>", "size": 1645376, "notes": "Kurzer Text"}
   ```
   `sha256` ist der SHA-256 der .bin-Datei, wie sie ausgeliefert wird. Von Hand: `sha256sum ms-waveshare-1.0.1.bin` (Linux), `Get-FileHash ms-waveshare-1.0.1.bin` (PowerShell, Kleinschreibung egal). Optional `"min_version"`: ältere Geräte installieren dann nicht.
4. Hochladen, eine der zwei Varianten (Repository muss öffentlich sein; die Firmware enthält keine Geheimnisse):
   - **GitHub Release** (empfohlen): Release mit Tag `v1.0.1` anlegen, beide Dateien als Assets anhängen; `--base-url https://github.com/<user>/<repo>/releases/download/v1.0.1`. Manifest-URL im Gerät: `https://github.com/<user>/<repo>/releases/latest/download/manifest.json` (zeigt immer auf das neueste Release).
   - **GitHub Pages:** beide Dateien nach `<repo>/ms-waveshare/` (Pages-Branch); `--base-url https://<user>.github.io/<repo>/ms-waveshare`, Manifest-URL `https://<user>.github.io/<repo>/ms-waveshare/manifest.json`.
5. Die Manifest-URL steht in `sdkconfig.defaults` (`CONFIG_MS_OTA_MANIFEST_URL`): `https://github.com/jetstarlp/music_streamer_esp32/releases/latest/download/manifest.json`. Das Repository darf deshalb nicht umbenannt oder privat gemacht werden. Pro Gerät überschreibbar mit `ota url …` auf der Konsole.

## 6. Uhrzeit

`net_time_start()` setzt `TZ=CET-1CEST,M3.5.0,M10.5.0/3` und startet SNTP (`pool.ntp.org`); `main` ruft es auf, sobald das Netz steht. `net_time_is_set()` sagt, ob die Uhr gestellt ist.

## 7. Tests und was nicht getestet ist

- PC: `tools/host_test_net.sh` (Semver, Manifest, DNS-Antwort, Formulardaten, URL-Auflösung, Seiten-HTML, QR-Text, Kanalwahl). `NET_PAGES_OUT=<ordner>` schreibt die Seiten als HTML zum Ansehen.
- Emulator: `python tools/qemu_net_test.py` (baut `build-qemu`, ca. 5 min) – Portal über die emulierte Ethernet-Schnittstelle (DNS beantwortet die Prüf-Adressen von Android/iOS/Windows mit 192.168.4.1, Umleitungen, Formular, deutsche Fehlermeldungen, Stopp) und OTA über HTTPS mit Test-CA (`components/net/test`, nur im QEMU-Build vertraut): Weiterleitungen wie bei GitHub (`latest` → Release, Asset → lange signierte URL), falscher SHA-256 abgelehnt, 1,3 MB in 17 s geladen, Wechsel auf 1.0.1 in die zweite Partition, Bestätigung, Rollback eines unbestätigten 1.0.2 nach Neustart. Die neueren Abbilder sind derselbe Build mit anderer Versionsnummer (`tools/ota_set_version.py`). Stack des OTA-Tasks: ca. 4,5 von 8 kB benutzt.
- ESP-IDF 5.5.5 folgt keinen relativen `Location`-Angaben bei HTTPS (Fehler „HTTPS origin can only redirect to https:// targets“); GitHub antwortet mit absoluten URLs, ein eigener Server muss das auch tun.
- **Nicht getestet (keine Hardware):** echter WLAN-Hotspot (AP+STA, Kanalwahl, Verhalten echter Handys bei Captive-Portal und Kanalwechsel), Verbinden mit dem eingegebenen Netz, Hintergrundsuche auf dem Hotspot-Kanal, HTTPS gegen GitHub mit dem Zertifikatsbündel, Download-Dauer und RAM-Bedarf von OTA neben laufendem Spotify, Rollback mit echtem Bootloader im Flash, SNTP.
- Grenzen: Der Hotspot belegt Port 80 mit einem eigenen HTTP-Server; läuft später ein gemeinsamer HTTP-Server (Spotify-Login `/login`, `/cb`) ebenfalls auf Port 80, müssen beide zusammengelegt werden. Nur 2,4-GHz-WLANs.
