# Firmware bauen und flashen

Stand: Build-Spike vom 06./07.10.2026.

**Was davon wirklich ausgeführt wurde – bitte beim Lesen im Kopf behalten:**

| Umgebung | Stand |
|---|---|
| Linux (Ubuntu 24.04, Cloud-Container) | Alles in Abschnitt 9 und 10 wurde so ausgeführt: ESP-IDF-Installation, Firmware-Build aus frischem Klon, cspot-Kommandozeilen-Build, Protobuf-Erzeugung, Emulator-Test. |
| **Windows** | **Nicht ausgeführt.** Die Abschnitte 1–6 sind aus dem Linux-Ergebnis und der ESP-IDF-Dokumentation abgeleitet. Abweichungen bitte notieren. |
| Echte Hardware, echtes Spotify-Konto | **Nichts getestet.** Laufzeit-Belege gibt es nur aus dem Emulator (Abschnitt 10). |

Details, Messwerte und offene Risiken: [SPIKE_REPORT.md](SPIKE_REPORT.md).

---

## 1. Voraussetzungen (Windows 10/11)

* **ESP-IDF v5.5.x**, nativ installiert (kein WSL, kein PlatformIO), Ziel `esp32s3`.
  Geprüft wurde v5.5.5. Nicht ESP-IDF 6.x verwenden (Mbed TLS 4, mit cspot ungetestet).
* **Git** (bringt der ESP-IDF-Installer mit) – wird auch beim Bauen gebraucht:
  die mDNS-Komponente wird per Git von GitHub geholt.
* USB-C-Datenkabel zum Board. Ein Treiber ist unter Windows 10/11 nicht nötig.
* Rund 5 GB freier Platz, Internetzugang zu github.com beim ersten Build.
* Für Spotify: Premium-Konto und eine eigene App im
  [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) (Abschnitt 4).

Protobuf-Compiler, zusätzliche Python-Pakete oder `patch.exe` werden **nicht** gebraucht.

## 2. Pfade ohne Leerzeichen – das Wichtigste zuerst

Das Benutzerprofil `C:\Users\<Name>` enthält ein Leerzeichen. ESP-IDF und
seine Werkzeuge (CMake, Ninja, Kconfig, Linker-Skripte) kommen damit nicht
zuverlässig zurecht. Deshalb **nichts** unterhalb des Profils ablegen:

| Was | Empfohlener Ort | Nicht |
|---|---|---|
| ESP-IDF | `C:\esp\esp-idf` (im Installer `C:\esp` wählen) | `C:\Users\<Name>\esp` |
| ESP-IDF-Werkzeuge (`IDF_TOOLS_PATH`) | `C:\Espressif` oder `C:\esp\tools` | `%USERPROFILE%\.espressif` |
| Dieses Projekt | `C:\dev\music-streamer` | Desktop, Dokumente, OneDrive |
| Cache des Komponenten-Managers | `C:\esp\component-cache` (siehe unten) | `%LOCALAPPDATA%\Espressif\...` |

Zwei Dinge, die leicht übersehen werden:

