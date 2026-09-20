#!/usr/bin/env bash
# Nyxis OS 빌드 스크립트
#
# [수정 이력 요약]
#  - 예전에는 부트로더/커널만 복사한 64MB FAT32 이미지를 만들고 initrd.img 를 빠뜨려서
#    (Makefile 의 os 타깃과 결과가 달랐다) 커널이 항상 "No initrd loaded" 로 부팅했다.
#    이제 Makefile 의 os 타깃을 그대로 사용하고 결과물을 out/nyxis-os 로도 복사한다.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../out"
NYXIS_IMG="$OUT_DIR/nyxis-os"

cd "$SCRIPT_DIR"

echo "Building nxkernel bootloader, kernel and ESP image..."
make -B os

cp -f "$OUT_DIR/esp.img" "$NYXIS_IMG"

echo "Build complete."
echo "Kernel: $OUT_DIR/kernel.elf"
echo "Bootloader: $OUT_DIR/BOOTX64.EFI"
echo "EFI image: $NYXIS_IMG"
