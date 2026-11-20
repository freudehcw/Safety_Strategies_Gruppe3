# Safety Demonstrator: 
#### Safety Systems ILV
#### Gruppe 3
Demonstrator für ein geschwindigkeitsabhängiges Schutzsystem eines rotierenden Aktors (orientiert an Speed and Separation Monitoring). 

## Requirements

### Windows
*   **Nicht vollständig unterstützt:** Das System benötigt `BlueZ` (Linux-Bluetooth-Stack) und `pigpio` (Raspberry Pi GPIOs), welche unter Windows nicht nativ verfügbar sind.
*   *Workaround:* Entwicklung und Unit-Testing der Kern-Logik (UI, Safety-Algorithmen, Crypto) ist möglich, wenn Hardware-Abhängigkeiten in CMake auskommentiert oder gemockt werden. Benötigt wird: **CMake, GCC/Clang (z.B. via MSYS2), OpenSSL**.
*   *Empfehlung:* Nutze **WSL2** (Windows Subsystem for Linux), um das Linux-Setup auszuführen (siehe unten).

### macOS
*   **Eingeschränkt unterstützt:** Ähnlich wie bei Windows sind `BlueZ` und `pigpio` unter macOS nicht verfügbar. 
*   Die Entwicklung von Kern-Logik und Unit-Tests wird jedoch nativ unterstützt.
*   Voraussetzung: **Homebrew** (`brew`) muss auf dem System installiert sein, um Compiler, CMake und OpenSSL zu beziehen.

### Linux
Die benötigten Bibliotheken variieren leicht je nach Distribution. Folgende Basis-Tools werden erwartet:
*   Compiler: `gcc`, `gcc-c++`, `make` (oder `build-essential`)
*   Build-System: `cmake`, `pkg-config`
*   Abhängigkeiten: `libssl-dev` (OpenSSL), `libbluetooth-dev` (BlueZ), `pigpio`
*   Qualitätssicherung: `cppcheck`

## How to build

### Windows (ohne WSL)
Da keine offiziellen Build-Skripte für Windows existieren, muss der Build manuell über CMake erfolgen:
```Powershell
mkdir build
cd build
cmake .. -DPI1_APP_NAME="pi1_app" -DPI2_APP_NAME="pi2_app"
cmake --build .
```
*(Achtung: Dies kompiliert nur erfolgreich, wenn Linux-spezifische Code-Teile auskommentiert wurden).*

### macOS
Unser Bootstrap-Skript erkennt macOS automatisch und installiert die nötigen Basis-Tools via Homebrew.

```Bash
./bootstrap.sh
cd build
make -j4
```

### Linux (und WSL2)
Wir verwenden ein Bootstrap-Skript, das den Paketmanager (`apt`, `dnf`, `zypper`, `brew`) automatisch erkennt, alle Abhängigkeiten installiert und das CMake-Projekt vorbereitet.

Führe folgende Befehle in deinem Terminal aus:

```Bash
./bootstrap.sh
# Alternativ mit eigenen App-Namen:
# PI1_APP_NAME=host_ui PI2_APP_NAME=motor_control ./bootstrap.sh
cd build
make -j4
```
Die fertigen ausführbaren Dateien (Default: `pi1_app` und `pi2_app`) befinden sich danach direkt im `build/` Ordner.

## Raspberry Pi 2 (Safety/Steuerung)

Der Quellcode liegt in `src/pi2_safety/`, gemeinsame Teile (Nachrichtenformat, Konstanten) in `src/common/`.

### Hardware-Abstraktion (HAL)
CMake wählt die HAL automatisch: Ist `pigpio` installiert (Raspberry Pi, Paket `libpigpio-dev`), wird die reale Hardware verwendet, sonst eine **Simulation** (CMake gibt dann eine Warnung aus). Erzwingen mit `cmake -DPI2_HAL=pigpio ..` bzw. `-DPI2_HAL=sim`.

