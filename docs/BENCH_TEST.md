# Test-Firmware: Ton und Spotify ausprobieren

Stand: 12.10.2026. Diese Firmware testet die Tonkette (ESP32-S3 → PCM5102A → AUX → Kilburn III) und Spotify Connect, bevor Display, NFC und Bedienoberfläche dazukommen. Sie läuft auf dem **Waveshare ESP32-S3-Touch-AMOLED-1.64** und auf dem **Seeed XIAO ESP32-S3 Plus**. Das Display bleibt mit ihr schwarz; das ist so gewollt.

Zugangsdaten stehen nicht in der Datei. WLAN und Spotify-App-Daten gibst du nach dem Flashen über die serielle Konsole ein; sie bleiben im Gerät gespeichert.

## 1. Dateien

| Board | Datei |
|---|---|
| Waveshare AMOLED 1.64 | `ms-waveshare-<version>.bin` |
| XIAO ESP32-S3 Plus | `ms-bench-xiao-<version>.bin` |

Beide sind komplette Abbilder und werden an **Adresse 0x0** geschrieben.

## 2. Verdrahtung DAC (PCM5102A)

Die I2S-Pins sind auf beiden Boards dieselben GPIOs.

| PCM5102A | Waveshare (Stiftleiste, Beschriftung = GPIO) | XIAO (Beschriftung) | GPIO |
|---|---|---|---|
| BCK | 5 | D4 | 5 |
| LCK | 6 | D5 | 6 |
| DIN | 7 | D8 | 7 |
| XSMT (Pad 3, siehe unten) | 8 | D9 | 8 |
| VIN | 5V | 5V | – |
| GND | G | GND | – |

**VIN an 5 V, nicht an 3V3.** Das DAC-Modul hat eigene 3,3-V-Regler. Mit 5 V am Eingang regeln sie sauber und filtern Störungen aus der Versorgung; mit 3,3 V laufen sie ohne Reserve und reichen Störungen fast ungefiltert durch. Der 5-V-Pin führt die USB-Spannung, funktioniert also, solange das Board über USB versorgt wird. Für den späteren Akkubetrieb klären wir das neu.

**Lötbrücken des DAC-Moduls vor dem ersten Einschalten mit dem Multimeter prüfen:**

| Brücke | Soll | Bedeutung |
|---|---|---|
| SCK (Vorderseite) | mit GND verbunden | Takt aus BCK, keine MCLK-Leitung |
| 1 / H1L (FLT) | L | normales Filter |
| 2 / H2L (DEMP) | L | keine De-Emphasis |
| 3 / H3L (XSMT) | **offen**, Pad 3 an GPIO 8, 10 kΩ von Pad 3 nach GND | Firmware schaltet stumm/laut ohne Knacken |
| 4 / H4L (FMT) | L | I2S-Format |

Ohne den 10-kΩ-Widerstand funktioniert es auch, nur kann beim Einschalten kurz ein Knacken zu hören sein. Alternative: H3L auf H brücken und GPIO 8 frei lassen (dann kein Stummschalten durch die Firmware).

Ausgang: 3,5-mm-Buchse des DAC-Moduls → kurzes Stereo-Kabel → AUX am Kilburn III.

## 3. Flashen ohne ESP-IDF (Chrome oder Edge)

1. Board per USB-C mit dem PC verbinden.
2. https://espressif.github.io/esptool-js/ öffnen, bei „Program" die Baudrate auf 921600 stellen und **Connect** klicken; den Port des Boards auswählen (bei Windows „USB JTAG/serial debug unit").
3. Taucht kein Port auf oder schlägt das Verbinden fehl: **BOOT** gedrückt halten, kurz **RESET** drücken (XIAO: RST), BOOT loslassen, erneut Connect.
4. Flash Address **0x0**, die passende `.bin`-Datei wählen, **Program**.
5. Danach **RESET** drücken (oder USB kurz trennen).

Mit installiertem ESP-IDF geht es auch so (im Ordner `firmware`):
`python -m esptool --chip esp32s3 -p COMx write_flash 0x0 dist\ms-waveshare-<version>.bin`

## 4. Konsole

Die Konsole auf der esptool-js-Seite zeigt nur Ausgaben an; Eingaben schickt sie nicht an das Board. Zum Tippen ein echtes Terminal nehmen. Vorher auf der esptool-js-Seite **Disconnect** klicken oder den Tab schließen, sonst ist der Port belegt.

