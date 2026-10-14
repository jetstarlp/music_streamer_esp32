# NFC-Leser PN532 (`nfc`)

Stand: 13.10.2026. Komponente `firmware/components/nfc`: Dienst für den PN532 V3 (Elechouse-Bauform) über UART (HSU, 115200 Baud). Er erkennt aufgelegte NTAG213/215/216, liest den Spotify-Link (Format: [NFC_TAG.md](NFC_TAG.md)) und kann Tags beschreiben. Wiedergabe beim Auflegen gibt es noch nicht: Die Firmware protokolliert die Ereignisse nur (`app: nfc: tag present: spotify:playlist:…`).

## Verdrahtung (Waveshare ESP32-S3-Touch-AMOLED-1.64)

| PN532 V3 | Waveshare-Stiftleiste | Hinweis |
|---|---|---|
| VCC | 3V3 | nur 3,3 V, nicht 5 V (sonst 5-V-Pegel auf TX) |
| GND | GND | |
| SCL (= RX im HSU-Modus) | GPIO15 | ESP-TX → PN532-RX |
| SDA (= TX im HSU-Modus) | GPIO16 | PN532-TX → ESP-RX; 10 kΩ nach 3V3 (intern zusätzlich Pull-up an) |
| RSTPD_N (Lötpunkt/Pin „RSTPDN“) | GPIO2 | Hardware-Reset; ohne diese Leitung geht es auch, dann ohne Reset bei Hängern |

DIP-Schalter auf dem Modul: **beide OFF = HSU** (Tabelle auf der Platine: HSU = `0 0`, I2C = `1 0`, SPI = `0 1`). Nach dem Umstellen Spannung kurz trennen. IRQ und RSTO bleiben frei.

XIAO ESP32-S3 Plus (Prüfaufbau): TX = D10 (GPIO9), RX = D0 (GPIO1), RSTPD_N = D1 (GPIO2), siehe `board_pins.h`.

## Ablauf in der Firmware

- Task `nfc` auf Kern 0, Priorität 4, 4 kB Stack; 1,3 kB Lesepuffer im PSRAM. UART1, 512 Byte Empfangspuffer.
- Alle 300 ms (`nfc_set_poll_interval_ms`, 50 … 5000): HF-Feld an, einen Typ-A-Tag suchen, Feld wieder aus (Feld ist pro Abfrage nur ca. 20–40 ms an).
- Neuer Tag: CC und nur so viel NDEF-Daten lesen wie nötig, dann einmal `NFC_EVT_TAG_PRESENT`. Liegt er weiter auf, kommt nichts mehr. `NFC_EVT_TAG_REMOVED` erst nach 3 Abfragen ohne Tag (ca. 1 s).
- Handys (Kartenemulation mit Zufalls-UID `08 …`) zählen als ein Tag, solange sie liegen bleiben; sie liefern keinen Link.
- PN532 antwortet nicht: `NFC_EVT_READER_ERROR`, neuer Versuch nach 1, 2, 4, 8, 16, 30, 30 … s (eine Logzeile je Stufe, bei 30 s nur einmal). Wieder da: `NFC_EVT_READER_OK`.
- Schreiben (`nfc_write_ref`): Abfragen pausieren, auf Tag warten, nur NTAG213/215/216; prüft Sperrbits, CC und Passwort (AUTH0), formatiert ein leeres NTAG (CC `00 00 00 00`), schreibt sicher (Länge 0 → Rest → echte Länge) und liest zurück. Danach `NFC_EVT_TAG_WRITTEN` statt `TAG_PRESENT` (der frisch beschriebene Tag startet also nichts).

## Test am Gerät (serielle Konsole)

