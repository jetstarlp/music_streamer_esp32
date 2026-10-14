# Build-Spike: cspot auf dem ESP32-S3 mit ESP-IDF 5.5

Datum: 06./07.10.2026. Umgebung: Linux-Cloud-Container (Ubuntu 24.04, 2 Kerne),
keine Hardware, kein Spotify-Konto.

## 1. Antwort auf die Spike-Frage

**Ja: `philippe44/cspot` (Commit `60cf172`) samt `bell` kompiliert und linkt
für den ESP32-S3 mit ESP-IDF v5.5.5.** Aus einem frischen Klon läuft
`idf.py set-target esp32s3 && idf.py build` mit Exit-Code 0 durch; cspot ist
dabei vollständig eingebunden, nichts ist durch Attrappen ersetzt.

Zum Bauen nötig war genau **eine** Quelltextkorrektur (ein fehlendes
`#include <cstring>` in bell). Zwei weitere kleine Änderungen an cspot dienen
der Robustheit (Abschnitt 5.2); alle drei werden beim Build auf Kopien
angewandt, das Submodul selbst bleibt unverändert. Alles andere sind
Entscheidungen im Build-System.

Was der Spike **nicht** beantwortet: ob die Firmware auf dem Board Musik
spielt. Es lief nichts auf Hardware und nichts gegen Spotify. Die einzigen
Laufzeit-Belege stammen aus dem Emulator (Abschnitt 7) und reichen bis zum
ersten Schritt der Spotify-Anmeldung.

## 2. Meilensteine

| # | Meilenstein | Ergebnis | Beleg |
|---|---|---|---|
| 1 | ESP-IDF v5.5.5 + Toolchain, `hello_world` für esp32s3 | **erreicht** | frische Kopie von `examples/get-started/hello_world`: `idf.py set-target esp32s3 && idf.py build` → Exit 0, `hello_world.bin` 162 176 Bytes |
| 2 | `cspot/targets/cli` baut auf dem Linux-Host | **erreicht** | frischer Klon bei `60cf172`, `cmake -G Ninja … -DUSE_PORTAUDIO=ON && ninja` → Exit 0, 408 Schritte, `cspotcli` 1 667 144 Bytes. Nur mit Python `protobuf==3.20.3` und `setuptools<81`; mit aktuellen Paketen scheitert die Code-Erzeugung (Abschnitt 5.3). Nicht gestartet. |
| 3 | `firmware/` nur mit board + audio_out + main | **erreicht** | Wegwerfkopie des Baums ohne `components/spotify_connect`: `idf.py set-target esp32s3 && idf.py build` → Exit 0, `music_streamer.bin` 784 928 Bytes |
| 4 | cspot + bell kompilieren im ESP-IDF-Build | **erreicht** | 17 cspot-, 35 bell- und 5 nanopb-Quellen, übersetzt mit den ESP-IDF-Flags (`-mlongcalls -fno-rtti -O2 -std=gnu++2b -fexceptions`). Gegenprobe ohne Overlays mit `ninja -k 0`: genau ein Fehler im ganzen Build (Abschnitt 5.2). |
| 5 | Vollständiger Link aus sauberem Baum | **erreicht** | frischer `git clone` + Submodule, `idf.py set-target esp32s3 && idf.py build` → Exit 0, 918 Schritte, 0 Warnungen, `music_streamer.bin` 1 420 272 Bytes (27 % des 5-MB-Slots) |
| 6 | Wrapper gegen squeezelites `Shim.cpp` geprüft | **erreicht** (Lesen + Emulator) | Abschnitt 7 und 8. Dabei einen Absturz in cspot gefunden und behoben (Overlay 3). |

Die Zahlen in Zeile 5 stammen aus dem Clean-Build des Code-Standes `326b0f6`.
Danach wurden nur noch Dokumente, Kommentare und Testskripte geändert; der
abschließende Clean-Build des Endstandes wurde zur Kontrolle wiederholt
(gleiche Größe).

**Zum Ablauf:** Der Spike lief in zwei Durchgängen. Der erste (06.10. abends)
hatte ESP-IDF installiert und das Firmware-Gerüst samt Emulator-Test angelegt.
Im zweiten wurde nichts davon übernommen, ohne es nachzuprüfen: alle
Meilensteine sind aus frischen Kopien bzw. Klonen neu gebaut, die
eingecheckten Protobuf-Dateien gegen einen unabhängigen Build verglichen, der
Emulator-Test wiederholt und um einen gefälschten Login erweitert.

