/*
 * selftest_user.s - 자체 점검용 사용자 모드(ring 3) 프로그램 (selftest.c 가 사용자 페이지로 복사해 실행)
 *
 * 사용자 주소 배치 (selftest.c 와 일치):
 *   0x400000 : 코드 (R-X)   0x401000 : 결과 저장용 데이터 (RW-)   0x402000~0x403000 : 스택
 * 위치 독립 코드이며 문자열은 rip 상대 주소로 참조한다.
 *
 * 커널이 쓰기(copy_to_user) 로 채우는 모든 버퍼(struct 결과, 스칼라 결과 배열)는 반드시
 * "데이터 페이지"(0x401000, RW) 안에 있어야 한다. 코드 페이지는 R-X 라서 그곳에 쓰려고 하면
 * paging_is_user_range(write=true) 가 거부한다. 경로 문자열 등 "읽기만" 하는 상수는 .rodata(코드 페이지)에 둬도 된다.
 *
 * 데이터 페이지 레이아웃 (rbx = 0x401000):
 *   [0x000, 0x100) : 스칼라 결과 8바이트 x 32개 (rbx + 8*i)
 *   [0x100, 0x120) : struct nx_stat (NxStat 결과)
 *   [0x140, 0x170) : struct nx_sysinfo (NxSysInfo 결과)
 *   [0x190, 0x1B0) : struct nx_procinfo (NxProcessInfo 결과)
 *   [0x200, ...)   : NxRead 로 읽은 파일 내용
 */
    .set NX_SYS_GET_VERSION, 2
    .set NX_SYS_GET_TIME, 3
    .set NX_SYS_SLEEP, 4
    .set NX_SYS_YIELD, 5
    .set NX_SYS_SYSINFO, 7
    .set NX_SYS_OPEN, 32
    .set NX_SYS_CLOSE, 33
    .set NX_SYS_READ, 34
    .set NX_SYS_WRITE, 35
    .set NX_SYS_SEEK, 36
    .set NX_SYS_STAT, 37
    .set NX_SYS_DUPLICATE_HANDLE, 41
    .set NX_SYS_PROCESS_EXIT, 65
    .set NX_SYS_PROCESS_INFO, 68
    .set NX_SYS_KERNEL_PRINT, 771
    .set NX_SYS_DEBUG_NOP, 777

    .set NX_O_READ, 1
    .set NX_SEEK_SET, 0
    .set NX_CLOCK_MONOTONIC, 0
    .set NX_RIGHT_READ, 0x01
    .set NX_RIGHT_ALL, 0x1F
    .set NX_HANDLE_STDOUT, 1
    .set NX_PID_SELF, 0xFFFFFFFF

    .set RES_BASE, 0x401000
    .set STAT_BUF, 0x401100
    .set SYSINFO_BUF, 0x401140
    .set PROCINFO_BUF, 0x401190
    .set READ_BUF, 0x401200

    .section .rodata

    .global user_prog1_start
    .global user_prog1_end
