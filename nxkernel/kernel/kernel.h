/*
 * kernel.h - 커널 메인 / 부트 정보 접근
 */
#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"

/*
 * boot.s 가 higher-half 로 진입한 뒤 호출한다. 절대 반환하지 않는다.
 *   boot_kind      : BOOT_KIND_UEFI / BOOT_KIND_MULTIBOOT2 (kernel/boot/multiboot2.h)
 *   boot_info_phys : 부트 정보의 "물리 주소" (UEFI: 낮은 주소로 복사된 NTBLI, Multiboot2: MBI)
 */
void kernel_main(u32 boot_kind, u64 boot_info_phys) __attribute__((noreturn));

/*
 * 커널이 소유한 부트 정보 사본을 반환한다. (부트로더 메모리의 원본이 아님)
 * 검증에 실패했다면 NULL.
 */
const NTBLI *get_kernel_info(void);

/*
 * ring3 로 진입한다 (iretq). 사용자 주소 공간/유저 스택이 준비된 이후에만 사용할 수 있으며
 * 현재는 유저 프로세스 로더가 없어 사용되지 않는다.
 */
void enter_ring3(void *entry, void *stack_top) __attribute__((noreturn));

#endif /* KERNEL_H */
