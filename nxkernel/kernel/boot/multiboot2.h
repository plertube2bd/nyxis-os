/*
 * multiboot2.h - Multiboot2(GRUB 등) 부트 정보를 NTBLI 로 변환하는 어댑터
 *
 * 커널은 부트 방식(UEFI/GRUB/...)에 종속되지 않도록 항상 NTBLI 만 사용한다.
 * 새로운 부트 방식을 지원하려면 "그 방식의 정보 -> NTBLI" 변환 함수만 추가하면 된다.
 */
#ifndef KERNEL_BOOT_MULTIBOOT2_H
#define KERNEL_BOOT_MULTIBOOT2_H

#include "nyxis.h"

/* 진입 코드(boot.s)가 kernel_main 에 넘기는 부트 방식 */
#define BOOT_KIND_UEFI        1U   /* NYTB 가 만든 NTBLI 를 그대로 받음 */
#define BOOT_KIND_MULTIBOOT2  2U   /* Multiboot2 정보 구조체(MBI) 를 받음 */

/* Multiboot2 가 넘기는 정보 구조체는 4GiB 미만의 물리 주소에 있어야 한다 (초기 항등 매핑 범위) */
#define MB2_MAX_INFO_SIZE     (1UL << 20)

/*
 * MBI(mbi_phys, 4GiB 미만 물리 주소)를 검증하며 파싱하여 out 을 채운다.
 * 메모리 맵은 memmap_buf(memmap_cap 바이트)에 EFI 메모리 디스크립터 형식으로 변환해 저장한다.
 * 잘못된/손상된 MBI 이면 오류를 반환한다 (범위 밖 접근 없음).
 * 반환되는 out 의 주소 필드(initrd/framebuffer/Rsdp)는 모두 "물리 주소" 이다.
 */
Nstatus multiboot2_to_ntbli(u64 mbi_phys, NTBLI *out, void *memmap_buf, usize memmap_cap);

#endif /* KERNEL_BOOT_MULTIBOOT2_H */
