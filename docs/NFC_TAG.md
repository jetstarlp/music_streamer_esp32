# NFC-Tags mit Spotify-Links (`nfc_tag`)

Stand: 13.10.2026. Komponente `firmware/components/nfc_tag`: reines C ohne Hardware. Sie liest den Speicher eines NFC-Tags (NFC Forum Type 2, also NTAG213/215/216) und findet darin einen Spotify-Verweis; zum Beschreiben erzeugt sie den Inhalt und plant einen sicheren Schreibablauf. Den PN532 bedient die Komponente `nfc`.

## Was erkannt wird

**Tag-Inhalt** (Datenbereich ab Seite 4): die erste NDEF-TLV (`03`, auch mit 3-Byte-Länge `03 FF hh ll`). Davor stehende NULL-, Lock-Control-, Memory-Control- und proprietäre TLVs werden übersprungen (NTAG213 hat ab Werk `01 03 A0 0C 34`). Darin zählt der erste Datensatz mit gültigem Spotify-Link:

| Datensatz | Beispiel |
|---|---|
| URI (`U`), alle Präfix-Codes 0x00 … 0x23 | so schreiben NFC Tools & Co. eine URL |
| Text (`T`), UTF-8 oder UTF-16 | „Hör mal: https://open.spotify.com/album/…“: der Link wird im Text gesucht |
| Smart Poster (`Sp`) | dessen URI-Datensatz |
| Absolute URI (TNF 3) | |

Andere Datensätze (z. B. Android-App-Record) werden übergangen.

**Link-Formen** (Groß-/Kleinschreibung bei Schema, Host und Typ egal, Leerraum außen egal):

- `https://open.spotify.com/<typ>/<id>`, auch `http://`, ohne Schema, mit `intl-de/` o. ä., `embed/`, Schrägstrich am Ende, `?si=…` und `#…`
- `spotify:<typ>:<id>`
- Altformen `open.spotify.com/user/<name>/playlist/<id>` und `spotify:user:<name>:playlist:<id>`
- `play.spotify.com/…` wie `open.spotify.com`

Typen: `track`, `album`, `playlist`, `artist`, `show`, `episode`. Die ID hat genau 22 Zeichen aus `0-9A-Za-z`.

**Tags beschreiben** (Gerät): Inhalt ist immer `03 <len> D1 01 <plen> 55 04 "open.spotify.com/<typ>/<id>" FE` (höchstens 56 Byte, passt auf jedes NTAG21x). Handys öffnen diesen https-Link direkt in der Spotify-App. Vor dem Schreiben prüft die Komponente CC (Seite 3), statische und dynamische Sperrbits und das Passwort (AUTH0). Ablauf: Längenbyte erst 0, dann Rest, dann echte Länge, dann zurücklesen und vergleichen. Ein abgebrochener Schreibvorgang hinterlässt so eine leere Nachricht statt halber Daten. Lock-Control-TLVs vor der Nachricht bleiben erhalten.

## Tag mit dem Handy beschreiben

1. In der Spotify-App bei Playlist, Album, Künstler, Titel oder Podcast: **Teilen → Link kopieren**.
2. In **NFC Tools** (Android/iOS): **Schreiben → Datensatz hinzufügen → URL/URI** → Link einfügen → OK.
3. **Schreiben** antippen und den Tag ans Handy halten.

Der Link darf `?si=…` enthalten. Ein Text-Datensatz mit dem Link geht ebenfalls. In NFC Tools **kein** Passwort setzen und den Tag **nicht sperren**: sonst kann der Streamer ihn nicht mehr neu beschreiben.

## Nicht unterstützt

- **Kurzlinks** `spotify.link/…`, `spotify.app.link/…`, `spoti.fi/…`: Sie lassen sich nur online auflösen (Fehler „short link“). Abhilfe: Link im Browser öffnen und die lange `open.spotify.com`-Adresse auf den Tag schreiben.
- **Private Sammlungen und andere Seiten**: „Lieblingssongs“ (`collection`), Nutzerprofile, Genres, Unterseiten wie `artist/<id>/discography` (Fehler „unsupported kind“).
- Gestückelte (chunked) NDEF-Datensätze, Tags mit Passwortschutz oder Sperrbits beim Schreiben, andere Tag-Typen (MIFARE Classic, Ultralight, NTAG I2C).

## Tests

`firmware/tools/host_test_nfc_tag.sh` übersetzt `firmware/tests/host/nfc_tag/test_nfc_tag.c` mit gcc und ASan/UBSan und führt die Tests aus (rund 2,95 Mio. Prüfungen, ca. 4 s). Abgedeckt sind:

- echte Speicherabbilder (NFC Tools auf NTAG215, Text-Datensatz, leerer Werkszustand `03 00 FE` und Android `D0 00 00`, NTAG213 mit Lock-Control-TLV, lange TLV- und Datensatzlängen, Smart Poster)
- alle Link-Formen, alle Fehlercodes und Rundläufe bauen → lesen
- schrittweises Lesen (`nfc_tag_ndef_bytes_needed`) und Schreibpläne für NTAG213/215
- 200 000 Fuzz-Durchläufe mit festem Startwert (Zufallsbytes und veränderte gültige Abbilder)

Zeilenabdeckung von `nfc_tag.c`: 100 %. Für den ESP32-S3 übersetzt die Datei ohne Warnungen (`-Wall -Wextra -Werror`).