user_prog1_start:
    movabsq $RES_BASE, %rbx         /* 결과 배열 */

    movl $NX_SYS_DEBUG_NOP, %eax     /* [0x00] syscall 명령 -> 0 */
    syscall
    movq %rax, 0x00(%rbx)

    movl $NX_SYS_DEBUG_NOP, %eax     /* [0x08] int 0x80 -> 0 */
    int $0x80
    movq %rax, 0x08(%rbx)

    movl $NX_SYS_KERNEL_PRINT, %eax  /* [0x10] 유효한 사용자 문자열 -> 0 */
    leaq umsg(%rip), %rdi
    syscall
    movq %rax, 0x10(%rbx)

    movl $NX_SYS_KERNEL_PRINT, %eax  /* [0x18] NULL 포인터 -> NinvalidPointer(음수) */
    xorl %edi, %edi
    syscall
    movq %rax, 0x18(%rbx)

    movl $NX_SYS_KERNEL_PRINT, %eax  /* [0x20] 커널 주소 -> 거부되어야 함 */
    movabsq $0xFFFFFFFF80100000, %rdi
    int $0x80
    movq %rax, 0x20(%rbx)

    movl $NX_SYS_YIELD, %eax         /* [0x28] */
    syscall
    movq %rax, 0x28(%rbx)

    movl $NX_SYS_GET_VERSION, %eax   /* [0x30] */
    int $0x80
    movq %rax, 0x30(%rbx)

    movl $NX_SYS_GET_TIME, %eax      /* [0x38] t0 */
    movl $NX_CLOCK_MONOTONIC, %edi
    syscall
    movq %rax, 0x38(%rbx)

    movl $NX_SYS_SLEEP, %eax         /* [0x40] 20ms 재우기 -> 0 */
    movl $20, %edi
    syscall
    movq %rax, 0x40(%rbx)

    movl $NX_SYS_GET_TIME, %eax      /* [0x48] t1 (t1 > t0 이어야 함) */
    movl $NX_CLOCK_MONOTONIC, %edi
    int $0x80
    movq %rax, 0x48(%rbx)

    movl $NX_SYS_SYSINFO, %eax       /* [0x50] NxSysInfo(&SYSINFO_BUF) -> 0 */
    movabsq $SYSINFO_BUF, %rdi
    syscall
    movq %rax, 0x50(%rbx)

    movl $NX_SYS_OPEN, %eax          /* [0x58] NxOpen(path, READ) -> 핸들 */
    leaq path_hello(%rip), %rdi
    movl $NX_O_READ, %esi
    syscall
    movq %rax, 0x58(%rbx)
    movq %rax, %r12                  /* r12 = 파일 핸들 (이후 재사용) */

    movl $NX_SYS_READ, %eax          /* [0x60] 5바이트 읽기 -> "Hello" */
    movq %r12, %rdi
    movabsq $READ_BUF, %rsi
    movl $5, %edx
    syscall
    movq %rax, 0x60(%rbx)

    movl $NX_SYS_READ, %eax          /* [0x68] 나머지(최대 64바이트) 읽기 */
    movq %r12, %rdi
    movabsq $READ_BUF + 5, %rsi
    movl $64, %edx
    int $0x80
    movq %rax, 0x68(%rbx)

    movl $NX_SYS_STAT, %eax          /* [0x70] NxStat(handle, &STAT_BUF) -> 0 */
    movq %r12, %rdi
    movabsq $STAT_BUF, %rsi
    syscall
    movq %rax, 0x70(%rbx)

    movl $NX_SYS_SEEK, %eax          /* [0x78] NxSeek(handle, 0, SET) -> 0 */
    movq %r12, %rdi
    xorl %esi, %esi
    movl $NX_SEEK_SET, %edx
    syscall
    movq %rax, 0x78(%rbx)

    movl $NX_SYS_READ, %eax          /* [0x80] seek 이후 다시 5바이트 -> "Hello" */
    movq %r12, %rdi
    movabsq $READ_BUF + 128, %rsi
    movl $5, %edx
    syscall
    movq %rax, 0x80(%rbx)

    movl $NX_SYS_DUPLICATE_HANDLE, %eax  /* [0x88] 전체 권한(ALL)을 요청해도 원본의 READ 권한을 넘지 못함 */
    movq %r12, %rdi
    movl $NX_RIGHT_ALL, %esi
    syscall
    movq %rax, 0x88(%rbx)
    movq %rax, %r13                  /* r13 = 복제된 핸들 */

    movl $NX_SYS_WRITE, %eax         /* [0x90] 복제 핸들에 쓰기 시도 -> Npermission (권한 상승 불가 확인) */
    movq %r13, %rdi
    movabsq $READ_BUF, %rsi
    movl $1, %edx
    syscall
    movq %rax, 0x90(%rbx)

    movl $NX_SYS_CLOSE, %eax         /* [0x98] 원본 핸들 닫기 -> 0 */
    movq %r12, %rdi
    syscall
    movq %rax, 0x98(%rbx)

    movl $NX_SYS_CLOSE, %eax         /* [0xA0] 복제 핸들 닫기 -> 0 */
    movq %r13, %rdi
    syscall
    movq %rax, 0xA0(%rbx)

    movl $NX_SYS_CLOSE, %eax         /* [0xA8] 이미 닫은 핸들을 또 닫기 -> NinvalidArg (재사용/이중 close 방지 확인) */
    movq %r12, %rdi
    syscall
    movq %rax, 0xA8(%rbx)

    movl $NX_SYS_WRITE, %eax         /* [0xB0] stdout 에 쓰기 -> 쓴 바이트 수 */
    movl $NX_HANDLE_STDOUT, %edi
    leaq wmsg(%rip), %rsi
    movl $wmsg_len, %edx
    syscall
    movq %rax, 0xB0(%rbx)

    movl $NX_SYS_DUPLICATE_HANDLE, %eax  /* [0xB8] stdout 복제 */
    movl $NX_HANDLE_STDOUT, %edi
    movl $NX_RIGHT_ALL, %esi
    syscall
    movq %rax, 0xB8(%rbx)
    movq %rax, %r12                  /* r12 = stdout 복제 핸들 */

    movl $NX_SYS_WRITE, %eax         /* [0xC0] 복제된 stdout 에 쓰기 */
    movq %r12, %rdi
    leaq wmsg2(%rip), %rsi
    movl $wmsg2_len, %edx
    int $0x80
    movq %rax, 0xC0(%rbx)

    movl $NX_SYS_CLOSE, %eax         /* [0xC8] */
    movq %r12, %rdi
    syscall
    movq %rax, 0xC8(%rbx)

    movl $NX_SYS_PROCESS_INFO, %eax  /* [0xD0] NxProcessInfo(SELF, &PROCINFO_BUF) -> 0 */
    movl $NX_PID_SELF, %edi
    movabsq $PROCINFO_BUF, %rsi
    syscall
    movq %rax, 0xD0(%rbx)

    movl $NX_SYS_PROCESS_EXIT, %eax  /* NxProcessExit(0) */
    xorl %edi, %edi
    syscall
1:  jmp 1b

umsg:
    .asciz "hello from ring 3 (syscall + int 0x80)\n"
path_hello:
    .asciz "app:/hellowld.run"
wmsg:
    .ascii "ring3 stdout write\n"
    .set wmsg_len, . - wmsg
wmsg2:
    .ascii "ring3 dup(stdout) write\n"
    .set wmsg2_len, . - wmsg2
user_prog1_end:

    /* 특권 명령(hlt)을 실행 -> #GP -> 이 프로세스만 종료되고 커널은 계속 동작해야 한다 */
    .global user_prog2_start
    .global user_prog2_end
user_prog2_start:
    hlt
1:  jmp 1b
user_prog2_end:

    .section .note.GNU-stack,"",@progbits
