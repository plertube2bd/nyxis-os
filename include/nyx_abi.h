/*
 * nyx_abi.h - Nyxis 시스템 콜 ABI (커널과 유저랜드가 함께 쓰는 공개 헤더)
 *
 * 이 헤더는 "커널 내부 타입" 에 의존하지 않는다. (stdint.h 의 고정폭 정수만 사용)
 * 여기에 있는 구조체의 크기/배치는 ABI 이므로 바꾸면 안 된다. (NX_STATIC_ASSERT 는 커널 쪽에서 검사)
 *
 * 호출 규약 (int 0x80 / syscall 공통):
 *   rax = 번호, rdi, rsi, rdx, r10, r8, r9 = 인자 0~5
 *   반환 rax >= 0 성공(값의 의미는 각 호출 설명), rax < 0 오류 (Nstatus 부호 확장)
 *   syscall 명령은 rcx, r11 을 파괴한다.
 *
 * 핸들: u64. 상위 32비트 = 세대(generation), 하위 32비트 = 슬롯 번호.
 *       닫힌 핸들의 슬롯이 재사용되면 세대가 올라가므로 오래된 핸들 값은 거부된다.
 *       각 프로세스는 시작할 때 0=stdin, 1=stdout, 2=stderr 를 가진다 (첫 세대이므로 값 == 슬롯 번호).
 */
#ifndef NYX_ABI_H
#define NYX_ABI_H

#include <stdint.h>

/* ---------------- 시스템 콜 번호 ---------------- */
#define NX_SYS_GET_VERSION      2U
#define NX_SYS_GET_TIME         3U
#define NX_SYS_SLEEP            4U
#define NX_SYS_YIELD            5U
#define NX_SYS_SYSINFO          7U

#define NX_SYS_OPEN             32U
#define NX_SYS_CLOSE            33U
#define NX_SYS_READ             34U
#define NX_SYS_WRITE            35U
#define NX_SYS_SEEK             36U
#define NX_SYS_STAT             37U
#define NX_SYS_DUPLICATE_HANDLE 41U

/*
 * NxProcessCreate(const char *path) -> pid (실행 파일을 열 수 없거나 ELF 로 인식할 수 없으면
 * 음수 Nstatus). v1 은 인자를 경로 하나만 받는다 — argv/envp 전달은 아직 없다 (crt0 은
 * argc=0, argv=[NULL], envp=[NULL] 로 시작한다). 새 프로세스는 부모와 완전히 독립된
 * 주소 공간을 갖고, 자신만의 stdin/stdout/stderr(콘솔에 새로 연결됨, nx_handles_init)를
 * 갖는다 — 부모의 다른 핸들을 상속하지는 않는다.
 */
#define NX_SYS_PROCESS_CREATE   64U
#define NX_SYS_PROCESS_EXIT     65U
#define NX_SYS_PROCESS_INFO     68U

/*
 * NxVirtualAlloc(increment) -> 새 brk 가상 주소 (u64, 항상 >= 0 이므로 오류와 헷갈리지 않는다)
 *   increment 는 부호 있는 바이트 수(i64 로 해석): 양수면 힙을 늘리고, 음수면 줄이고,
 *   0 이면 아무것도 바꾸지 않고 현재 brk 를 그대로 돌려준다 (고전적인 sbrk(0) 관용구).
 *   현재는 brk(증가분) 방식만 지원한다 — 주소/길이를 직접 지정하는 mmap 스타일 할당이나
 *   보호 속성 변경은 아직 없다 (NxVirtualFree(97) 은 그 미래 API를 위해 번호만 예약해
 *   두었고, v1 에서는 구현하지 않는다: 지금은 NxVirtualAlloc 에 음수를 넘기는 것이 "해제"다).
 */
#define NX_SYS_VIRTUAL_ALLOC    96U

#define NX_SYS_KERNEL_PRINT     771U
#define NX_SYS_DEBUG_NOP        777U

/* ---------------- 버전 ---------------- */
/* NxGetVersion 의 반환값: (major << 32) | (minor << 16) | patch */
#define NX_ABI_MAJOR   1UL
#define NX_ABI_MINOR   1UL
#define NX_ABI_PATCH   0UL
#define NX_ABI_VERSION ((NX_ABI_MAJOR << 32) | (NX_ABI_MINOR << 16) | NX_ABI_PATCH)

