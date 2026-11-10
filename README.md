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
sudo ./pi2_app              # Raspberry Pi: Bluetooth-RFCOMM, Kanal 1 (root für pigpio, SCHED_FIFO, Watchdog)
./pi2_app --tcp 5555 --test # Entwicklung: TCP auf localhost statt Bluetooth, mit Testkonsole
```
Im Testmodus (`--test`) können über die Konsole Fehler eingespeist werden (`force`, `block`, in der Simulation zusätzlich `d` für den Abstand); eine leere bzw. unbekannte Eingabe zeigt die Hilfe.

### Hardware-Watchdog (Ergänzung zum Designdokument)
Die Hardware-PWM läuft unabhängig vom Prozess weiter. Damit der Motor bei einem Absturz oder hängenden Programm nicht weiterläuft, verwendet `pi2_app` auf dem Raspberry Pi den Hardware-Watchdog `/dev/watchdog` mit 1 Sekunde Timeout. Die Safety-Logik bedient ihn nur, solange die Motorsteuerung ihre Freigaben umsetzt. Bleibt das aus, wird der Pi zurückgesetzt; danach sind die GPIOs Eingänge und STBY liegt über den Pull-down auf LOW.

*   Voraussetzung: `/dev/watchdog` ist vorhanden (sonst `dtparam=watchdog=on` in `/boot/firmware/config.txt`), und systemd verwendet ihn nicht selbst (`RuntimeWatchdogSec`).
*   Ist der Watchdog nicht verfügbar, startet das Programm nicht.
*   `--no-watchdog` schaltet ihn für die Entwicklung ab. In der Simulation ist er nie aktiv.

### Ohne Pi 1 testen
`tools/pi1_sim.py` simuliert Pi 1 (Heartbeat, Stufenwahl, Stop, fehlerhafte Nachrichten):
```Bash
./pi2_app --tcp 5555 --test          # Terminal 1
../tools/pi1_sim.py --tcp 5555       # Terminal 2: low | medium | high | pause | resume | stop ...
```

### Unit-Tests
```Bash
cd build
make -j4 && ctest --output-on-failure
```

### Dokumentation
Erklärungen zu den Modulen von Pi 2 (Aufgabe, Ablauf, Entscheidungen, typische Fehlerquellen) liegen in `documents/modules/`, beginnend mit `documents/modules/erklaerung_pi2_ueberblick.md`. Dort steht auch der Abgleich mit dem Auftrag `pi2_agent_prompt.md`. Die MISRA-Konformität ist in `documents/misra_konformitaet_pi2.md` beschrieben.

### Noch offen
*   **Security:** `src/common/crypto_utils.c` ist ein Platzhalter (CRC statt AES-256-GCM, keine Verschlüsselung). Das Frame-Format ist bereits final.
*   **Sicherheitsabstände und PWM-Stufen** (`src/common/config.h`, `src/pi2_safety/pi2_config.h`) sind Startwerte und müssen am realen Aufbau festgelegt werden (SAF-10).

## Einsatz von Sprachmodellen

| Teil | Werkzeug und Modell | Erstellt | Prüfung |
|---|---|---|---|
| `src/common/`, `src/pi2_safety/`, `tests/test_pi2.c`, `tools/pi1_sim.py`, Abschnitt Pi 2 in `CMakeLists.txt` | Claude Code mit Claude Fable 5.2 | Quellcode, Kommentare und Unit-Tests | Build ohne Warnungen, 195 Prüfungen in den Unit-Tests, Simulationslauf gegen `tools/pi1_sim.py`, cppcheck mit MISRA-Addon. Durchsicht durch ein Gruppenmitglied: offen (Platzhalter `<Name>` in jeder Datei). |
| `documents/modules/erklaerung_pi2_*.md`, `documents/misra_konformitaet_pi2.md`, Abschnitt Pi 2 in diesem README | Claude Code mit Claude Fable 5.2 | Text | Durchsicht durch ein Gruppenmitglied: offen. |

Auf dem Raspberry Pi wurde der Code noch nicht gebaut oder getestet.
