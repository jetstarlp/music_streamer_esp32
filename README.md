# Music Streamer

Firmware für einen kleinen Spotify-Connect-Streamer:

* **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.64 Rev 1.1 (ESP32-S3R8, 8 MB Octal-PSRAM, 16 MB Flash)
* **Audioweg:** Spotify (Ogg Vorbis, 320 kbit/s) wird auf dem ESP32-S3 von
  [cspot](https://github.com/philippe44/cspot) dekodiert und per I2S an einen
  PCM5102A-DAC gegeben, dessen Line-Ausgang am AUX-Eingang des Lautsprechers hängt.
* **Geplant, noch nicht in diesem Repository:** LVGL-Oberfläche auf dem AMOLED,
  NFC-Leser PN532 (Tags starten Playlists/Alben), Spotify-Web-API-Client,
  WLAN-Einrichtungsportal, OTA-Updates.

## Stand

Build-Spike vom 06./07.10.2026. Die Firmware **kompiliert und linkt** für den
ESP32-S3 mit ESP-IDF v5.5.5 und startet im Emulator (QEMU) bis „wartet auf
Spotify-Login". **Auf echter Hardware und gegen ein Spotify-Konto ist noch
nichts getestet.**

* [docs/BUILD.md](docs/BUILD.md) – Bauen und Flashen unter Windows, Zugangsdaten eintragen, Stolperfallen
* [docs/SPIKE_REPORT.md](docs/SPIKE_REPORT.md) – was gemacht wurde, Versionen, Workarounds, Größen, offene Risiken

## Aufbau

```
firmware/
  main/                       app_main: NVS, WLAN (Zugangsdaten aus Kconfig), Audio, Spotify, Heap-Log
  components/board/           board_pins.h – alle GPIOs an einer Stelle
  components/audio_out/       I2S-Ausgabe (i2s_std) für den PCM5102A, Ringpuffer + Feeder-Task
  components/spotify_connect/ C-API um cspot + bell (Git-Submodul), Zeroconf-Login, Zugangsdaten im NVS
  partitions.csv              16 MB: nvs, otadata, phy_init, ota_0/ota_1 (je 5 MB), storage (LittleFS)
  sdkconfig.defaults
  sdkconfig.qemu, tools/      Rauchtest im Emulator (ohne Hardware)
docs/
```

## Kurzfassung

```
git clone --recursive <dieses Repository> C:\dev\music-streamer     (Pfad ohne Leerzeichen!)
cd C:\dev\music-streamer\firmware
idf.py menuconfig          ->  "Music Streamer": WLAN + Spotify Client-ID/Secret
idf.py build
idf.py -p COMx flash monitor
```

Im Repository stehen keine Zugangsdaten: WLAN- und Spotify-Daten sind
standardmäßig leer und liegen nur in der lokalen, von Git ignorierten Datei
`firmware/sdkconfig` (oder `firmware/sdkconfig.secrets`).

## Lizenz

cspot steht unter der GNU General Public License v3, und diese Firmware wird
dagegen gelinkt. **Das Projekt als Ganzes ist deshalb GPL-3.0** (siehe
[LICENSE](LICENSE)): Wer die Firmware oder ein Gerät damit weitergibt, muss dem
Empfänger auch den zugehörigen Quelltext anbieten.

Fremdcode und Lizenzen: cspot (GPL-3.0), bell (siehe dessen Repository),
tremor (BSD-3-Clause, Xiph.org), nanopb (zlib), cJSON (MIT), ESP-IDF und
espressif/mdns (Apache-2.0).

cspot ist eine unabhängige Nachimplementierung des Spotify-Connect-Protokolls
und steht in keiner Verbindung zu Spotify. Nötig sind ein Spotify-Premium-Konto
und eine eigene Spotify-Entwickler-App (Client-ID/Secret).
