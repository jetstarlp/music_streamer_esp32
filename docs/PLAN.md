# Music Streamer – Entscheidungen und Projektplan

Stand: 07.10.2026. Grundlage: Schaltplan Rev 1.1, Datenblätter im Ordner `docs/`, sechs Recherche-Berichte, ein Build-Test. Auf Hardware und gegen Spotify ist noch nichts gelaufen; ungeprüfte Punkte sind als **offen** markiert.

## 0. Stand

- **Build-Test bestanden:** Der cspot-Fork kompiliert und linkt für den ESP32-S3 mit ESP-IDF v5.5.5. Firmware-Gerüst 1,42 MB (27 % der 5-MB-Partition), zweimal unabhängig aus frischem Klon gebaut. Details: `docs/SPIKE_REPORT.md`.
- **Vorhanden:** Repo-Gerüst, Pin-Datei, I2S-Ausgabe, Spotify-Connect-Hülle, Architektur (`docs/ARCHITECTURE.md`), Bauanleitung für Windows (`docs/BUILD.md`, dort nicht ausgeführt), drei UI-Entwürfe.
- **Entschieden am 06./07.10.:** Audioweg AUX; natives ESP-IDF 5.5; Empfängerin nutzt Android; Speisung aus dem Lautsprecher wird zuerst getestet, LiPo als Ausweg; Pi Zero 2 W nicht lieferbar.
- **Als Nächstes:** Arbeitspakete P1–P7 aus der Architektur parallel umsetzen; Spotify-Entwickler-App anlegen; ESP-IDF auf dem PC installieren.
- **09.10.:** UI-Richtung C gewählt; Testboard XIAO ESP32-S3 Plus; `settings`-Komponente und Board-Auswahl in `main`; Arbeitspakete gestartet und abgebrochen, Zwischenstände als WIP-Branches gesichert. Fortsetzung: `docs/NEXT_STEPS.md`.

## 1. Festgelegte Architektur

| Thema | Entscheidung | Begründung |
|---|---|---|
| Audioweg | ESP32-S3 → I2S → PCM5102A → 3,5-mm-Kabel → AUX am Kilburn III | ESP32-S3 kann kein Bluetooth-Audio; AUX vermeidet zweite verlustbehaftete Kompression |
| Spotify-Audio | Fork `philippe44/cspot` (Commit `60cf172`, 06.10.2026) mit `philippe44/bell` | Original `feelfreelinux/cspot` steht seit 12.07.2024 still und ist seit dem Auth-Wechsel im Dez. 2025 faktisch defekt; der Fork wird aktiv gepflegt und steckt in squeezelite-esp32 |
| Qualität | 320 kbps Ogg Vorbis, 44,1 kHz/16 Bit, unskaliert | Maximum für inoffizielle Clients; im Gerüst fest eingestellt |
| NFC-Start | Spotify Web API (`PUT /me/player/play` mit `context_uri`) auf das cspot-Gerät | `loadTrackFromURI()` in cspot ist ein leerer Stub; das Gerät kann selbst keinen Kontext laden |
| Spotify-Login | OAuth PKCE: QR-Code am Display → Spotify-Login am Handy → HTTPS-Weiterleitungsseite (GitHub Pages) → Gerät | Spotify erlaubt keine HTTP-Redirects auf LAN-Adressen und bietet keinen Geräte-Login-Flow |
| Framework | Natives ESP-IDF v5.5.5, nicht PlatformIO, nicht IDF 6 | cspot ist reines CMake mit Submodulen; IDF 6 wechselt auf Mbed TLS 4 |
| Display | `esp_lcd_co5300` 2.2.0 mit Waveshare-Init-Tabelle, Spaltenversatz 20; Fallback `esp_lcd_sh8601` wie im Waveshare-Beispiel | |
| Touch | `esp_lcd_touch_ft5x06` 1.1.1, INT = GPIO18 | |
| UI | LVGL 9.x über `esp_lvgl_port`, Teilpuffer 2 × 280×56 im internen RAM; Fallback LVGL 8.4 | |
| NFC-Anbindung | PN532 über UART (HSU, 115200), DIP-Schalter beide OFF | I2C am gemeinsamen Bus könnte Touch blockieren (Clock-Stretching) |
| NFC-Bibliothek | `jef-sure/pn532`; Fallback `garag/esp-idf-pn532` mit eigenem NDEF-Code | |
| Tag-Inhalt | Ein NDEF-URI-Eintrag `https://open.spotify.com/<typ>/<id>`; Optionen (Shuffle, Verhalten beim Abnehmen) pro Tag-UID im Gerät | Handy öffnet beim Antippen Spotify; Tags bleiben mit jeder NFC-App beschreibbar |
| WLAN-Einrichtung | Eigener Setup-Hotspot mit Webformular, QR-Codes am Display, bis zu 5 gespeicherte Netze | Ohne App, funktioniert auf iOS und Android |
| Updates | OTA mit zwei App-Partitionen und Rollback | Inoffizielle Clients brechen gelegentlich; Reparatur muss nach dem Verschenken möglich sein |

