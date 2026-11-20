#!/bin/bash
# gen_psk.sh - Erzeugt den 256-Bit-Pre-Shared-Key (32 Byte Rohdaten) aus
# /dev/urandom mit Rechten 0600 (SEC-01). Der Schluessel wird nicht ausgegeben.
#
# Aufruf: tools/gen_psk.sh [ZIELDATEI]   (Standard: /etc/ssm/psk.bin)
# Danach dieselbe Datei auf den anderen Pi kopieren (z. B. scp), nie ins Git.
#
# Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
# geprueft von: <Name>
set -eu

TARGET="${1:-/etc/ssm/psk.bin}"

if [ -e "$TARGET" ]; then
    echo "Fehler: $TARGET existiert bereits und wird nicht ueberschrieben." >&2
    exit 1
fi

mkdir -p "$(dirname "$TARGET")"
# umask 077: Die Datei entsteht direkt mit Rechten 0600, ohne Zwischenzustand.
(umask 077 && head -c 32 /dev/urandom > "$TARGET")
chmod 600 "$TARGET"

SIZE=$(stat -c %s "$TARGET")
if [ "$SIZE" -ne 32 ]; then
    rm -f "$TARGET"
    echo "Fehler: Schluessel hat $SIZE statt 32 Byte." >&2
    exit 1
fi
echo "Schluessel erzeugt: $TARGET (32 Byte, Rechte 0600, Eigentuemer $(stat -c %U "$TARGET"))."
