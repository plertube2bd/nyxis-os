#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../out"
NYXIS_IMG="$OUT_DIR/nyxis-os"

cd "$SCRIPT_DIR"

echo "Building nxkernel bootloader and kernel..."
make -B all

echo "Creating FAT32 EFI system image: $NYXIS_IMG"
rm -f "$NYXIS_IMG"
dd if=/dev/zero of="$NYXIS_IMG" bs=1M count=64 status=none
mkfs.vfat -F 32 -n EFI "$NYXIS_IMG"

mmd -i "$NYXIS_IMG" EFI
mmd -i "$NYXIS_IMG" EFI/BOOT
mcopy -i "$NYXIS_IMG" "$OUT_DIR/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$NYXIS_IMG" "$OUT_DIR/kernel.elf" ::kernel.elf

echo "Build complete."
echo "Kernel: $OUT_DIR/kernel.elf"
echo "Bootloader: $OUT_DIR/BOOTX64.EFI"
echo "EFI image: $NYXIS_IMG"