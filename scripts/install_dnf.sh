#!/bin/bash
set -e
echo "   Führe dnf Installation aus..."
sudo dnf check-update || true
sudo dnf install -y \
    gcc gcc-c++ make \
    cmake \
    git \
    pkgconf \
    openssl-devel \
    bluez-libs-devel \
    cppcheck \
    python3 \
    pigpio