## 3. Versionen

| Teil | Version |
|---|---|
| ESP-IDF | **v5.5.5**, Commit `b774170ff46c393eeb5e495ea37936038d3f4f4f` (16.07.2026) |
| Toolchain | `xtensa-esp-elf` **esp-14.2.0_20260121** (GCC 14.2.0), von `install.sh esp32s3` aus GitHub-Releases |
| Enthalten in ESP-IDF | Mbed TLS 3.6.6, cJSON 1.7.19 |
| Python-Werkzeuge | Python 3.13.16, esptool 4.12.0, idf-component-manager 2.5.2, esp-idf-kconfig 2.5.4, esp-idf-size 2.3.2 |
| Host | CMake 3.28.3, Ninja 1.11.1, protoc 3.21.12 (nur für Meilenstein 2) |
| cspot | `philippe44/cspot` @ `60cf17296e2ac9b903e86b92271cb6b4989a80af` (06.10.2026, zugleich `master`), Git-Submodul |
| bell | `philippe44/bell` @ `cfbc12072b565661f0ab2be3843b928f833f8691`, Submodul von cspot |
| nanopb | 0.4.6-dev, in bell enthalten; erzeugter Code ist eingecheckt |
| Vorbis-Decoder | tremor (Ganzzahl), in bell enthalten |
| mDNS | `espressif/mdns` 1.14.0 = `esp-protocols` @ `5c4650d0ab6f9fde165ae1e3c1ef16714be0a619` (Tag `mdns-v1.14.0`), als Git-Abhängigkeit |
| Emulator | QEMU 9.2.2 `esp_develop_9.2.2_20260417` |
| Referenz | squeezelite-esp32 `master-v4.3` @ `1ac12a9` (`components/spotify/Shim.cpp`), nur gelesen |

## 4. Was angelegt wurde

```
firmware/
  main/                       app_main, WLAN-Station, Kconfig-Menü "Music Streamer", Heap-Log
  components/board/           board_pins.h (alle Pins)
  components/audio_out/       i2s_std-Senke, Ringpuffer (512 kB im PSRAM), Feeder-Task (Prio 18), XSMT-Ablauf
  components/spotify_connect/ C-API ohne cspot-Typen; Zeroconf (mDNS + HTTP), NVS-Zugangsdaten, Ereignisse
    third_party/cspot         Submodul (darin bell)
    generated/protobuf        eingecheckte nanopb-Ausgabe + Prüfsummen
    cmake/cspot_bell.cmake    baut cspot/bell als zwei statische Bibliotheken
    patches/                  3 Overlays (CMake) + dieselben Änderungen als .patch-Dateien
    tools/gen_protos.py       erzeugt generated/protobuf neu
  tools/qemu_smoke_test.sh    Emulator-Test, tools/zeroconf_fake_login.py spielt dabei die Spotify-App
  partitions.csv, sdkconfig.defaults, sdkconfig.qemu, sdkconfig.secrets.example
```

Abweichungen von der Vorgabe, bewusst:

* `spotify_connect_start()` nimmt eine Konfigurationsstruktur (Gerätename,
  Client-ID, Client-Secret, Bitrate, Port, Ereignis-Callback) statt dreier
  Einzelargumente.
* Die Option aus der Vorgabe, `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`,
  heißt in ESP-IDF 5.5 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`. Der alte
  Name ist nur noch Alias. Gesetzt ist der neue; im erzeugten `sdkconfig`
  stehen beide auf `y`.
* `MBEDTLS_DYNAMIC_BUFFER` ist aus: alle Mbed-TLS-Allokationen liegen ohnehin
  im PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC`), der dynamische Puffer würde keinen
  internen RAM sparen.
* Änderungen an Fremdcode sind CMake-Overlays statt angewandter Patches
  (Begründung in 5.2). Die Patch-Dateien liegen zusätzlich als Referenz bei.

## 5. Alle Patches und Workarounds

### 5.1 Build-Umgebung (nur wegen der Netzsperren im Container)

| Workaround | Grund |
|---|---|
| `espidf.constraints.v5.5.txt` von Hand angelegt | `install.sh` will die Datei von `dl.espressif.com` laden (gesperrt). Eine vorhandene, weniger als einen Tag alte Datei wird ohne Download benutzt. Folge: neueste statt von Espressif geprüfte Python-Pakete. |
| `IDF_PIP_WHEELS_URL=""` beim `install.sh` | entfernt den pip-Zusatzindex `https://dl.espressif.com/pypi` |
| mDNS als `git:`-Abhängigkeit (`esp-protocols`, Pfad `components/mdns`, fester Commit) | ESP Component Registry gesperrt. Funktioniert auch mit Registry-Zugang. |

