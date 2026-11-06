#!/bin/bash
set -e
echo "   Führe apt-get Installation aus..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    libbluetooth-dev \
    cppcheck \
    python3 \
    pigpio python3-pigpio
