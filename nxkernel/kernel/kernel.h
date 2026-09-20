/*
 * kernel.h - 커널 메인 / 부트 정보 접근
 */
#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"

/* 어셈블리 진입점(_start)에서 호출된다. 절대 반환하지 않는다. */
void kernel_main(NTBLI *boot_info) __attribute__((noreturn));

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