### 5.2 cspot/bell im ESP-IDF-Build

| Maßnahme | Art | Grund |
|---|---|---|
| Eigenes `cmake/cspot_bell.cmake` statt der CMakeLists von cspot/bell | Build-System | Upstream startet im Build den nanopb-Generator (braucht protoc + altes Python-protobuf, siehe 5.3) und bindet `idf::driver` und einen festen mDNS-Namen ein. Funktionsauswahl wie squeezelite: `BELL_DISABLE_CODECS/SINKS/FMT/REGEX/MQTT/WEBSERVER`, `BELL_ONLY_CJSON`, cJSON aus ESP-IDF, Vorbis = tremor. |
| nanopb-Ausgabe eingecheckt (`generated/protobuf`), SHA-256-Prüfung gegen die `.proto`/`.options` des Submoduls beim Konfigurieren | Build-System | Der Firmware-Build braucht so weder protoc noch Python-protobuf. Die Dateien sind byte-identisch mit dem Ergebnis des unabhängigen CLI-Builds aus Meilenstein 2 (`cmp`). |
| **Overlay 1:** `bell/main/io/URLParser.cpp` + `#include <cstring>` | **Build-Fix, nötig** | einziger Compilerfehler unter GCC 14 (unten) |
| **Overlay 2:** `cspot/src/MercurySession.cpp`, Stack von `mercury_dispatcher` 4 kB → 16 kB | Robustheit, nicht nötig zum Bauen, **ungetestet** | Nach einem Verbindungsabbruch führt dieser Task selbst den Neuaufbau aus, samt TLS-Handshake. 4 kB sind dafür knapp; der Stack liegt im PSRAM. |
| **Overlay 3:** `cspot/src/ApResolve.cpp` prüft das geparste JSON und wirft sonst eine Exception | **Absturz-Fix**, nicht nötig zum Bauen, Fehlerpfad im Emulator getestet | siehe 7.2 |
| `bell/main/platform/esp/MDNSService.cpp` nicht mitgebaut | Build-System | mDNS-Anmeldung macht `sc_zeroconf.cpp` direkt mit der ESP-IDF-Komponente |
| bells Audio-Senken nicht mitgebaut (`BELL_DISABLE_SINKS`) | Build-System | `PCM5102AudioSink` nutzt den alten I2S-Treiber + APLL; eigene Senke `audio_out` |
| `-w` für libcspot/libbell | Build-System | Warnungen in Fremdcode sollen den Build weder abbrechen noch fluten |
| `CONFIG_COMPILER_CXX_EXCEPTIONS=y`, `CONFIG_FREERTOS_HZ=1000` | Konfiguration | cspot wirft und fängt Exceptions; cspot/bell schlafen in Millisekundenschritten |

Ein Overlay kopiert die betroffene Datei beim Konfigurieren nach
`build/sc_overlay/` und ersetzt dort Text; das Submodul bleibt unverändert
(`git status` darin ist leer). Passt der Suchtext nach einem Submodul-Update
nicht mehr, bricht CMake mit einer klaren Meldung ab. Gegenüber
`git apply`/`patch` spart das `patch.exe` unter Windows und ist unempfindlich
gegen CRLF-Umwandlung. Die drei Änderungen liegen zusätzlich als
`patches/000x-*.patch` vor; `git apply --check` gegen die festgelegten Commits
ist geprüft.

Der einzige Compilerfehler ohne Overlays (`ninja -k 0`, also bis zum Ende
durchgebaut):

```
FAILED: esp-idf/spotify_connect/CMakeFiles/bell.dir/third_party/cspot/cspot/bell/main/io/URLParser.cpp.obj
.../cspot/bell/main/io/URLParser.cpp: In member function 'void bell::URLParser::parse(const char*, std::vector<std::__cxx11::basic_string<char> >&)':
.../cspot/bell/main/io/URLParser.cpp:22:9: error: 'strstr' was not declared in this scope
   22 |   url = strstr(url, match[2].c_str()) + match[2].size();
      |         ^~~~~~
.../cspot/bell/main/io/URLParser.cpp:2:1: note: 'strstr' is defined in header '<cstring>'; this is probably fixable by adding '#include <cstring>'
ninja: build stopped: cannot make progress due to previous errors.
```