### Starten
```Bash
sudo ./pi2_app                          # Raspberry Pi: Bluetooth-RFCOMM, Kanal 1, Schlüssel /etc/ssm/psk.bin
./pi2_app --tcp 5555 --key psk.bin      # Entwicklung: TCP auf localhost, Schlüssel aus eigener Datei
```
Ohne gültigen Schlüssel (32 Byte, Rechte höchstens 0600) startet `pi2_app` nicht. Einen Testschlüssel erzeugt `tools/gen_psk.sh psk.bin`.
Auf dem Raspberry Pi startet das Programm nur mit Root-Rechten, verfügbarem Watchdog und `SCHED_FIFO`. Ohne Echo bleibt das System in FEHLER / SENSOR; im Messbereich (bis 4 m) muss deshalb eine Rückwand stehen.

### Testmodus
Die Fehlereinspeisung ist ein eigenes Programm: `pi2_app_test` wird mit `SSM_TESTMODE` gebaut und liegt neben `pi2_app` im Build-Verzeichnis. Im normalen `pi2_app` ist dieser Code nicht enthalten. `pi2_app_test` nimmt Befehle auf der Standardeingabe an und schreibt das Test-Log `pi2_testlog.csv` (Zustandswechsel und Empfangszeit der letzten gültigen Nachricht mit der monotonen Uhr, Zyklusüberschreitungen).

| Befehl | Wirkung |
|---|---|
| `force <1\|2> <mm>` / `unforce <1\|2>` | Rohmesswert eines Sensors ersetzen bzw. Vorgabe aufheben (TC-01a, TC-01b, TC-02) |
| `block <1\|2> <ms>` | Sensor-Thread einmalig anhalten (TC-04) |
| `run <1\|2\|3> <s>` | Stufe LOW/MEDIUM/HIGH für s Sekunden ohne Pi 1 freigeben, danach Motor AUS (Nachlaufmessung) |
| `d <1\|2> <mm>` | simulierten Abstand setzen (nur Simulation) |
| `stop` | lokaler Stop/Shutdown |

**Nachlaufmessung (SAF-10):** `sudo ./pi2_app_test`, dann z. B. `run 3 5`. Der Motor läuft 5 Sekunden mit HIGH, sofern beide Sensoren gültig sind und der Sicherheitsabstand eingehalten ist. Der Zeitpunkt von Motor AUS steht im Test-Log; die Zeit bis zum Stillstand des Rotors wird am Aufbau gemessen und bestimmt die Sicherheitsabstände in `src/pi2_safety/pi2_config.h`.

### Hardware-Watchdog (Ergänzung zum Designdokument)
Die Hardware-PWM läuft unabhängig vom Prozess weiter. Damit der Motor bei einem Absturz oder hängenden Programm nicht weiterläuft, verwendet `pi2_app` auf dem Raspberry Pi den Hardware-Watchdog `/dev/watchdog` mit 1 Sekunde Timeout. Die Safety-Logik bedient ihn nur, solange die Motorsteuerung ihre Freigaben umsetzt. Bleibt das aus, wird der Pi zurückgesetzt; danach sind die GPIOs Eingänge und STBY liegt über den Pull-down auf LOW.

*   Voraussetzung: `/dev/watchdog` ist vorhanden (sonst `dtparam=watchdog=on` in `/boot/firmware/config.txt`), und systemd verwendet ihn nicht selbst (`RuntimeWatchdogSec`).
*   Ist der Watchdog nicht verfügbar, startet das Programm nicht.
*   `--no-watchdog` schaltet ihn für die Entwicklung ab. In der Simulation ist er nie aktiv.

### Ohne Pi 1 testen
Gegen den echten Pi 1 auf demselben Rechner:
```Bash
../tools/gen_psk.sh psk.bin                                              # einmalig
./pi2_app_test --tcp 5555 --key psk.bin                                  # Terminal 1
./pi1_app --tcp --tcp-port 5555 --key psk.bin --lcd-console              # Terminal 2: 1/2/3, q
```
Für die Angriffstests (TC-08, TC-09) erzeugt `tools/pi1_sim.py` gezielt fehlerhafte Nachrichten. Er verwendet AES-256-GCM über das Python-Paket `cryptography` (`pip install cryptography` bzw. `python3-cryptography`):
```Bash
../tools/pi1_sim.py --tcp 5555 --key psk.bin   # low | medium | high | pause | resume | corrupt | tamper | badsession | replay | badlevel | stop
```

