# Test v1 (Firmware 75422da) – Anschluss NFC und Prüfschritte

Stand: 14.10.2026. Ziel: alles, was am Montag übergeben wird, einmal am echten Gerät geprüft.

## A. PN532 anschließen

Gerät vorher vom USB trennen.

| PN532 V3 (4-polige Leiste) | Waveshare-Stiftleiste |
|---|---|
| VCC | **3V3** (nicht 5V) |
| GND | GND |
| SDA / TXD | **GPIO16** |
| SCL / RXD | **GPIO15** |

- RSTPD_N bleibt frei (optional, auf dem V3 meist nicht herausgeführt).
- DIP-Schalter am PN532: **beide auf OFF/0 = HSU (UART)**.
- Kreuzen ist richtig so: TX des einen an RX des anderen; die Firmware sendet auf GPIO15 und empfängt auf GPIO16.
- Abstand zur Antenne des ESP32 und zu Metall halten; Kabel möglichst kurz.

## B. Flashen

`ms-waveshare-6c8dde6.bin` wie gewohnt an 0x0. WLAN und Spotify-Login bleiben erhalten.
Danach Konsole öffnen (Arduino IDE, 115200, „Newline“) und RESET drücken.

## C. Prüfschritte

Bei jedem Schritt: beobachten, kurz notieren (ok / was anders war), Log-Zeilen kopieren, wo verlangt.

1. **Start und Speicher**
   - Startbild „Music Streamer – startet …“, dann „Verbinde …“, dann „Musik wählen“ oder „Bereit“.
   - Im Log: `display: ready`, `nfc: reader ready` (oder `PN532 v1.6 ready`).
   - Kopieren: alle Zeilen mit `heap [` (beim Start und nach ca. 1 Minute).
2. **NFC-Leser**: `nfc` eingeben → zeigt `reader ready` und die Firmware „PN532 v1.6“. Wenn nicht: Verdrahtung, DIP-Schalter.
3. **Tag beschreiben**
   - Spotify-App: Playlist → Teilen → Link kopieren (beginnt mit `https://open.spotify.com/playlist/…`; ein Kurzlink `spotify.link/…` erst im Browser öffnen und den langen Link kopieren).
   - Konsole: `nfc write <Link>` und innerhalb von 15 s einen leeren NTAG215 auflegen → `written and verified`.
   - Gegenprobe: `nfc read` mit aufgelegtem Tag zeigt `spotify:playlist:…`.
   - Optional zweiter Tag mit einem Album-Link.
4. **Abspielen über das Handy**: in der Spotify-App „Music Streamer“ wählen, Song starten.
   - Display: Titel, Interpret, Cover (nach 1–3 s), Fortschritt läuft.
   - Tasten am Display: Pause/Play, Weiter, Zurück; Wischen links/rechts.
   - Ton: Knackser oder Aussetzer im Vergleich zur letzten Version?
5. **Tag auflegen (Kernpunkt)**: Tag mit Playlist auflegen.
   - Hinweis „Playlist wird gestartet …“, nach einigen Sekunden Musik.
   - Kopieren: Log vom Auflegen bis Musik läuft (wichtig: `resolving`, `context-resolve`, `Web API`, `local load`).
   - Spotify-App am Handy: zeigt sie die Playlist auf „Music Streamer“?
6. **Tag abnehmen**: Musik pausiert (Standard „Pausieren“). Wieder auflegen → spielt an der Stelle weiter.
7. **Einstellung „Titel zu Ende“**: Einstellungen (Zahnrad) → „Wenn der Tag abgenommen wird“ → „Titel zu Ende“. Tag abnehmen → Hinweis „Pause nach diesem Titel“, am Titelende Pause. Danach wieder auf „Pausieren“ stellen.
8. **Anderer Tag**: Album-Tag auflegen, während die Playlist läuft → wechselt zum Album.
9. **Helligkeit**: Regler in den Einstellungen; nach Neustart (`reboot`) bleibt der Wert.
10. **WLAN-Einrichtung**: Einstellungen → WLAN „Ändern“.
    - Display zeigt QR-Code und „Music-Streamer“.
    - Handy: QR-Code scannen → mit Hotspot verbinden → Seite öffnet sich (sonst Browser: `http://192.168.4.1`).
    - Liste der WLANs sichtbar? Nichts speichern; am Display „Abbrechen“ → zurück.
11. **Ruhe/Bildschirmschoner**: 5 Minuten nichts tun, nichts abspielen → Bildschirmschoner; Antippen weckt.
12. **Dauertest**: 20 Minuten Playlist laufen lassen, dabei ein paar Mal am Display weiter/zurück.
    - Am Ende `status` eingeben und die Ausgabe kopieren (dma gaps, underruns, heap).

13. **Am Marshall Kilburn III, Strom aus der Box (Powerbank)** – der echte Einsatz, ohne PC.
    - Aufbau: Klinkenkabel DAC → AUX der Box; USB-Kabel vom USB-Ausgang der Box zum Waveshare. Box mit vollem und einmal mit fast leerem Akku probieren.
    - **Start**: Box einschalten → startet der Streamer sauber (Startbild, WLAN, Spotify)? Bleibt er an, oder geht er aus bzw. startet neu (Display flackert, Startbild kommt wieder)?
    - **Stromspitzen**: Tag auflegen, Playlist mit hoher Lautstärke an der Box, dabei Display antippen/wischen und Cover wechseln lassen (WLAN + Display gleichzeitig) – kein Neustart?
    - **Störgeräusche**: in einer leisen Stelle bzw. bei Pause genau hinhören, Box laut gestellt: Brummen, Pfeifen, Knistern, Zirpen im Takt von WLAN/Display? Zum Vergleich einmal mit Strom aus einer anderen Powerbank/USB-Netzteil statt aus der Box.
    - **Pegel**: Lautstärke in der Spotify-App auf 100 %, Box laut: klingt es bei Bass/lauten Stellen verzerrt? Falls ja, mit App-Lautstärke ~80 % vergleichen.
    - **Bereitschaft der Box**: 20–30 Minuten pausiert liegen lassen → schaltet die Box ab (Auto-Standby) und damit den Streamer? Kommt nach dem Wiedereinschalten alles von selbst zurück (WLAN, Spotify, Tag startet)?
    - **Laufzeit**: 1 Stunde Musik aus Akku – wie viel Akku hat die Box verloren (Anzeige in der Marshall-App)?
    - Ohne PC gibt es kein Log: Auffälligkeiten mit Uhrzeit notieren. **Vorher** am PC `status clear` eingeben (setzt die Zähler auf 0), **danach** am PC `status`: die Zeile `resets:` zählt Unterspannungs-Neustarts (brownout) und Abstürze, die während des Tests passiert sind.

## D. Rückmeldung

Für jeden Punkt 1–13: ok / Problem (Beschreibung), dazu die verlangten Log-Ausschnitte. Bei Absturz oder Neustart: die Zeilen ab `Guru Meditation` bzw. `Backtrace` komplett.