/* ---------------- 핸들 ---------------- */
#define NX_HANDLE_STDIN   0UL
#define NX_HANDLE_STDOUT  1UL
#define NX_HANDLE_STDERR  2UL

/* 핸들에 붙는 권한 (NxDuplicateHandle 은 권한을 "줄이기만" 할 수 있다) */
#define NX_RIGHT_READ   0x01UL
#define NX_RIGHT_WRITE  0x02UL
#define NX_RIGHT_SEEK   0x04UL
#define NX_RIGHT_STAT   0x08UL
#define NX_RIGHT_DUP    0x10UL
#define NX_RIGHT_ALL    0x1FUL

/* ---------------- NxOpen 플래그 ---------------- */
#define NX_O_READ   0x1UL     /* 읽기 */
#define NX_O_WRITE  0x2UL     /* 쓰기 (읽기 전용 파일시스템에서는 NreadOnly) */
#define NX_O_MASK   0x3UL

/* ---------------- NxSeek ---------------- */
#define NX_SEEK_SET 0UL
#define NX_SEEK_CUR 1UL
#define NX_SEEK_END 2UL

/* ---------------- NxGetTime ---------------- */
#define NX_CLOCK_MONOTONIC 0UL   /* 부팅 후 경과 시간(나노초). 해상도는 타이머 주기(현재 10ms) */
/*
 * NX_CLOCK_REALTIME(1): 번호만 예약해 둔다. 커널에 RTC(실시간 시계) 드라이버가 아직 없어서
 * NxGetTime 은 이 clock_id 에 대해 Nunsupported 를 반환한다. libc 의 time()/gettimeofday()
 * 는 이 오류를 (time_t)-1 로 변환해 알려야 한다(정하지 않은 값을 지어내지 않는다) — 실제
 * 벽시계가 생기기 전까지는 이것이 "모른다"를 정직하게 표현하는 유일한 방법이다.
 */
#define NX_CLOCK_REALTIME  1UL

/* ---------------- 제한 ---------------- */
#define NX_PATH_MAX        256UL      /* NxOpen 경로 (NUL 포함) */
#define NX_IO_MAX          0x100000UL /* 한 번의 읽기/쓰기 최대 바이트 (넘으면 잘라서 처리) */
#define NX_MAX_HANDLES     32UL       /* 프로세스당 핸들 수 */
#define NX_SLEEP_MAX_MS    3600000UL  /* NxSleep 최대 1시간 */
#define NX_PID_SELF        0xFFFFFFFFUL

/* NxVirtualAlloc 남용을 막는 상한(기초적인 DoS 방지). 물리 프레임 할당자는 유한하므로
 * 프로세스 하나가 한 번의 호출로, 또는 누적으로 전체 RAM 을 다 가져가지 못하게 한다. */
#define NX_HEAP_GROW_MAX_PER_CALL (256UL * 1024UL * 1024UL)  /* 한 번의 NxVirtualAlloc 상한: 256MiB */
#define NX_HEAP_MAX_TOTAL         (512UL * 1024UL * 1024UL)  /* 프로세스당 힙 총량 상한: 512MiB */

/* ---------------- 구조체 ---------------- */

/* 파일 종류 */
#define NX_TYPE_FILE     1U
#define NX_TYPE_DIR      2U
#define NX_TYPE_CONSOLE  3U

/* NxStat: 32바이트 */
struct nx_stat {
    uint64_t size;
    uint32_t type;        /* NX_TYPE_* */
    uint32_t rights;      /* 이 핸들이 가진 NX_RIGHT_* */
    uint64_t reserved[2]; /* 0 */
};

/* NxSysInfo: 48바이트 */
struct nx_sysinfo {
    uint32_t abi_version_major;
    uint32_t abi_version_minor;
    uint32_t page_size;
    uint32_t timer_hz;
    uint64_t ram_bytes;       /* 사용 가능한 RAM 의 끝 주소(대략적인 총량) */
    uint64_t uptime_ns;
    uint32_t process_count;   /* 살아 있는 프로세스 수 */
    uint32_t reserved0;
    uint64_t reserved1;
};

/* NxProcessInfo: 32바이트 */
struct nx_procinfo {
    uint32_t pid;
    uint32_t state;           /* 0=READY 1=RUNNING 2=BLOCKED 3=TERMINATED */
    uint32_t handle_count;    /* 열려 있는 핸들 수 */
    uint32_t reserved0;
    uint64_t reserved[2];
};

#endif /* NYX_ABI_H */