### 5.3 nanopb/protobuf (betrifft nur Meilenstein 2 und das Neu-Erzeugen)

Die Vorab-Recherche („protobuf 4.x bricht die Erzeugung") stimmt in der Sache,
der erste Fehler ist heute aber ein anderer. Mit protobuf 7.36.2,
grpcio-tools 1.84.0, setuptools 84.0.0:

```
  File ".../nanopb/generator/proto/__init__.py", line 7, in <module>
    import pkg_resources
ModuleNotFoundError: No module named 'pkg_resources'
--nanopb_out: protoc-gen-nanopb: Plugin failed with status code 1.
```

Mit `setuptools<81` (80.10.2), aber weiterhin protobuf 7.36.2 folgt
`AttributeError: module 'google.protobuf.reflection' has no attribute 'MakeClass'`.
Mit `protobuf==3.20.3` + `setuptools<81` + protoc 3.21.12 läuft die Erzeugung.

### 5.4 Emulator (nur `sdkconfig.qemu`)

Quad- statt Octal-PSRAM, Software-Krypto statt Hardware-AES/SHA/MPI, emuliertes
Ethernet statt WLAN, UART-Konsole, QEMU mit `-m 8M`. Gründe in
[BUILD.md](BUILD.md), Abschnitt 10. Diese Abweichungen begrenzen, was der
Emulator-Test über die Hardware aussagt.

## 6. Build-Ergebnis

Auszug aus dem Clean-Build (frischer Klon, `idf.py set-target esp32s3 && idf.py build`):

```
NOTICE: Processing 2 dependencies:
NOTICE: [1/2] espressif/mdns (5c4650d0ab6f9fde165ae1e3c1ef16714be0a619)
NOTICE: [2/2] idf (5.5.5)
...
[874/918] Linking CXX static library esp-idf/spotify_connect/libbell.a
[910/918] Building CXX object esp-idf/spotify_connect/CMakeFiles/cspot.dir/__/__/sc_overlay/src/MercurySession.cpp.obj
[911/918] Linking CXX static library esp-idf/spotify_connect/libcspot.a
[912/918] Linking C static library esp-idf/spotify_connect/libspotify_connect.a
[916/918] Linking CXX executable music_streamer.elf
[917/918] Generating binary image from built executable
Bootloader binary size 0x5170 bytes. 0x2e90 bytes (36%) free.
music_streamer.bin binary size 0x15abf0 bytes. Smallest app partition is 0x500000 bytes. 0x3a5410 bytes (73%) free.
Project build complete.
```

Dass echter cspot-Code gelinkt ist, zeigt die Symboltabelle der ELF-Datei
(`cspot::SpircHandler::handleFrame`, `cspot::TrackPlayer::runTask`,
`cspot::MercurySession::reconnect`, `cspot::CDNAudioFile::readBytes`, `ov_read`,
`pb_decode`, `bell::TLSSocket::open` …) und der Platzbedarf unten.

### Größe

| | Bytes | |
|---|---|---|
| `music_streamer.bin` | 1 420 272 | 27,1 % des 5-MB-Slots, 3 822 608 Bytes frei |
| `bootloader.bin` | 20 848 | |
| zum Vergleich ohne spotify_connect (Meilenstein 3) | 784 928 | Spotify Connect kostet rund 635 kB Flash |

`idf.py size`:

```
┃ Memory Type/Section ┃ Used [bytes] ┃ Used [%] ┃ Remain [bytes] ┃ Total [bytes] ┃
│ Flash Code          │      1049568 │          │                │               │
│ Flash Data          │       250776 │          │                │               │
│    .rodata          │       214968 │          │                │               │
│    .eh_frame        │        35552 │          │                │               │
│ DIRAM               │       130475 │    38.18 │         211285 │        341760 │
│    .text            │        81715 │    23.91 │                │               │
│    .bss             │        27072 │     7.92 │                │               │
│    .data            │        21688 │     6.35 │                │               │
│ IRAM                │        16384 │    100.0 │              0 │         16384 │
│    .text            │        15356 │    93.73 │                │               │
│    .vectors         │         1028 │     6.27 │                │               │
```

Lesehilfe: „IRAM 100 %" ist kein Alarm. Das ist der 16 kB große reine
Code-Bereich; weiterer RAM-Code liegt im gemeinsamen Bereich DIRAM (dort
`.text` 81 715 Bytes). Für den internen Heap zählt die DIRAM-Zeile: statisch
belegt sind 130 475 von 341 760 Bytes.

Größte Beiträge (`idf.py size-components`, Gesamtgröße): libstdc++ 180 705,
libnet80211 149 609, **libcspot 107 318**, libc 104 816, **libbell 103 504**,
liblwip 100 963, libmbedcrypto 78 315, mDNS 39 507, libspotify_connect 24 295.
cspot und bell belegen zusammen nur 497 Bytes `.bss`/`.data`; der
squeezelite-Kniff, das `.bss` von cspot ins PSRAM zu legen, ist hier unnötig.
Im Image steckt auch das CA-Bündel von ESP-IDF (rund 69 kB), das cspot nicht
benutzt (Abschnitt 10, Punkt 6).

### Heap zur Laufzeit – nur Emulator

Mit Ethernet statt WLAN und Quad-PSRAM, also nur als Größenordnung:

| Zeitpunkt | intern frei | intern Tiefstwert | PSRAM frei |
|---|---|---|---|
| Start von `app_main` | 354 kB | 290 kB | 8 386 kB |
| Netz oben, Audio gestartet (Ringpuffer 512 kB im PSRAM) | 304 kB | 236 kB | 7 844 kB |
| Spotify wartet auf Login | 279 kB | 212 kB | 7 811 kB |
| während wiederholter TLS-Verbindungsversuche | 279 kB | 199 kB | 7 811 kB (Tiefstwert 7 777 kB) |

Auf Hardware kommt der WLAN-Treiber hinzu (erfahrungsgemäß einige zehn kB
interner RAM), später LVGL. Messen, nicht schätzen: die Firmware gibt die Werte
alle 10 s aus.

## 7. Laufzeit-Belege aus dem Emulator

`firmware/tools/qemu_smoke_test.sh`, zuletzt auf einem frischen Klon: beide
Stufen bestanden. Das Skript gibt der emulierten Maschine standardmäßig keinen
Weg nach außen (QEMU `restrict=on`), damit der Test überall gleich abläuft und
nie etwas an Spotify gesendet wird:

```
PASS (1/2): firmware boots in QEMU, Spotify Connect waits for a Zeroconf login
PASS (2/2): fake login blob decrypted, login attempt handled without a crash (network error, retried: mbedtls_ssl_handshake error)
```

Mit `QEMU_NET_OPEN=1` darf die Firmware über den Host nach außen. Im Container
ergab das einen zweiten, anderen Fehlerpfad (7.2):

```
PASS (2/2): fake login blob decrypted, login attempt handled without a crash (network error, retried: apresolve: unexpected response)
```

### 7.1 Was dabei wirklich ausgeführt wurde

* Bootloader, Partitionstabelle, App aus `ota_0`, PSRAM-Heap, NVS-Init.
* `audio_out_init`/`audio_out_start` (Ringpuffer im PSRAM, I2S-Kanal angelegt,
  XSMT-Ablauf). Der Feeder läuft, aber der emulierte I2S nimmt keine Daten ab
  (`i2s_channel_write: ESP_ERR_TIMEOUT`) – der Datenpfad ist damit **nicht** belegt.
* `spotify_connect_start`: Zugangsdaten aus dem NVS lesen (drei Fälle
  getestet: nichts gespeichert; gültiges JSON → „using stored credentials of
  user …", kein Zeroconf; kaputtes JSON → verworfen, Zeroconf).
* Session-Task mit 32-kB-Stack im PSRAM, Diffie-Hellman-Schlüsselerzeugung,
  HTTP-Server, mDNS-Registrierung (Aufrufe erfolgreich; ob die Ankündigung im
  Netz ankommt, zeigt der Emulator nicht), Ereignis an die Anwendung.
* `GET /spotify_info?action=getInfo`, Ablehnung eines verstümmelten `addUser`.
* **Gefälschter Login:** `zeroconf_fake_login.py` rechnet die Gegenseite des
  Schlüsseltauschs und baut einen formal korrekten Login-Blob mit erfundenen
  Zugangsdaten. Das Gerät entschlüsselt ihn; die MAC-Prüfung stimmt (Gegenprobe
  mit absichtlich falscher MAC: `LoginBlob.cpp:59: Mac doesn't match!`).
* Erster Schritt der Anmeldung, die HTTPS-Anfrage an
  `apresolve.spotify.com` aus dem PSRAM-Stack-Task heraus, in zwei Fehlerlagen:
  * **kein Weg nach außen** (Standard des Skripts): Namensauflösung scheitert
    (`TLSSocket.cpp:45: failed! connect returned -82`), bell wirft
    `mbedtls_ssl_handshake error`;
  * **Antwort, die kein gültiges JSON ist** (`QEMU_NET_OPEN=1` im Container):
    der TLS-Handshake mit Software-Krypto läuft durch, die Antwort ist
    unbrauchbar. Von wem sie im Container stammte, wurde nicht untersucht.

  In beiden Fällen: Exception, Abfangen im Wrapper, Wiederholung alle 5 s.
  Über zehn bzw. zwölf Wiederholungen blieb der freie Heap auf das Byte gleich
  (intern 279 255 bzw. 279 275, PSRAM 7 810 780 bzw. 7 810 800) – kein Leck in
  diesem Pfad.

### 7.2 Gefundener Absturz in cspot

Mit unverändertem cspot endete die zweite Fehlerlage (Antwort ist kein JSON)
in einem Absturz und Neustart:

```
I (1797) sc_zeroconf: login blob received
Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled.
EXCVADDR: 0x00000010
Backtrace:
  cspot::ApResolve::fetchFirstApAddress() at cspot/src/ApResolve.cpp:35
  cspot::Session::connectWithRandomAp() at cspot/src/Session.cpp:65
  Player::runTask() at components/spotify_connect/src/spotify_connect.cpp
```

Ursache: `ApResolve.cpp` greift ohne Prüfung auf `json["ap_list"][0]` zu. Ist
die Antwort kein erwartetes JSON (Anmeldeseite eines Gast-WLANs, Proxy,
HTTP-Fehlerseite, leerer Rumpf), ist das ein Nullzeiger-Zugriff. Derselbe Code
läuft bei jedem Wiederaufbau der Verbindung; das Gerät würde also neu starten,
solange die Lage anhält, und mit gespeicherten Zugangsdaten in einer
Neustartschleife hängen. Overlay 3 lässt die Funktion stattdessen eine
Exception werfen, die beide Aufrufer bereits behandeln. Danach im Emulator:
Fehlermeldung, Wiederholung, kein Absturz.

Fehlt die Verbindung ganz (erste Fehlerlage), stürzt auch unverändertes cspot
nicht ab; dann wirft schon bell. Ohne den gefälschten Login wäre der Absturz
erst auf Hardware aufgefallen, und nur hinter einem Netz, das Anfragen umleitet
oder mit Fehlerseiten beantwortet.

## 8. Abgleich des Wrappers mit squeezelites `Shim.cpp`

Übernommen ist die Struktur: ein eigener Task (32 kB, PSRAM) für Login und
`MercurySession::handlePacket()`, PCM-Callback mit der Regel „ein Titel spielt,
höchstens ein weiterer wird schon dekodiert", Meldung an Spotify erst, wenn der
neue Titel hörbar wird, und dieselbe Ereignisbehandlung.

| Punkt | Shim | Hier | Bewertung |
|---|---|---|---|
| Ereignisse `PLAYBACK_START`, `PLAY_PAUSE`, `TRACK_INFO`, `FLUSH`/`NEXT`/`PREV`, `SEEK`, `DEPLETED`, `VOLUME`, `DISC` | Kommandos an die squeezelite-Senke | gleiche Abfolge auf `audio_out` (leeren, pausieren) + Ereignis an die Anwendung | gleichwertig, ungetestet |
| „Titel ist hörbar" | fragt die Senke | Zählerstand `frames_written` beim Titelwechsel merken; erreicht, sobald `frames_played + frames_dropped` ihn überschreitet | gleichwertig; die rund 93 ms im I2S-DMA bleiben unberücksichtigt |
| Ende der Wiedergabeliste | ein Zustand, den `DEPLETED` überschreibt | zwei getrennte Merker; ein noch ausstehender Titelwechsel wird zuerst gemeldet | etwas robuster |
| Zugangsdaten speichern | FreeRTOS-Timer (Timer-Task hat internen Stack) | eigener Ereignis-Task mit internem Stack; schreibt nur bei geändertem Inhalt | gleicher Grund: ein Task mit PSRAM-Stack darf nicht auf Flash zugreifen |
| Gespeicherte Zugangsdaten | nur wenn Zeroconf abgeschaltet ist | immer nach erfolgreichem Login; beim Start bevorzugt, bei Ablehnung zurück zu Zeroconf | gewollt: Gerät soll nach dem Einschalten ohne Handy erreichbar sein |
| Zeroconf nach dem Login | bleibt aktiv | HTTP-Endpunkt und mDNS-Dienst werden beendet | gewollt (Einzelnutzer-Gerät); ein anderer Nutzer im WLAN kann das Gerät dann nicht übernehmen |
| Netzfehler beim Login | 5 Versuche um `connectWithRandomAp()`, `authenticate()` ungeschützt | beides in `try`, Wiederholung alle 5 s | robuster; im Emulator belegt |
| Exception in der Sitzungsschleife | nicht abgefangen → Abbruch | abgefangen: Sitzung abbauen, neu anmelden | robuster, **ungetestet** |
| Verbindungsabbruch im Betrieb | cspot baut selbst neu auf (`MercurySession::reconnect`) | unverändert cspot überlassen | ungetestet |
| HTTP-`addUser` | `strtok` auf dem Rumpf, keine Längenprüfung, ein `recv` | eigener Formular-Parser, Längen- und Feldprüfung, Empfang in Schleife | robuster; im Emulator belegt |
| Lokale Kommandos (Play/Pause/Next/Prev/Lautstärke) | direkt aus fremden Tasks in `spirc` | über eine Queue im Sitzungs-Task ausgeführt | vermeidet gleichzeitige Zugriffe, ungetestet |
| Lautstärke | Senke skaliert | Audio bleibt unskaliert; Wert wird nur gemeldet und als 100 % angekündigt | gewollt (Lautsprecher hat eigenen Regler); der Regler in der Spotify-App bewirkt hörbar nichts |
| Task-Kern | Kern 0 | Kern 1 (wie die cspot-eigenen Tasks) | offen, auf Hardware beobachten |
| Client-ID/Secret | zur Compilezeit | zur Laufzeit übergeben | gleichwertig |

Bekannte Eigenheiten, unverändert von der Referenz übernommen:

* Sprung (Seek), während der Decoder schon im nächsten Titel ist: danach wird
  kurz Position 0 gemeldet.
* Nach dem letzten Titel lädt cspot ihn pausiert neu. Die Anwendung sieht also
  nach `PLAYBACK_STOPPED` noch `PLAYBACK_STARTED` + `PLAYBACK_PAUSED`.

Schwächen in cspot, die gelesen, aber nicht ausgelöst und **nicht** gepatcht
wurden, stehen in `components/spotify_connect/patches/README.md`: zwei weitere
ungeprüfte JSON-Zugriffe (Token, CDN-Adresse), ein Token-Abruf ohne `try`
(Netzfehler genau in diesem Moment → Neustart) und fehlende Grenzprüfungen
beim Entschlüsseln des Login-Blobs.

## 9. Zur Laufzeit ungetesteter Code

Alles Folgende ist kompiliert und gelinkt, aber **nie gelaufen**:

**`components/audio_out`**
* tatsächliche I2S-Ausgabe, Takt, Pinbelegung
* Datenpfad `audio_out_write` → Ringpuffer → Feeder → I2S mit echten Daten
* Pause/Fortsetzen mit Nullen, `audio_out_flush`, Unterlauf- und Vorpuffer-Logik, Zähler
* `audio_out_stop`, `audio_out_set_mute`; Wirkung und Zeiten des XSMT-Ablaufs am DAC

**`main`**
* `wifi_sta.c` vollständig (der Emulator nutzt Ethernet), inklusive `esp_wifi_set_ps(WIFI_PS_NONE)` und Wiederverbinden
* USB-Serial-JTAG-Konsole

**`components/spotify_connect`**
* erfolgreicher Login (Zeroconf mit echter App, gespeicherte Zugangsdaten)
* `sc_credentials_save` und `sc_credentials_erase` (nur das Lesen lief)
* abgelehnter Login → `LOGIN_FAILED` → zurück zu Zeroconf
* `runSession`: Paketschleife, sämtliche Spirc-Ereignisse, `pcmWrite`, `pollPlayback`, Metadaten und Cover-URL, Positionsberechnung
* lokale Kommandos, `spotify_connect_get_status` im Betrieb
* Sitzungsabbruch mit Neuanmeldung, Gerätewechsel (`DISC`)
* mDNS-Sichtbarkeit in der Spotify-App
* 320 kbit/s: gesetzt wird `AudioFormat_OGG_VORBIS_320`; ob Spotify das Format liefert, ist offen (cspot fällt sonst still auf 96 kbit/s zurück)

**cspot/bell selbst**
* alles nach `ApResolve`: Schlüsseltausch mit dem Access Point, Authentifizierung, Mercury, Metadaten, Audio-Schlüssel, Token, CDN-Abruf, AES-Entschlüsselung, Vorbis-Dekodierung
* Overlay 2 (größerer Stack) und der Erfolgspfad von Overlay 3
* Hardware-Krypto (AES/SHA/MPI) – der Emulator lief mit Software-Krypto
* Octal-PSRAM – der Emulator lief mit Quad-PSRAM

## 10. Offene Risiken, die nur Hardware und ein Spotify-Konto klären

1. **Funktioniert der Fork heute gegen Spotify?** Login, Metadaten, Token per
   Client-ID/Secret, CDN, 320 kbit/s. Der Fork ist aktiv (letzter Commit
   06.10.2026) und steckt in squeezelite, dort aber auf ESP-IDF 4.x und dem
   ESP32 ohne S3.
2. **Interner RAM** mit WLAN + TLS + später LVGL. Statisch sind 211 kB frei;
   der Tiefstwert im Betrieb ist unbekannt.
3. **Tasks mit PSRAM-Stack und Flash-Zugriffe.** Greift ein solcher Task auf
   Flash zu (NVS, Dateisystem, OTA), bricht ESP-IDF mit einer Assertion ab. Der
   Wrapper hält die Regel ein; für cspot ist sie nur gelesen, nicht erlebt.
   Umgekehrt halten Flash-Schreibzugriffe anderer Tasks beide Kerne kurz an:
   NVS- oder OTA-Schreiben während der Wiedergabe kann Aussetzer geben.
4. **Hardware-Krypto und Octal-PSRAM bei 80 MHz** (siehe Abschnitt 9).
5. **Stack-Reserven** aller Tasks, besonders `mercury_dispatcher` beim
   Wiederverbinden und der HTTP-Task beim Login. Auf Hardware mit
   `uxTaskGetStackHighWaterMark` nachmessen.
6. **TLS ohne Zertifikatsprüfung.** bell prüft Serverzertifikate nur, wenn
   `bell::X509Bundle::init()` aufgerufen wird. Das geschieht hier nicht (wie
   bei squeezelite). bells Bündelformat ist das alte von ESP-IDF 4.x und passt
   nicht zum Bündel von ESP-IDF 5.5; das Einschalten ist also eine eigene Aufgabe.
7. **Neustarts durch cspot** bei Netzstörungen im falschen Moment (Abschnitt 8,
   letzter Absatz). Mit gespeicherten Zugangsdaten meldet sich das Gerät danach
   selbst wieder an, die Wiedergabe ist aber unterbrochen.
8. **Klang:** Der ESP32-S3 hat keinen APLL; der Takt entsteht per
   Bruchteiler aus 160 MHz und hat entsprechend Jitter. BCK ist 32 × fs
   (1,4112 MHz) ohne MCLK – laut Datenblatt für die PLL des PCM5102A zulässig,
   am Modul nicht geprüft. Knackser beim Stummschalten, Masseschleife über USB.
9. **Start per NFC über die Web-API** auf ein cspot-Gerät (altes
   Spirc-Protokoll): ob `PUT /me/player/play` das Gerät als Ziel annimmt, ist
   ungeprüft.
10. **Eigene Client-ID/Secret:** ob Spotify den Abruf der Audio-Adressen mit
    einem solchen Token dauerhaft zulässt, liegt nicht in unserer Hand. Das
    Secret steht derzeit im Klartext im Image.
11. **Windows-Build** nicht ausgeführt ([BUILD.md](BUILD.md)).
12. **Lizenz:** cspot ist GPL-3.0; das gilt damit für die ganze Firmware, auch
    beim Verschenken des Geräts (Quelltext anbieten).

## 11. Vorschlag für den ersten Hardwaretag

1. Ohne Spotify-Daten flashen: Bootmeldung, PSRAM-Größe, Heap-Zeile, WLAN.
   Mit dem Oszilloskop oder Logikanalysator BCK/LCK/DIN und XSMT ansehen.
2. Echter Login aus der Spotify-App, ein Titel, Pause, Sprung, nächster Titel,
   Ende der Liste. Dabei Heap-Tiefstwerte und `underruns` mitschreiben.
3. Neustart: Anmeldung mit gespeicherten Zugangsdaten ohne Handy.
4. WLAN-Router kurz ausschalten: baut cspot die Verbindung neu auf, oder
   startet das Gerät neu?