1. Firmware flashen, Konsole öffnen. Beim Start erscheint `nfc: PN532 v1.6 ready …`.
2. `nfc` zeigt Leser, Firmware, aufliegenden Tag und Zähler.
3. Tag auflegen: `nfc: tag 04:…: (NTAG215): spotify:playlist:…` bzw. `no Spotify reference - empty NDEF message` bei einem leeren Tag. Abnehmen: nach ca. 1 s `tag … removed`. Liegen lassen: keine weiteren Meldungen.
4. Beschreiben: `nfc write https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M` (auch `spotify:album:<id>`), dann innerhalb von 15 s den Tag auflegen → `written and verified`.
5. Lesen: `nfc read` (wartet 5 s) zeigt UID, Typ, Anzahl gelesener Bytes und den Link oder den Grund.
6. Gegenprobe mit dem Handy: Tag ans Handy halten, es öffnet den Link in der Spotify-App.

## Fehlersuche

| Symptom | Ursache / Abhilfe |
|---|---|
| `PN532 not answering (no ACK for command 0x14)` | TX/RX vertauscht (SDA/SCL tauschen); DIP-Schalter nicht beide OFF; Modul ohne 3V3/GND; nach DIP-Änderung nicht stromlos gemacht |
| `no answer to command 0x14` | Pegel/Masse prüfen, Pull-up 10 kΩ an GPIO16; RSTPD_N hängt auf Low (Leitung prüfen oder abklemmen) |
| `unexpected IC` | anderer Chip als PN532 am UART |
| Leser fällt im Betrieb aus (`stopped answering`) | Wackelkontakt, Versorgung bricht bei HF-Feld ein (kurze Leitungen, 3V3 direkt vom Board) |
| Tag wird nicht erkannt | Tag mittig auf die Antenne (Seite mit dem Aufdruck), Abstand < 3 cm; Metall unter dem Tag stört |
| `not an NFC Forum Type 2 tag (SAK 08)` | MIFARE Classic o. ä.: nicht unterstützt, NTAG21x verwenden |
| `write failed: tag is read-only` / `password` | Tag mit Sperrbits oder Passwort (z. B. in NFC Tools gesetzt): neuen Tag nehmen |
| `aborted mid-write (tag now empty)` | Tag beim Schreiben bewegt: noch einmal schreiben |

## Tests ohne Hardware

`firmware/tools/host_test_nfc.sh` (gcc, ASan/UBSan, `-Werror`):

- `tests/host/nfc/test_pn532_frame.c`: Rahmen bauen (Beispiele aus dem PN532-Handbuch UM0701-02: GetFirmwareVersion `00 00 FF 02 FE D4 02 2A 00`, SAMConfiguration, InListPassiveTarget), ACK/NACK/Fehlerrahmen, Antwort von GetFirmwareVersion `00 00 FF 06 FA D5 03 32 01 06 07 E8 00`, InListPassiveTarget mit 7-Byte-UID, 4-Byte-UID und ATS, erweiterter Rahmen, falsche Prüfsummen, Müll vor dem Rahmen, zu lange Rahmen, 20 000 Rauschdurchläufe.
- `tests/host/nfc/test_t2t.c`: Lesen/Schreiben gegen ein simuliertes NTAG213/215/216 (OTP-CC, statische und dynamische Sperrbits, AUTH0, Überlauf am Speicherende), unformatierte Tags, Rücklesefehler, Tag-Verlust nach jedem einzelnen Befehl (danach steht immer der alte Link, der neue oder eine leere Nachricht auf dem Tag), schrittweises Lesen einer 307-Byte-Nachricht mit 3-Byte-TLV-Länge.

QEMU (`firmware/tools/qemu_smoke_test.sh`) hat keinen PN532: Der Dienst meldet einmal je Rückzugsstufe `PN532 not answering` und läuft sonst unauffällig weiter.

## Nicht getestet (keine Hardware vorhanden)

Der Treiber ist nur gegen Handbuch-Bytefolgen und Simulation geprüft. Offen am echten Aufbau: Aufwachen im HSU-Modus nach Reset (Wartezeiten 20 ms Reset, 100 ms Hochlauf), Zeitverhalten von InListPassiveTarget mit 8 Aktivierungsversuchen bei abgeschaltetem Feld, GET_VERSION über InCommunicateThru, WRITE (0xA2) über InDataExchange, tatsächliche Lesereichweite, Störungen des Audiopfads durch das HF-Feld, Stromspitzen an 3V3.
