#!/usr/bin/env bash
# Dump the GD25Q32(B) SPI flash from the AEH-W41H1 with a CH341A + SOIC-8 clip.
# Reads twice and compares so you know the dump is reliable (CH341A reads can be flaky).
set -euo pipefail

CHIP="GD25Q32(B)"
OUT="${1:-w41h1_dump}"

# CH341A must be in PROGRAMMER mode (USB 1a86:5512). If it enumerates as 1a86:5523
# it's in TTL/UART mode -> flip the board's mode switch.
echo "# probing..."
flashrom -p ch341a_spi   # should report: Found GigaDevice flash chip "GD25Q32(B)"

echo "# read #1"; flashrom -p ch341a_spi -c "$CHIP" -r "${OUT}1.bin"
echo "# read #2"; flashrom -p ch341a_spi -c "$CHIP" -r "${OUT}2.bin"

s1=$(sha256sum "${OUT}1.bin" | cut -d' ' -f1)
s2=$(sha256sum "${OUT}2.bin" | cut -d' ' -f1)
if [ "$s1" = "$s2" ]; then
  echo "MATCH: reliable dump -> ${OUT}1.bin ($s1)"
else
  echo "MISMATCH: reseat the clip, reads not stable"; exit 1
fi
