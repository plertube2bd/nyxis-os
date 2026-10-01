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
 * ring3 로 진입한다 (iretq). 호출 시점에 "현재 프로세스의" 주소 공간(CR3)에 entry/stack_top
 * 이 이미 유효하게 매핑되어 있어야 한다. process_create_user() 가 만든 커널 스레드가 이
 * 함수를 통해서만 ring3 로 내려간다 (kernel/process/process.c 의 user_process_entry_thunk).
 */
void enter_ring3(void *entry, void *stack_top) __attribute__((noreturn));

#endif /* KERNEL_H */