## 2. Pinbelegung (aus Schaltplan Rev 1.1)

Intern belegt: 0 (BOOT), 4 (Akku-ADC), 9 (IMU_INT1), 10–14 (Display QSPI), 19/20 (USB), 21 (Display- und Touch-Reset), 38–41 (SD-Karte), 46 (LCD_CS), 47/48 (I2C: Touch, IMU).
Am Header, aber intern mitbenutzt und daher gesperrt: 17 (IMU_INT2), 18 (TP_INT), 45 (LCD_TE, Strapping).
Frei am Header: **1, 2, 5, 6, 7, 8, 15, 16**. Bedingt: 3 (Strapping), 43/44 (UART0).

| Board | Ziel | Hinweis |
|---|---|---|
| GPIO5 | DAC BCK | |
| GPIO6 | DAC LCK | |
| GPIO7 | DAC DIN | |
| GPIO8 | DAC XSMT (Pad 3) | 10 kΩ nach GND; Lötbrücke H3L dann offen lassen |
| GPIO15 (TX) | PN532 SCL/RX | |
| GPIO16 (RX) | PN532 SDA/TX | 10 kΩ nach 3V3 |
| GPIO2 | PN532 RSTPD_N (Pad auf der Rückseite) | Reset und Stromsparen |
| GPIO1 | Reserve | |
| 3V3, GND | DAC VIN, PN532 VCC | je eigene Leitung, Stützkondensatoren am Modul |

DAC-Lötbrücken: SCK → GND, FLT = L, DEMP = L, FMT = L, XSMT per GPIO. Auslieferzustand vor dem Einschalten mit dem Multimeter prüfen. Im Code steht die Belegung in `firmware/components/board/include/board_pins.h`.

## 3. Stromversorgung

Wunsch: Speisung aus dem USB-C-Anschluss des Kilburn III. Das wird am ersten Hardwaretag getestet.

- Der Kilburn III hat **einen** USB-C-Anschluss für Laden und Abgabe. Solange der Lautsprecher lädt, bekommt der Streamer daraus keinen Strom.
- Abgabe nur bei eingeschaltetem Lautsprecher und über 20 % Akku (Handbuch).
- USB-Strom und AUX am selben Lautsprecher bilden eine Masseschleife; hörbares Ticken ist wahrscheinlich (**offen**).
- Ob der Anschluss bei geringer Last abschaltet, ist nicht dokumentiert (**offen**).

Test: leises Testsignal bei voller Lautstärke, einmal aus dem Lautsprecher gespeist, einmal aus einer separaten Quelle. Ergebnis und Folge:

| Ergebnis | Folge |
|---|---|
| Still, Anschluss bleibt an | Speisung aus dem Lautsprecher; LiPo nur für die Zeit, in der der Lautsprecher lädt |
| Ticken oder Brummen | Isolierter DC/DC-Wandler in die 5-V-Leitung, oder LiPo-Betrieb |
| Anschluss schaltet bei Pause ab | LiPo als Puffer |

## 4. Zusatzteile

| Teil | Status |
|---|---|
| Geschirmtes 3,5-mm-Stereokabel, 0,2–0,5 m | nötig |
| USB-C-auf-USB-C-Kabel ≤ 0,3 m | nötig für Speisung aus dem Lautsprecher |
| LiPo, Vorschlag: EREMIT 3,7 V 2500 mAh „104050" (52 × 40 × 10 mm, Schutzschaltung, Stecker JST-PH 2.0) plus Anschlusskabel **MX1.25 2-polig** zum Umlöten; rechnerisch rund 10 h. Polung gegen den „− +"-Aufdruck am Board prüfen | empfohlen |
| Isolierter DC/DC 5 V→5 V, 2–3 W, plus Elko 1000 µF | empfohlen als Vorrat, falls Störgeräusche |
| Elko 470 µF/10 V, 2 × 47–100 µF, 100 nF, Ferritperle oder 1 Ω | empfohlen |
| Widerstände 2 × 10 kΩ, 3 × 33–47 Ω | empfohlen |
| USB-Netzteil 5 V ≥ 1 A | empfohlen |
| 2 × 510 Ω | nur falls der AUX-Eingang bei 2 Vrms übersteuert |

## 5. Spotify-Rahmenbedingungen