* Der Komponenten-Manager legt seinen Git-Cache standardmäßig unter
  `%LOCALAPPDATA%\Espressif\ComponentManager\Cache` ab, also wieder im Profil
  mit Leerzeichen. Falls das Herunterladen von `espressif/mdns` scheitert, eine
  Umgebungsvariable setzen (Windows-Einstellungen → „Umgebungsvariablen für
  dieses Konto bearbeiten") und die ESP-IDF-Konsole neu öffnen:
  `IDF_COMPONENT_CACHE_PATH=C:\esp\component-cache`
* **Pfadlänge.** Kurzer Projektpfad hilft. Einmalig ausführen:
  `git config --global core.longpaths true`. Der längste Pfad eines
  vollständigen `--recursive`-Klons liegt bei 215 Zeichen unterhalb des
  Projektordners (in einem Submodul, das gar nicht gebraucht wird); mit dem
  sparsamen Klon aus Abschnitt 3 sind es 163. Im Build-Ordner kommen rund 130
  Zeichen plus die Länge des ESP-IDF-Pfads zusammen – ein Grund mehr für `C:\esp`.

## 3. Projekt holen

Das Repository hat noch kein Remote; es wird als Ordner oder Git-Bundle
weitergegeben. In beiden Fällen nach `C:\dev\music-streamer`.

cspot liegt als Git-Submodul unter
`firmware\components\spotify_connect\third_party\cspot`, darin `bell` als
weiteres Submodul. Die beiden Unter-Submodule von bell (`nlohmann_json`,
`mdnssvc`) braucht die Firmware nicht.

**Variante A – mit Git-Historie (Bundle oder später ein Remote):**

```
git clone <Bundle-Datei oder URL> C:\dev\music-streamer
cd C:\dev\music-streamer
git submodule update --init --depth 1 firmware/components/spotify_connect/third_party/cspot
git -C firmware/components/spotify_connect/third_party/cspot submodule update --init --depth 1 cspot/bell
```

(`git clone --recursive` geht auch, lädt aber die zwei ungenutzten Submodule mit.)

**Variante B – Ordner ohne `.git` kopiert:** cspot von Hand in genau der
festgelegten Version holen:

```
cd C:\dev\music-streamer\firmware\components\spotify_connect\third_party
rmdir cspot                      (nur falls ein leerer Ordner "cspot" vorhanden ist)
git clone https://github.com/philippe44/cspot.git cspot
cd cspot
git checkout 60cf17296e2ac9b903e86b92271cb6b4989a80af
git submodule update --init --depth 1 cspot/bell
```

bell muss danach auf `cfbc12072b565661f0ab2be3843b928f833f8691` stehen
(`git -C cspot/bell rev-parse HEAD`).

Fehlen die Quellen, bricht CMake ab mit
`spotify_connect: the cspot/bell sources are missing`.

## 4. Einmalige Einrichtung

### 4.1 ESP-IDF installieren

1. Den Windows-Installer von Espressif verwenden (ESP-IDF Installation Manager
   bzw. „ESP-IDF Tools Installer"), Version **v5.5.5** (oder eine andere
   5.5.x), Ziel **esp32s3**, Installationspfad **`C:\esp`**.
2. Prüfen, dass auch der Werkzeugpfad kein Leerzeichen enthält (Abschnitt 2).
3. Die vom Installer angelegte Verknüpfung „ESP-IDF 5.5 PowerShell" (oder CMD)
   öffnen. Sie richtet die Umgebung ein. Test: `idf.py --version` muss
   `ESP-IDF v5.5.x` ausgeben.

Die VS-Code-Erweiterung „ESP-IDF" kann auf dieselbe Installation zeigen; die
Befehle unten funktionieren in deren Terminal genauso.

### 4.2 Spotify-App anlegen (Client-ID und Client-Secret)

Der cspot-Fork braucht eine eigene Client-ID samt Secret, um die Adressen der
Audiodateien bei Spotify abzufragen. Ohne beides startet Spotify Connect nicht.

1. <https://developer.spotify.com/dashboard> → „Create app".
2. Name und Beschreibung beliebig. Als Redirect-URI zum Beispiel
   `http://127.0.0.1:8888/callback` (wird hier nicht benutzt, das Feld ist nur
   Pflicht). API: „Web API".
3. In den Einstellungen der App **Client ID** und **Client secret** ablesen.

### 4.3 Zugangsdaten eintragen

Alle Zugangsdaten sind standardmäßig leer. Ohne sie startet die Firmware, gibt
I2S aus und bleibt offline.

**Weg 1 – menuconfig:**

```
cd C:\dev\music-streamer\firmware
idf.py menuconfig
```

Menü **Music Streamer**:

| Eintrag | Inhalt |
|---|---|
| WiFi SSID / WiFi password | das WLAN zu Hause (2,4 GHz – der ESP32-S3 kann kein 5 GHz) |
| Spotify Connect device name | Name in der Geräteliste der Spotify-App, Standard „Music Streamer" |
| Spotify client ID / client secret | aus Abschnitt 4.2 |
| Spotify audio quality | 320 kbit/s (Standard) |

Mit `S` speichern, mit `Q` verlassen.

**Weg 2 – Datei (empfohlen, weil sie Neuaufbauten übersteht):**
`firmware\sdkconfig.secrets.example` nach `firmware\sdkconfig.secrets` kopieren,
ausfüllen und danach `firmware\sdkconfig` löschen (oder `idf.py fullclean`),
damit die Vorgaben neu eingelesen werden.

Wichtig bei Weg 1: Die Werte stehen nur in `firmware\sdkconfig`. Diese Datei
wird von `idf.py set-target` und beim Löschen neu aus den Vorgaben erzeugt –
die eingetragenen Zugangsdaten sind dann weg (die alte Datei bleibt als
`sdkconfig.old` liegen). Mit Weg 2 passiert das nicht.

`sdkconfig` und `sdkconfig.secrets` werden von Git ignoriert. Achtung: Das
Client-Secret und das WLAN-Passwort stehen derzeit im Klartext im
Firmware-Image. Images mit echten Werten nicht weitergeben oder veröffentlichen.

## 5. Bauen, flashen, Monitor

```
cd C:\dev\music-streamer\firmware
idf.py build
idf.py -p COMx flash monitor
```

* `idf.py set-target esp32s3` ist **nicht nötig**: das Ziel steht in
  `sdkconfig.defaults`. Der Befehl löscht den Build-Ordner und erzeugt
  `sdkconfig` neu; nach einem `menuconfig` ausgeführt, verwirft er die dort
  eingetragenen Zugangsdaten (siehe 4.3).
* Der erste Build holt die mDNS-Komponente von GitHub nach
  `firmware\managed_components` und dauert einige Minuten (Linux, 2 Kerne:
  knapp 2 Minuten für 918 Schritte; Windows ist erfahrungsgemäß langsamer).
* Geflasht wird über den **nativen USB-Anschluss** des Boards
  (USB-Serial-JTAG); dort erscheint auch die Konsole. Den COM-Port zeigt der
  Geräte-Manager. Taucht kein Port auf oder startet das Flashen nicht, das
  Board von Hand in den Download-Modus bringen: **BOOT** gedrückt halten und
  dabei das USB-Kabel einstecken (oder, falls das Board eine Reset-Taste hat,
  diese kurz drücken), BOOT loslassen, flashen, danach neu starten. Die
  COM-Nummer kann zwischen Bootloader und laufender Firmware wechseln.
* Monitor beenden: `Strg+]`.

Erwartetes Ergebnis des Builds: `build\music_streamer.bin`, etwa 1,42 MB. Die
App-Partition fasst 5 MB.

## 6. Erster Start

Was im Monitor erscheinen sollte (aus dem Emulator-Lauf übernommen, auf
Hardware noch nicht gesehen):

```
I (...) app: Music Streamer starting, PSRAM 8388608 bytes
I (...) audio_out: init: 44100 Hz, 16 bit stereo, ring 524288 bytes (PSRAM), BCK=5 WS=6 DOUT=7 XSMT=8
I (...) audio_out: started
I (...) wifi_sta: got ip ...
I (...) spotify: started, 320 kbit/s
I (...) sc_zeroconf: advertising "Music Streamer" (_spotify-connect._tcp, port 7864)
I (...) app: spotify: select "Music Streamer" in the Spotify app (same WiFi) to log in
```

Dann am Handy (im selben WLAN) in der Spotify-App das Gerät „Music Streamer"
auswählen. Nach erfolgreichem Login legt die Firmware wiederverwendbare
Zugangsdaten im NVS ab; bei späteren Starts meldet sie sich damit selbst an
(`using stored credentials of user ...`) und ist ohne Handy sofort erreichbar.

Alle 10 Sekunden gibt die Firmware den freien Heap aus (intern/PSRAM, jeweils
mit Tiefstwert). Der interne Wert ist der knappe; bitte beim ersten
Hardwaretest mitschreiben.

Gespeicherte Spotify-Anmeldung löschen: `idf.py -p COMx erase-flash` und neu
flashen (löscht den ganzen Flash, auch die WLAN-Kalibrierung – unkritisch).

## 7. Was auf dem Windows-PC anders ist als im Linux-Build

| Thema | Linux-Container (ausgeführt) | Windows-PC (erwartet) |
|---|---|---|
| ESP Component Registry | gesperrt → mDNS als Git-Abhängigkeit | Registry erreichbar; die Git-Abhängigkeit funktioniert trotzdem unverändert. Wer mag, kann in `components/spotify_connect/idf_component.yml` die drei Zeilen `git/path/version` durch `version: "~1.14.0"` ersetzen (nicht ausprobiert). |
| `dl.espressif.com` | gesperrt → Constraints-Datei von Hand, kein zusätzlicher pip-Index | erreichbar; der Installer erledigt das. Es werden dadurch Espressifs getestete Python-Paketversionen installiert, hier waren es die jeweils neuesten. |
| Python / protobuf | Für den Firmware-Build egal. Nur zum Neu-Erzeugen der Protobuf-Dateien nötig: Python `protobuf==3.20.3` und `setuptools<81` (Abschnitt 9.2) | genauso. Im normalen Build wird kein protoc und kein Python-protobuf gebraucht, weil der erzeugte Code eingecheckt ist. |
| Pfade | keine Leerzeichen, kurze Pfade | Abschnitt 2 beachten |
| Zeilenenden | LF | Git für Windows wandelt in den Submodulen evtl. nach CRLF. Unkritisch: Prüfsummen und Overlays normalisieren das; das eigene Repository erzwingt LF (`.gitattributes`). |
| Werkzeugversionen | CMake 3.28.3, Ninja 1.11.1, Python 3.13.16 | die vom Installer mitgebrachten |
| Emulator-Test | lief | Skript ist Bash; unter Windows nur in WSL |

## 8. Stolperfallen, die im Spike tatsächlich aufgetreten sind

| Symptom | Ursache / Abhilfe |
|---|---|
| `error: 'strstr' was not declared in this scope` (bell, `URLParser.cpp:22`) | Einziger Compilerfehler von cspot+bell mit GCC 14 / ESP-IDF 5.5. Wird beim Build automatisch behoben (Overlay 1, `components/spotify_connect/patches/`). Tritt nur auf, wenn das Overlay entfernt wurde. |
| Absturz `LoadProhibited` in `ApResolve::fetchFirstApAddress` direkt nach dem Login-Versuch | cspot wertet die Antwort von `apresolve.spotify.com` ungeprüft aus. Im Emulator reproduziert; behoben durch Overlay 3. Ohne Overlay startet das Gerät in dieser Lage endlos neu. |
| `ModuleNotFoundError: No module named 'pkg_resources'` bzw. `AttributeError: module 'google.protobuf.reflection' has no attribute 'MakeClass'` | Nur beim cspot-eigenen CMake-Build bzw. beim Neu-Erzeugen der Protobuf-Dateien: der mitgelieferte nanopb-Generator (0.4.6-dev) läuft nicht mit aktuellem setuptools/protobuf. Abhilfe in 9.2. Der Firmware-Build ist nicht betroffen. |
| `Failed to download .../espidf.constraints.v5.5.txt` | `dl.espressif.com` nicht erreichbar (nur im Container). Abschnitt 9.1. |
| mDNS-Komponente lässt sich nicht laden | Component Registry nicht erreichbar (Container) → deshalb Git-Abhängigkeit. Unter Windows zusätzlich möglich: Git nicht im Pfad oder Leerzeichen im Cache-Pfad → `IDF_COMPONENT_CACHE_PATH` (Abschnitt 2). |
| Änderungen in `sdkconfig.defaults` / `sdkconfig.secrets` wirken nicht | `sdkconfig` existiert schon. Löschen oder `idf.py fullclean`. |
| `generated/protobuf does not match ...` | cspot-Submodul wurde auf eine andere Version gesetzt → 9.2. |
| `spotify_connect overlay: text not found ...` | cspot/bell wurde aktualisiert; das Overlay passt nicht mehr → `patches/README.md`. |
| Emulator: `nvs_flash_init` scheitert, QEMU stürzt beim ersten Flash-Schreiben ab, DH-Schlüsselerzeugung hängt | Eigenheiten von QEMU, siehe Abschnitt 10. Betrifft die Hardware nicht. |

Noch nicht aufgetreten, aber auf Hardware zu erwarten:

| Symptom | Ursache / Abhilfe |
|---|---|
| Bootschleife mit `PSRAM chip is not connected, or wrong PSRAM line mode` | falsche Board-Variante: die Firmware erwartet 8 MB **Octal**-PSRAM (ESP32-S3R8) |
| Kein Ton, obwohl „playing" | Lötbrücken des DAC-Moduls (SCK → GND, FMT = L), XSMT an GPIO8, Verdrahtung BCK/LCK/DIN = GPIO5/6/7 |

## 9. Linux – so wurde tatsächlich gebaut

Der Container erreichte GitHub und PyPI, aber **nicht** `dl.espressif.com` und
**nicht** die ESP Component Registry. Mit *(nur Container)* markierte Schritte
sind Umgehungen dafür und auf einem normalen Rechner überflüssig.

### 9.1 ESP-IDF und Firmware

```
# 1. ESP-IDF v5.5.5 von GitHub (flach, mit Submodulen)
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git

# 2. Toolchain + Python-Umgebung
export IDF_TOOLS_PATH=~/esp/tools          # beliebiger Pfad ohne Leerzeichen; Standard ist ~/.espressif
#   (nur Container) Die pip-"Constraints"-Datei kommt sonst von dl.espressif.com.
#   install.sh benutzt eine vorhandene Datei, die jünger als ein Tag ist, ohne Download.
mkdir -p $IDF_TOOLS_PATH
cat > $IDF_TOOLS_PATH/espidf.constraints.v5.5.txt <<'EOT'
esptool>=4.10,<5
esp-idf-kconfig>=2.5.0,<3
esp-idf-size>=1.6,<3
esp-idf-monitor>=1.6,<2
esp-coredump>=1.13,<2
idf-component-manager>=2.2,<3
esp-idf-nvs-partition-gen<1
esp-idf-panic-decoder<2
pyparsing>=3.1,<3.3
EOT
#   (nur Container) IDF_PIP_WHEELS_URL="" entfernt den Zusatz-Index https://dl.espressif.com/pypi;
#   alles Nötige liegt auf PyPI.
IDF_PIP_WHEELS_URL="" ~/esp/esp-idf/install.sh esp32s3
#   Die Toolchain-Archive kommen standardmäßig von GitHub-Releases.

# 3. Bauen
. ~/esp/esp-idf/export.sh                  # IDF_TOOLS_PATH muss noch gesetzt sein
cd <Projekt>/firmware
idf.py set-target esp32s3
idf.py build
```

Folge der handgeschriebenen Constraints: Es wurden schlicht die neuesten
Python-Pakete unterhalb der nächsten Hauptversion installiert (esptool 4.12.0,
idf-component-manager 2.5.2, esp-idf-kconfig 2.5.4 …), nicht Espressifs
geprüfter Satz.

### 9.2 Protobuf-Dateien neu erzeugen (nur nach einem cspot-Update)

Nötig, wenn CMake meldet, dass `generated/protobuf` nicht zum Submodul passt.

```
sudo apt install protobuf-compiler               # protoc 3.21.12
python3 -m venv pbvenv && . pbvenv/bin/activate  # NICHT die ESP-IDF-Umgebung benutzen
pip install "protobuf==3.20.3" "setuptools<81"
python firmware/components/spotify_connect/tools/gen_protos.py
```

Windows: `protoc-21.12-win64.zip` von den GitHub-Releases von
protocolbuffers/protobuf in den Pfad legen, sonst gleich (nicht ausprobiert).

Die eingecheckten Dateien sind byte-identisch mit dem, was der cspot-eigene
CMake-Build erzeugt (im Spike mit `cmp` gegen einen unabhängigen Build geprüft).

### 9.3 cspot-Kommandozeilenplayer bauen (Gegenprobe, ob der Fork selbst baut)

```
sudo apt install libmbedtls-dev portaudio19-dev libavahi-client-dev protobuf-compiler
git clone --recurse-submodules --shallow-submodules https://github.com/philippe44/cspot
git -C cspot checkout 60cf17296e2ac9b903e86b92271cb6b4989a80af
python3 -m venv pbvenv && . pbvenv/bin/activate
pip install "protobuf==3.20.3" "setuptools<81"
cmake -G Ninja -S cspot/targets/cli -B cli-build -DUSE_PORTAUDIO=ON
ninja -C cli-build                                # -> cli-build/cspotcli (408 Schritte)
```

Gebaut und gelinkt, nicht gegen Spotify gestartet. Mit aktuellem
`protobuf`/`grpcio-tools`/`setuptools` scheitert schon die Code-Erzeugung
(siehe Tabelle in Abschnitt 8). Ein gescheiterter Versuch hinterlässt
`generator/proto/nanopb_pb2.py` im bell-Ordner; vor dem nächsten Versuch mit
`git -C cspot/cspot/bell clean -fdX` entfernen.

### 9.4 cspot aktualisieren

```
cd firmware/components/spotify_connect/third_party/cspot
git fetch origin && git checkout <Commit> && git submodule update --init cspot/bell
```

Danach bauen. CMake bricht mit einer von drei eindeutigen Meldungen ab, falls
etwas nachzuziehen ist: veralteter Protobuf-Code (→ 9.2), ein Overlay, dessen
Suchtext nicht mehr passt (→ `patches/README.md`), oder neue Quelldateien, die
eine abgeschaltete Funktion brauchen.

## 10. Rauchtest im Emulator (ohne Hardware)

Espressifs QEMU emuliert genug vom ESP32-S3, um die Firmware zu starten, eine
emulierte Netzwerkkarte hochzufahren und Spotify Connect bis zum Login-Versuch
laufen zu lassen. WLAN, I2S und Display werden nicht emuliert.

```
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa     # einmalig; danach export.sh erneut ausführen
#   Ubuntu: sudo apt install libslirp0 libpixman-1-0 libsdl2-2.0-0 libgcrypt20
firmware/tools/qemu_smoke_test.sh
```

Das Skript baut eine zweite Konfiguration in `firmware/build-qemu`
(`sdkconfig.defaults` + `sdkconfig.qemu`: UART-Konsole, emuliertes Ethernet
statt WLAN, Software-Krypto, Quad-PSRAM, Platzhalter als Client-ID) und prüft:

1. Start bis „wartet auf Zeroconf-Login"; `GET /spotify_info?action=getInfo`
   liefert den öffentlichen Schlüssel; ein kaputter `addUser`-Aufruf wird
   abgelehnt; die Anwendung erhält das Ereignis.
2. Ein **gefälschter**, formal korrekter Login (`tools/zeroconf_fake_login.py`
   spielt die Rolle der Spotify-App mit erfundenen Zugangsdaten) wird
   entschlüsselt; der anschließende Anmeldeversuch scheitert und wird ohne
   Absturz wiederholt.

Die emulierte Maschine hat dabei standardmäßig keinen Weg nach außen (QEMU
`restrict=on`): Der Test läuft überall gleich ab und sendet nie etwas an
Spotify. Mit `QEMU_NET_OPEN=1 firmware/tools/qemu_smoke_test.sh` darf die
Firmware über den Rechner ins Internet; der erfundene Login geht dann wirklich
an Spotify und wird dort abgelehnt – nur bewusst benutzen.

Interaktiv nach dem Build:

```
cd firmware
idf.py -B build-qemu qemu --qemu-extra-args "-m 8M -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:7864-:7864"
```

Eigenheiten von QEMU 9.2.2 (`esp_develop_20260417`), alle in `sdkconfig.qemu`
bzw. im Skript berücksichtigt:

* `-m 8M` ist nötig. Mit den 32 MB PSRAM, die `idf.py qemu` vorgibt, ist der
  Datenadressraum erschöpft und `nvs_flash_init()` scheitert
  (`esp_mmu_map: no such vaddr range`).
* Octal-PSRAM-Emulation lässt QEMU selbst beim ersten Flash-Schreibzugriff
  abstürzen → die QEMU-Konfiguration nutzt Quad-PSRAM.
* Der MPI-Beschleuniger (RSA) ist nicht emuliert: die
  Diffie-Hellman-Schlüsselerzeugung hängt, solange
  `CONFIG_MBEDTLS_HARDWARE_MPI` an ist.
* `i2s_channel_write: ESP_ERR_TIMEOUT` (etwa einmal pro Sekunde, gedrosselt
  protokolliert) und `esp_eth ... mac filter not supported` sind Folgen
  fehlender Peripherie.

**Ein `build-qemu`-Image nie auf das Board flashen.**

## 11. Wie der Fremdcode gebaut wird

`components/spotify_connect` enthält unseren Verbindungscode und cspot/bell:

* `cmake/cspot_bell.cmake` übersetzt die Quellen des Submoduls zu zwei
  statischen Bibliotheken (`libcspot.a`, `libbell.a`). **Die CMakeLists.txt von
  cspot und bell werden nicht benutzt** – sie würden den nanopb-Generator im
  Build starten (siehe 9.2) und binden feste ESP-IDF-Komponenten ein. Die
  Funktionsauswahl entspricht squeezelite-esp32
  (`BELL_DISABLE_CODECS/SINKS/FMT/REGEX/MQTT/WEBSERVER`, `BELL_ONLY_CJSON`):
  keine Opus/AAC/MP3-Decoder, kein civetweb, kein nlohmann_json, kein fmt.
  Vorbis dekodiert tremor (Ganzzahl).
* `generated/protobuf/` enthält die nanopb-Ausgabe für die `.proto`-Dateien
  von cspot. Eine Prüfsummenprüfung beim Konfigurieren stellt sicher, dass sie
  zum Submodul passt.
* `patches/source_overlays.cmake` wendet drei kleine Änderungen auf *Kopien*
  von drei Quelldateien im Build-Ordner an; das Submodul bleibt unberührt.
  Begründungen und dieselben Änderungen als Patch-Dateien: `patches/`.
* Die Audio-Senken von bell werden nicht gebaut. `components/audio_out` ist
  unsere Senke (Treiber `i2s_std`, kein APLL, kein MCLK).
* mDNS-Anmeldung und der Zeroconf-HTTP-Endpunkt stehen in `src/sc_zeroconf.cpp`
  (ESP-IDF `esp_http_server` + mDNS-Komponente).

`firmware/CMakeLists.txt` setzt `MINIMAL_BUILD`: Es werden nur Komponenten
gebaut, die `main` direkt oder indirekt verlangt. Eine neue Komponente muss
ihre Abhängigkeiten in `REQUIRES`/`PRIV_REQUIRES` nennen, und `main` muss sie
verlangen.

`dependencies.lock` **ist eingecheckt**: Die Datei hält den genauen
mDNS-Commit und die ESP-IDF-Version fest. Ein Build mit einer anderen 5.5.x
schreibt sie neu – die Änderung einchecken oder verwerfen, beides ist in Ordnung.
