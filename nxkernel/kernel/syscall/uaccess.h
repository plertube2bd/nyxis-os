/*
 * uaccess.h - 사용자 메모리 안전 접근 (시스템 콜에서 사용자 포인터를 다루는 유일한 통로)
 *
 * 규칙: 사용자 포인터(u64 값)를 C 포인터로 직접 역참조하지 않는다. 항상 아래 함수를 쓴다.
 * 각 함수는 (1) 오버플로/커널 영역 침범 검사 (2) 페이지 테이블의 USER(및 쓰기 시 RW) 비트 검사
 * (3) 폴트 복구 가능한 복사, 세 단계를 모두 수행한다.
 */
#ifndef KERNEL_UACCESS_H
#define KERNEL_UACCESS_H

#include "nyxis.h"
#include "interrupt.h"

/* 사용자 -> 커널 복사. 실패 시 NinvalidPointer. len == 0 이면 포인터를 검사하지 않고 성공. */
Nstatus copy_from_user(void *dst, u64 user_src, usize len);

/* 커널 -> 사용자 복사 (대상 페이지가 쓰기 가능해야 함). 실패 시 NinvalidPointer. */
Nstatus copy_to_user(u64 user_dst, const void *src, usize len);

/*
 * 사용자 문자열을 최대 max 바이트(NUL 포함)까지 복사한다. 성공하면 dst 는 항상 NUL 로 끝난다.
 * NUL 을 max 바이트 안에서 못 찾으면 NpathTooLong, 접근 불가면 NinvalidPointer.
 */
Nstatus copy_string_from_user(char *dst, u64 user_src, usize max);

/*
 * #PF 핸들러(interrupt.c)가 호출한다. 커널 모드에서 사용자 영역 주소로의 nx_uaccess_copy 접근이
 * 실패한 경우라면 프레임의 rip 를 복구 지점으로 바꾸고 true 를 반환한다. 그 외에는 false (=진짜 커널 버그).
 */
bool uaccess_fixup(struct trap_frame *frame);

#endif /* KERNEL_UACCESS_H */
