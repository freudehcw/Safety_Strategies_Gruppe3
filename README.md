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
