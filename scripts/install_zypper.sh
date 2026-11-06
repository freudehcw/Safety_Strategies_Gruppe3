#!/bin/bash
set -e
echo "   Führe zypper Installation aus..."
sudo zypper --non-interactive refresh
sudo zypper --non-interactive install \
    gcc gcc-c++ make \
    cmake \
    git \
    pkgconf \
    libopenssl-devel \
    bluez-devel \
    cppcheck \
    python3
echo "   Hinweis: 'pigpio' ist in openSUSE Standard-Repos oft nicht enthalten und muss bei Bedarf manuell kompiliert werden."