- Nötig ist eine eigene Spotify-Entwickler-App (Client-ID und Secret). Der Besitzer braucht dauerhaft Premium; endet es, steht das Gerät.
- Die beschenkte Person muss mit Name und Spotify-E-Mail in die Nutzerliste der App (max. 5) und braucht selbst Premium. Die E-Mail fehlt noch; für die Entwicklung reicht das eigene Konto.
- Refresh-Tokens laufen seit Juni 2026 nach 6 Monaten ab: Die Person muss etwa zweimal im Jahr den QR-Login wiederholen.
- Damit das Gerät ohne Handy-Anstoß erreichbar ist, wählt die Person es einmal in der Spotify-App aus; danach meldet es sich mit gespeicherten Zugangsdaten selbst an.
- Entwicklungsmodus: Titel fremder Playlists sind nicht lesbar, redaktionelle Spotify-Playlists gesperrt. Ob sie per `context_uri` trotzdem abspielen, ist **offen**.
- cspot ist ein nachgebauter Client; Spotifys Nutzungsbedingungen untersagen Reverse Engineering. Sperrungen von Premium-Konten wurden nicht gefunden. Lizenz GPLv3 (relevant erst bei Weitergabe des Codes).
- Das Client-Secret gehört nicht in ein öffentlich gehostetes Firmware-Image; es wird beim Flashen getrennt im Gerät abgelegt.

## 6. Risiken und Prüfpunkte

1. **Läuft cspot auf dem Board?** Der Build ist bestätigt, die Laufzeit nicht: Anmeldung, Abruf mit eigener Client-ID und 320-kbps-Wiedergabe sind **offen**. Prüfpunkt: Spielt der S3 am Ende von Hardwaretag 2 keine Musik, braucht es einen Linux-Kleinrechner mit go-librespot für den Ton; der ESP32 bliebe Display und NFC. Der Pi Zero 2 W ist derzeit nicht lieferbar, ein anderes Modell müsste beschafft werden.
2. Interner RAM knapp (TLS, WLAN, LVGL, Audio gleichzeitig): Im Emulator blieben mindestens 199 kB frei, aber ohne WLAN-Treiber und ohne LVGL. Heap ab dem ersten Tag mitloggen.
3. Weiterleitung von der HTTPS-Seite auf die LAN-Adresse des Geräts kann am Handy-Browser scheitern: auf Android Chrome testen; Fallback kleiner Cloud-Relay.
4. Bekannte Schwächen in cspot (Netzfehler beim Token-Abruf führt zum Neustart) sind noch nicht behoben.
5. Zeitplan: voller Umfang bis Mitte nächster Woche ist knapp. Erste Version siehe Abschnitt 7; Rest per OTA.

## 7. Umfang

Erste Version: Spotify Connect mit 320 kbps, Wiedergabe-Bildschirm mit Cover und Bedienung, NFC Auflegen/Abnehmen mit einstellbarem Verhalten, WLAN-Einrichtung, Spotify-Login, OTA.
Danach per Update: Tags am Gerät beschreiben, Playlist-Auswahl, mehrere Netze und Hotspot-Feinschliff, Einbrennschutz-Feinheiten.

## 8. Ablauf und Agents

| Phase | Zeitraum | Parallel | Inhalt | Stand |
|---|---|---|---|---|
| A Architektur | Di–Mi | 1 Agent | Repo-Gerüst, Modulschnittstellen, `sdkconfig`, Partitionstabelle, Build-Test | erledigt |
| B Vorarbeit ohne Hardware | Mi–Fr | 7 Agents | Arbeitspakete P1–P7 aus `docs/ARCHITECTURE.md` | offen |
| C Inbetriebnahme | Hardwaretag 1–2 | du + 2 Agents | Waveshare-Demo, Display/Touch, Sinuston über DAC, Störgeräusch- und Pegeltest, cspot auf dem S3 | offen |
| D Integration | Tag 3–4 | 1 Agent, dann 2 | Module zusammenführen, RAM- und Prioritätsabstimmung | offen |
| E Prüfung | Tag 5 | 2–3 Agents | Unabhängiges Review, Testprotokoll, Aufbauanleitung | offen |

Hardwaretests kann kein Agent ausführen; sie bestimmen das Tempo ab Phase C.

## 9. Quellen (Auswahl)

- Schaltplan, Datenblatt PCM5102A, Kilburn-III-Handbuch: Ordner `docs/`
- https://github.com/philippe44/cspot · https://github.com/feelfreelinux/cspot/issues · https://github.com/sle118/squeezelite-esp32
- https://developer.spotify.com/documentation/web-api/tutorials/february-2026-migration-guide
- https://developer.spotify.com/blog/2026-06-18-refresh-token-expiration
- https://developer.spotify.com/documentation/web-api/concepts/redirect_uri
- https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.64-v2
- https://components.espressif.com/components/espressif/esp_lcd_co5300
- https://components.espressif.com/components/jef-sure/pn532
- https://www.elechouse.com/elechouse/images/product/PN532_module_V3/PN532_%20Manual_V3.pdf
- https://www.eremit.de/p/eremit-3-7v-2500mah-lipo-104050
- https://github.com/devgianlu/go-librespot
