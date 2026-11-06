#!/bin/bash
set -e

# Verzeichnis des Hauptskripts ermitteln
# So kann es auch woanders aufgerufen werden
BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPTS_DIR="$BASE_DIR/scripts"

# Konfigurierbarer executable Name mit default piX_app
# Kann aus ENV mit PIX_APP_NAME kommen
PI1_NAME="${PI1_APP_NAME:-pi1_app}"
PI2_NAME="${PI2_APP_NAME:-pi2_app}"

echo "=== Safety Demonstrator (Gruppe 3) Bootstrap ==="
echo "Konfigurierte Zielnamen:"
echo "  Pi 1 (Host)   -> $PI1_NAME"
echo "  Pi 2 (Safety) -> $PI2_NAME"
echo "================================================"
echo ""
echo "[1/3] Erkenne System und installiere Abhängigkeiten..."

# OS Erkennung und Aufruf des passenden Sub-Skripts
if [ -x "$(command -v apt-get)" ]; then
    echo "=> Debian/Ubuntu basiertes System erkannt."
    "$SCRIPTS_DIR/install_apt.sh"

elif [ -x "$(command -v rpm-ostree)" ] && [ -x "$(command -v brew)" ]; then
    echo "=> Immutable OS (rpm-ostree) erkannt. Weiche auf Homebrew aus."
    "$SCRIPTS_DIR/install_brew.sh"

elif [ -x "$(command -v dnf)" ]; then
    echo "=> RHEL/Fedora/CentOS basiertes System erkannt."
    "$SCRIPTS_DIR/install_dnf.sh"

elif [ -x "$(command -v zypper)" ]; then
    echo "=> openSUSE basiertes System erkannt."
    "$SCRIPTS_DIR/install_zypper.sh"

elif [ "$(uname -s)" = "Darwin" ]; then
    echo "=> macOS erkannt."
    if [ -x "$(command -v brew)" ]; then
        "$SCRIPTS_DIR/install_brew.sh"
    else
        echo "Fehler: Homebrew ist auf diesem Mac nicht installiert. Bitte zuerst installieren."
        exit 1
    fi

    echo ""
    echo "=================================================================="
    echo " WARNUNG FUER MACOS / NON-LINUX SYSTEME:"
    echo " 'BlueZ' (Bluetooth) und 'pigpio' sind Linux/Raspberry-spezifisch!"
    echo " Die Entwicklung der Kern-Logik und Unit-Tests ist machbar, aber der"
    echo " komplette Hardware-Build wird auf macOS fehlschlagen."
    echo "=================================================================="
    echo ""

elif [ -x "$(command -v brew)" ]; then
    echo "=> Generisches Linuxbrew erkannt."
    "$SCRIPTS_DIR/install_brew.sh"

else
    echo "Warnung: Kein bekannter Paketmanager (apt, dnf, zypper, brew) gefunden."
    echo "Bitte stelle sicher, dass Compiler, CMake, OpenSSL-Header, BlueZ und pigpio manuell installiert sind."
fi

echo ""
echo "[2/3] Bereite Build-Verzeichnis vor..."
mkdir -p "$BASE_DIR/build"
cd "$BASE_DIR/build"

echo ""
echo "[3/3] Generiere Makefiles mit CMake..."
# Override mit cmake -D möglich, auch nach erstem cmake Run
cmake -DPI1_APP_NAME="$PI1_NAME" -DPI2_APP_NAME="$PI2_NAME" ..

echo ""
echo "=== Bootstrap erfolgreich abgeschlossen! ==="
echo ""
echo "Kompilieren:"
echo "  cd build"
echo "  make -j4"
