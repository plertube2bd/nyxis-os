/*
 * setjmp.h - C89 7.6 비지역 점프
 *
 * jmp_buf 는 System V AMD64 의 callee-saved 범용 레지스터(rbx rbp r12~r15)와 rsp, 복귀 주소를 담는다.
 * x87 제어 워드/MXCSR 은 저장하지 않는다: nlibc 프로그램은 아직 부동소수점을 쓰지 않는다 (stdlib.h 머리말).
 *
 * C89 규칙 그대로: setjmp 를 부른 함수가 끝난 뒤에 longjmp 하면 정의되지 않은 동작이고,
 * setjmp 이후 바뀐 비-volatile 지역 변수의 값은 longjmp 뒤에 불확정이다.
 */
#ifndef _NL_SETJMP_H
#define _NL_SETJMP_H

typedef long jmp_buf[8];

#ifdef __GNUC__
int setjmp(jmp_buf env) __attribute__((__returns_twice__));
void longjmp(jmp_buf env, int val) __attribute__((__noreturn__));
#else
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
#endif

/* C89 은 setjmp 가 매크로인지 함수인지 정하지 않는다: 여기서는 함수 + 같은 이름 매크로 */
#define setjmp(env) setjmp(env)

#endif /* _NL_SETJMP_H */
