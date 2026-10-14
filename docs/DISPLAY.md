# Display: Inbetriebnahme und Test

Stand: 12.10.2026. Komponente `firmware/components/display`, Test-App `firmware/components/display/test_app`.

## Aufbau

| Teil | Lösung |
|---|---|
| Panel | CO5300 über QSPI 40 MHz, Treiber `espressif/esp_lcd_co5300` 2.2.0 mit Waveshares Init-Sequenz, Spaltenversatz 20 |
| Touch | FT3168 an I2C 0x38 (SDA 47, SCL 48), direkt ausgelesen wie in Waveshares Beispiel, neuer I2C-Treiber |
| LVGL | v9.3.0 über `esp_lvgl_port` 2.9.0, zwei Zeichenpuffer 280 × 40 Pixel im internen DMA-RAM |
| Drehung | Software-Drehung, Standard 90° (Querformat 456 × 280), umstellbar in menuconfig → „Music Streamer display“ |
| Helligkeit | Befehl 0x51 über `display_set_brightness()`, Standard 70 % |

Die LVGL-Einstellungen, auf die sich die Oberfläche verlassen kann, stehen in `firmware/components/display/sdkconfig.lvgl`: 16-Bit-Farbe, Speicher über `malloc` (große Blöcke im PSRAM), Montserrat 14, QR-Code-Widget.

## Test am Gerät

1. `ms-display-test.bin` an Adresse 0x0 flashen (wie in `BENCH_TEST.md`, Abschnitt 3), RESET drücken.
2. Erwartetes Bild:
   - weißer 1-Pixel-Rahmen genau auf allen vier Rändern,
   - Ecken: **rot oben links, grün oben rechts, blau unten links, weiß unten rechts**,
   - Farbbalken rot, grün, blau, messing, creme, grau,
   - Text „456 x 280 (rotation 90)“ aufrecht,
   - unten ein Helligkeitsregler, rechts unten die Bildrate.
3. Mit dem Finger tippen und ziehen: Ein gelbes Kreuz folgt dem Finger, die Koordinaten stehen in der Mitte.

## Was tun, wenn …

| Beobachtung | Ursache | Abhilfe |
|---|---|---|
| Bild steht auf dem Kopf | falsche Drehrichtung für den Einbau | menuconfig → Rotation 270 statt 90 |
| Rand links oder rechts fehlt, Streifen am Rand | Spaltenversatz | `LCD_X_GAP` in `display.c` prüfen (Waveshare: 20) |
| Rot und Blau vertauscht | Farbreihenfolge | `rgb_ele_order` auf BGR |
| Farben völlig falsch, Rauschen | Byte-Reihenfolge | `swap_bytes` in `display.c` umschalten |
| Bild zerrissen oder versetzte Blöcke | ungerade Fensterkanten | Rounder prüfen |
| Kreuz an falscher Stelle, gespiegelt | Touch-Achsen | Koordinaten-Zuordnung in `touch_read_cb` |
| Schwarz, Log „touch controller not answering“ | Touch nicht erreichbar | Bild sollte trotzdem kommen; Log schicken |
| Ganz schwarz | Panel startet nicht | Waveshare-Werksfirmware flashen; geht die, liegt es an meinem Code, sonst an der Hardware |

Das Log liest du über die Konsole (esptool-js → Console, oder `idf.py monitor`).

## Messwert für später

Stromaufnahme mit einem USB-Messadapter bei Helligkeit 20 %, 70 % und 100 % notieren; das entscheidet über Dimmen und Akkulaufzeit.

## Nicht getestet

Alles, was die Hardware betrifft: Init-Sequenz mit diesem Treiber, Versatz, Byte- und Farbreihenfolge, Software-Drehung, Touch-Auslesen, Helligkeitsbefehl, Bildrate. Geprüft ist nur, dass die Test-App fehlerfrei und ohne Warnungen baut (ESP-IDF v5.5.5, LVGL 9.3.0): 529 kB Programm, 76 kB internes RAM statisch belegt.