### Unit-Tests
```Bash
cd build
make -j4 && ctest --output-on-failure
```

### Testfälle
Zuordnung der Testfälle aus dem Designdokument zu den Tests. Alle Tests sind in der Simulation (TCP auf einem Linux-PC) bestanden; am Raspberry Pi stehen sie noch aus.

| Testfall | Unit-Test (`tests/`) | Durchlauf mit den Programmen |
|---|---|---|
| TC-01 Sensor ungültig | `test_pi2`: `test_p1_range`, `test_invalid_sensor` | `pi2_app_test`: `d <1\|2> 0` (kein Echo) oder Sensor verdecken |
| TC-01a Messbereich (P1) | `test_pi2`: `test_p1_range` | `pi2_app_test`: `force <1\|2> 5000` |
| TC-01b Sprung (P2) | `test_pi2`: `test_p2_rate` | `pi2_app_test`: `force <1\|2> <mm>` mit großem Sprung |
| TC-02 Sensorabweichung (P3) | `test_pi2`: `test_cross_check`, `test_cross_check_single_update` | `pi2_app_test`: `force 2 1200` bei 1000 mm |
| TC-03 Verbindungsverlust | `test_pi2`: `test_comm_timeout` | `pi1_app` mit `kill -9` beenden oder `pi1_sim.py`: `pause`; Reaktionszeit im Test-Log von Pi 2 |
| TC-04 Datenalter (P4) | `test_pi2`: `test_stale_data` | `pi2_app_test`: `block 1 300` |
| TC-05 Objekt im Sicherheitsabstand | `test_pi2`: `test_object_stop` | `pi2_app_test`: `d 1 400` und `d 2 400` |
| TC-06 Programmende | `test_pi2`: `test_shutdown` | `pi1_app`: `q`, SIGINT oder SIGTERM; STOP, Motor AUS, ACK |
| TC-07 Wiederanlauf | `test_pi2`: `test_comm_timeout`, `test_object_stop` | Objekt entfernen; `pi1_app` neu starten |
| TC-08 Spoofing | `test_pi2`: `test_protocol`; `test_common` | gegen Pi 2: `pi1_sim.py` `corrupt`, `tamper`; gegen Pi 1: `mock_pi2` `badtag`, `badpayload` |
| TC-09 Replay | `test_common` (Session) | gegen Pi 2: `pi1_sim.py` `replay`, `badsession`; gegen Pi 1: `mock_pi2` `replay`, `oldsession` |

### Dokumentation
Die Software-Dokumentation liegt in `documents/software/` (Einstieg: `documents/software/README.md`). Für Pi 2 beginnt sie mit `documents/software/pi2/ueberblick.md`. Offene Punkte und der Abgleich mit dem Auftrag stehen gesammelt in `documents/software/pi2/stand.md`, die MISRA-Konformität in `documents/software/pi2/misra_konformitaet.md`.

## Einsatz von Sprachmodellen

| Teil | Werkzeug und Modell | Erstellt | Prüfung |
|---|---|---|---|
| `src/pi2_safety/`, `tests/test_pi2.c`, `tools/pi1_sim.py`, Abschnitt Pi 2 in `CMakeLists.txt` | Claude Code mit Claude Fable 5.2 und Claude Opus 5.5 | Quellcode, Kommentare und Unit-Tests | Build ohne Warnungen, 216 Prüfungen in den Unit-Tests, Durchläufe gegen `pi1_app` und `tools/pi1_sim.py` (siehe Testfälle), cppcheck mit MISRA-Addon. Durchsicht durch ein Gruppenmitglied: offen (Platzhalter `<Name>` in jeder Datei). |
| `documents/software/README.md`, `documents/software/pi2/*.md`, Abschnitt Pi 2 in diesem README | Claude Code mit Claude Fable 5.2 und Claude Opus 5.5 | Text | Durchsicht durch ein Gruppenmitglied: offen. |

Auf dem Raspberry Pi wurde der Code noch nicht gebaut oder getestet.
