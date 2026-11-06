#!/bin/bash
set -e
echo "   Führe Homebrew Installation aus..."
brew update
brew install \
    cmake \
    git \
    pkg-config \
    openssl \
    cppcheck \
    python3