- **Arduino IDE** (ist bei dir installiert): Werkzeuge → Port → den COM-Port des Boards wählen (das Board-Modell ist egal), dann Werkzeuge → Serieller Monitor. Unten rechts **„Neue Zeile“** (Newline) und **115200 Baud** einstellen. Befehle oben ins Eingabefeld tippen, Enter.
- **Alternative im Browser:** https://googlechromelabs.github.io/serial-terminal/ in Chrome oder Edge, Port wählen, Connect, direkt ins Fenster tippen.
- Mit installiertem ESP-IDF: `idf.py -p COMx monitor`.

Nach einem Reset erscheint `streamer>`; `help` listet alle Befehle. Siehst du keine Eingabeaufforderung, einmal Enter drücken oder RESET am Board.

Einrichtung:

```
wifi add "Name deines WLANs" deinPasswort
spotify creds <Client-ID> <Client-Secret>
reboot
```

- SSIDs mit Leerzeichen in Anführungszeichen setzen. Nur 2,4-GHz-Netze funktionieren.
- Client-ID und Secret stammen aus deiner App im Spotify Developer Dashboard. Das Secret siehst du nur beim Eintippen; die Firmware zeigt danach nur die letzten 4 Zeichen.
- `status` zeigt alles auf einen Blick: Speicher, WLAN (Signalstärke, IP), Spotify-Zustand, Audio-Zähler.
- Weitere Befehle: `wifi list`, `wifi del <ssid>`, `name <Gerätename>`, `spotify forget`, `factory-reset yes`.

Beim XIAO zeigt die LED den Zustand: langsam blinkend = kein WLAN, dauerhaft an = WLAN verbunden, schnell blinkend = Musik läuft. Der Waveshare hat keine LED dafür.

## 5. Testablauf

Lautstärke am Kilburn zunächst niedrig, Quelle AUX (keine Bluetooth-Wiedergabe gleichzeitig).

1. **Ton:** `tone` → 5 s Sinus 1 kHz. Kommt nichts: `status` → Zeile „audio" muss „running" zeigen und darf nicht „MUTED" enthalten; Verdrahtung und Lötbrücken prüfen.
2. **Pegel/Übersteuerung:** `tone 60 0 5` und `tone 1000 0 5` (volle Aussteuerung), Lautsprecher auf mittlerer Lautstärke. Klingt der 60-Hz-Ton verzerrt oder schnarrend, übersteuert der AUX-Eingang; dann melden, ich dämpfe in der Firmware oder per Widerstand.
3. **Störgeräusche (Masseschleife):** Lautsprecher auf **maximale** Lautstärke, `tone noise-test` (60 s, sehr leiser Ton bei −60 dBFS). Zweimal hören:
   a) Board aus dem USB-C-Anschluss des Kilburn III versorgt,
   b) Board aus Laptop oder Powerbank versorgt.
   Auf Ticken, Brummen oder Zirpen achten, auch bei WLAN-Verkehr (z. B. während Spotify lädt). `tone stop` beendet den Ton.
4. **Spotify:** Nach `reboot` verbindet sich das Gerät. In der Spotify-App auf dem Android-Handy (im selben WLAN) unten auf das Geräte-Symbol tippen und „Music Streamer" wählen. Prüfen: Wiedergabe startet, Titel überspringen, Pause, 15 Minuten am Stück ohne Aussetzer. Danach einmal `reboot`: Das Gerät muss sich ohne Handy wieder anmelden (`status` → „logged in", „stored login").
5. **Lautstärke** regelst du am Kilburn. Der Lautstärkeregler in der Spotify-App ändert absichtlich nichts, damit das Signal unverfälscht bleibt.

## 6. Was du mir zurückschicken solltest

- Ergebnis von Test 1–4 in Stichworten (insbesondere 3a gegen 3b).
- Ausgabe von `status` nach 15 Minuten Wiedergabe.
- Falls das Gerät neu startet oder hängt: den kompletten Text aus der Konsole ab der letzten Meldung vor dem Problem.

## 7. Nicht getestet

Gebaut und im Emulator geprüft sind Start, Konsole, Speichern und Löschen der Einstellungen, Testton-Befehl und die ersten Schritte der Spotify-Anmeldung. Noch nie gelaufen sind: WLAN-Verbindung, Tonausgabe über I2S, Spotify-Anmeldung und Wiedergabe, Status-LED. Genau das prüft dieser Test.

## 8. NFC (PN532) – folgt

Geplante Pins, noch nicht in dieser Firmware:

| PN532 (HSU, beide DIP-Schalter OFF) | Waveshare | XIAO |
|---|---|---|
| SCL/RX | GPIO 15 | D10 (GPIO 9) |
| SDA/TX | GPIO 16 | D0 (GPIO 1) |
| RSTPD_N (Pad auf der Rückseite) | GPIO 2 | D1 (GPIO 2) |
| VCC / GND | 3V3 / G | 3V3 / GND |
