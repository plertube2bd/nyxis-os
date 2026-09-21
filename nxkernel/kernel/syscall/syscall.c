/*
 * syscall.c - 시스템 콜 디스패치와 구현 (int 0x80 / syscall 공통)
 *
 * 구조:
 *   - 시스템 콜은 "번호 -> 핸들러" 표(g_syscalls)로 선언한다. 표에 없는 번호는 NsyscallFailed.
 *   - 핸들러는 사용자 값을 절대 신뢰하지 않는다:
 *       * 사용자 포인터는 uaccess.h 의 copy_from_user/copy_to_user/copy_string_from_user 로만 접근
 *       * 객체는 핸들 테이블(handles.h)에서 "권한까지" 확인한 것만 사용
 *       * 길이/플래그/오프셋은 상한과 오버플로를 검사
 *   - 반환은 Nstatus 를 64비트로 부호 확장한 값 (음수 = 오류) 또는 호출별 양수 결과.
 *
 * [수정 이력 요약] (기존 코드의 문제는 git 히스토리/docs/AUDIT.md 참고)
 *  - 유저 포인터 검증 없는 printk(message) -> 검증 + 서식 문자열 아님 + 제어 문자 제거 출력.
 *  - 시스템 콜을 switch 에서 표 기반으로 바꾸고 핵심 호출(시간/핸들/파일/프로세스 정보)을 구현.
 */

#include "kernel/syscall/syscall.h"
#include "kernel/syscall/uaccess.h"
#include "kernel/syscall/handles.h"
#include "kernel/paging/paging.h"
#include "kernel/process/process.h"
#include "kernel/timer/pit/pit_base.h"
#include "console/outputs/printk.h"
#include "drivers/filesystem/vfs.h"
#include "boot_info.h"
#include "kernel/kernel.h"
#include "lowlevel.h"
#include "memory.h"
#include "phys.h"
#include "string.h"

/* ABI 구조체 크기는 유저랜드와의 약속이므로 컴파일 타임에 고정한다 */
NX_STATIC_ASSERT(nx_stat_size, sizeof(struct nx_stat) == 32);
NX_STATIC_ASSERT(nx_sysinfo_size, sizeof(struct nx_sysinfo) == 48);
NX_STATIC_ASSERT(nx_procinfo_size, sizeof(struct nx_procinfo) == 32);

/* 한 번에 커널 버퍼로 옮기는 크기 (커널 스택 사용량을 작게 유지) */
#define IO_CHUNK 512U

typedef i64 (*syscall_fn)(const u64 *a);

/* 현재 프로세스의 핸들 테이블 (현재 프로세스가 없으면 NULL) */
static nx_handle_table_t *cur_handles(void)
{
    return current_process ? &current_process->handles : (nx_handle_table_t *)0;
}

/* ------------------------------------------------------------------ */
/* Core                                                                */
/* ------------------------------------------------------------------ */

static i64 sys_get_version(const u64 *a)
{
    (void)a;
    return (i64)NX_ABI_VERSION;
}

/* NxGetTime(clock_id) -> 나노초 */
static i64 sys_get_time(const u64 *a)
{
    if (a[0] != NX_CLOCK_MONOTONIC)
        return (i64)Nunsupported;      /* 실시간(RTC) 시계는 아직 없다 */

    return (i64)timer_get_ns();
}

/* NxSleep(milliseconds) */
static i64 sys_sleep(const u64 *a)
{
    u32 hz = timer_get_hz();
    u64 ticks;

    if (a[0] > NX_SLEEP_MAX_MS)
        return (i64)NinvalidArg;
    if (hz == 0)
        return (i64)Nunsupported;

    ticks = (a[0] * (u64)hz + 999UL) / 1000UL;     /* 올림 */
    return (i64)process_sleep_ticks(ticks);
}

static i64 sys_yield(const u64 *a)
{
    (void)a;
    schedule();
    return 0;
}

/* NxSysInfo(struct nx_sysinfo *out) */
static i64 sys_sysinfo(const u64 *a)
{
    struct nx_sysinfo si;
    const NTBLI *info = get_kernel_info();

    memset(&si, 0, sizeof(si));
    si.abi_version_major = (u32)NX_ABI_MAJOR;
    si.abi_version_minor = (u32)NX_ABI_MINOR;
    si.page_size = (u32)PAGE_SIZE;
    si.timer_hz = timer_get_hz();
    si.ram_bytes = info ? info->memory_size : 0;      /* 크기만 알려준다 (주소는 노출하지 않는다) */
    si.uptime_ns = timer_get_ns();
    si.process_count = process_count();

    return (i64)copy_to_user(a[0], &si, sizeof(si));
}

/* ------------------------------------------------------------------ */
/* Object / Handle                                                     */
/* ------------------------------------------------------------------ */

/* NxOpen(const char *path, flags) -> 핸들 */
static i64 sys_open(const u64 *a)
{
    char path[NX_PATH_MAX];
    nx_handle_table_t *table = cur_handles();
    handle_t file;
    nx_handle_t *slot;
    u64 handle;
    u64 flags = a[1];
    u32 rights;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if ((flags & ~NX_O_MASK) != 0 || flags == 0)
        return (i64)NinvalidArg;

    status = copy_string_from_user(path, a[0], sizeof(path));
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    status = vfs_open(path, (u32)flags, &file);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    /*
     * 쓰기로 열려면 파일시스템이 실제로 쓰기를 지원해야 한다. 0바이트 쓰기로 능력을 확인한다.
     * (읽기 전용 파일시스템은 NreadOnly 를 돌려준다)
     */
    if (flags & NX_O_WRITE) {
        usize written = 0;
        char dummy = 0;

        status = vfs_write(&file, &dummy, 0, &written);
        if (NSTATUS_IS_ERR(status)) {
            (void)vfs_close(&file);
            return (i64)status;
        }
    }

    rights = (u32)(NX_RIGHT_SEEK | NX_RIGHT_STAT | NX_RIGHT_DUP);
    if (flags & NX_O_READ)
        rights |= (u32)NX_RIGHT_READ;
    if (flags & NX_O_WRITE)
        rights |= (u32)NX_RIGHT_WRITE;

    status = nx_handle_alloc(table, NX_HANDLE_FILE, rights, &handle, &slot);
    if (NSTATUS_IS_ERR(status)) {
        (void)vfs_close(&file);
        return (i64)status;
    }

    slot->file = file;
    return (i64)handle;
}

/* NxClose(handle) */
static i64 sys_close(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();

    if (!table)
        return (i64)NnotInitialized;
    return (i64)nx_handle_close(table, a[0]);
}

/* NxRead(handle, void *buf, len) -> 읽은 바이트 수 (0 = EOF) */
static i64 sys_read(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    u8 chunk[IO_CHUNK];
    u64 buf = a[1];
    u64 len = a[2];
    u64 total = 0;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;

    status = nx_handle_lookup(table, a[0], (u32)NX_RIGHT_READ, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    if (len == 0)
        return 0;
    if (len > NX_IO_MAX)
        len = NX_IO_MAX;                    /* 상한을 넘으면 잘라서 처리 (짧은 읽기는 정상) */

    if (slot->type != NX_HANDLE_FILE)
        return (i64)Nunsupported;           /* 콘솔 입력(키보드)은 아직 연결되지 않았다 */

    while (total < len) {
        usize want = (usize)((len - total) < IO_CHUNK ? (len - total) : IO_CHUNK);
        usize got = 0;

        status = vfs_read(&slot->file, chunk, want, &got);
        if (NSTATUS_IS_ERR(status))
            return total ? (i64)total : (i64)status;
        if (got == 0)
            break;                          /* EOF */

        status = copy_to_user(buf + total, chunk, got);
        if (NSTATUS_IS_ERR(status)) {
            slot->file.offset -= got;       /* 사용자에게 전달하지 못한 바이트는 읽지 않은 것으로 되돌린다 */
            return total ? (i64)total : (i64)status;
        }

        total += got;
        if (got < want)
            break;
    }

    return (i64)total;
}

/* NxWrite(handle, const void *buf, len) -> 쓴 바이트 수 */
static i64 sys_write(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    u8 chunk[IO_CHUNK];
    u64 buf = a[1];
    u64 len = a[2];
    u64 total = 0;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;

    status = nx_handle_lookup(table, a[0], (u32)NX_RIGHT_WRITE, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    if (len == 0)
        return 0;
    if (len > NX_IO_MAX)
        len = NX_IO_MAX;

    while (total < len) {
        usize n = (usize)((len - total) < IO_CHUNK ? (len - total) : IO_CHUNK);

        status = copy_from_user(chunk, buf + total, n);
        if (NSTATUS_IS_ERR(status))
            return total ? (i64)total : (i64)status;

        if (slot->type == NX_HANDLE_CONSOLE) {
            printk_write((const char *)chunk, n);
        } else {
            usize written = 0;

            status = vfs_write(&slot->file, chunk, n, &written);
            if (NSTATUS_IS_ERR(status))
                return total ? (i64)total : (i64)status;
            if (written < n) {
                total += written;
                break;
            }
        }

        total += n;
    }

    return (i64)total;
}

/* NxSeek(handle, offset(부호 있음), whence) -> 새 오프셋 */
static i64 sys_seek(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    vfs_stat_t st;
    i64 offset = (i64)a[1];
    i64 base;
    i64 target;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;

    status = nx_handle_lookup(table, a[0], (u32)NX_RIGHT_SEEK, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (slot->type != NX_HANDLE_FILE)
        return (i64)Nunsupported;

    switch (a[2]) {
    case NX_SEEK_SET:
        base = 0;
        break;
    case NX_SEEK_CUR:
        base = (i64)slot->file.offset;
        break;
    case NX_SEEK_END:
        status = vfs_fstat(&slot->file, &st);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        base = (i64)st.size;
        break;
    default:
        return (i64)NinvalidArg;
    }

    /* base 는 0 이상 2^62 이하이므로 offset 이 극단값이 아닌 한 덧셈은 안전하지만 명시적으로 검사한다 */
    if (base < 0 || base > 0x3FFFFFFFFFFFFFFFL)
        return (i64)Noverflow;
    if (offset > 0x3FFFFFFFFFFFFFFFL || offset < -0x3FFFFFFFFFFFFFFFL)
        return (i64)NinvalidArg;

    target = base + offset;
    if (target < 0)
        return (i64)NinvalidArg;

    slot->file.offset = (u64)target;
    return target;
}

/* NxStat(handle, struct nx_stat *out) */
static i64 sys_stat(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    struct nx_stat st;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;

    status = nx_handle_lookup(table, a[0], (u32)NX_RIGHT_STAT, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    memset(&st, 0, sizeof(st));
    st.rights = slot->rights;

    if (slot->type == NX_HANDLE_CONSOLE) {
        st.type = NX_TYPE_CONSOLE;
    } else {
        vfs_stat_t vs;

        status = vfs_fstat(&slot->file, &vs);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        st.size = vs.size;
        st.type = (vs.type == 2U) ? NX_TYPE_DIR : NX_TYPE_FILE;
    }

    return (i64)copy_to_user(a[1], &st, sizeof(st));
}

/* NxDuplicateHandle(handle, rights_mask) -> 새 핸들 (권한은 줄이기만 가능) */
static i64 sys_duplicate_handle(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    nx_handle_t *dup;
    u64 mask = a[1];
    u64 handle;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if ((mask & ~NX_RIGHT_ALL) != 0)
        return (i64)NinvalidArg;

    status = nx_handle_lookup(table, a[0], (u32)NX_RIGHT_DUP, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    status = nx_handle_alloc(table, slot->type, slot->rights & (u32)mask, &handle, &dup);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    if (slot->type == NX_HANDLE_CONSOLE) {
        dup->console_fd = slot->console_fd;
    } else {
        /* 드라이버 내부 상태를 공유하지 않도록 파일을 새로 연다 (오프셋은 복사, 이후 독립) */
        status = vfs_reopen(&slot->file, &dup->file);
        if (NSTATUS_IS_ERR(status)) {
            nx_handle_discard(dup);
            return (i64)status;
        }
    }

    return (i64)handle;
}

/* ------------------------------------------------------------------ */
/* Process                                                             */
/* ------------------------------------------------------------------ */

/* NxProcessExit(exit_code) - 반환하지 않는다 */
static i64 sys_process_exit(const u64 *a)
{
    process_exit_with_code((i32)(a[0] & 0xFFFFFFFFUL));
}

/* NxProcessInfo(pid, struct nx_procinfo *out): 지금은 자기 자신만 조회할 수 있다 */
static i64 sys_process_info(const u64 *a)
{
    struct nx_procinfo pi;
    process_t *self = current_process;

    if (!self)
        return (i64)NnotInitialized;

    /* 다른 프로세스의 정보는 권한 모델(capability)이 정해질 때까지 노출하지 않는다 */
    if (a[0] != NX_PID_SELF && a[0] != (u64)self->pid)
        return (i64)Npermission;

    memset(&pi, 0, sizeof(pi));
    pi.pid = self->pid;
    pi.state = self->state;
    pi.handle_count = nx_handles_count(&self->handles);

    return (i64)copy_to_user(a[1], &pi, sizeof(pi));
}

/* ------------------------------------------------------------------ */
/* Debug                                                               */
/* ------------------------------------------------------------------ */

/* NxKernelPrint(const char *str): 커널 로그에 문자열 출력 (서식 문자열이 아니며 제어 문자는 제거됨) */
static i64 sys_kernel_print(const u64 *a)
{
    char buffer[SYSCALL_STRING_MAX];
    Nstatus status = copy_string_from_user(buffer, a[0], sizeof(buffer));

    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    printk_write(buffer, strlen(buffer));
    return 0;
}

static i64 sys_debug_nop(const u64 *a)
{
    (void)a;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 표                                                                  */
/* ------------------------------------------------------------------ */

struct syscall_entry {
    u32 nr;
    syscall_fn fn;
};

static const struct syscall_entry g_syscalls[] = {
    { NX_SYS_GET_VERSION,      sys_get_version },
    { NX_SYS_GET_TIME,         sys_get_time },
    { NX_SYS_SLEEP,            sys_sleep },
    { NX_SYS_YIELD,            sys_yield },
    { NX_SYS_SYSINFO,          sys_sysinfo },

    { NX_SYS_OPEN,             sys_open },
    { NX_SYS_CLOSE,            sys_close },
    { NX_SYS_READ,             sys_read },
    { NX_SYS_WRITE,            sys_write },
    { NX_SYS_SEEK,             sys_seek },
    { NX_SYS_STAT,             sys_stat },
    { NX_SYS_DUPLICATE_HANDLE, sys_duplicate_handle },

    { NX_SYS_PROCESS_EXIT,     sys_process_exit },
    { NX_SYS_PROCESS_INFO,     sys_process_info },

    { NX_SYS_KERNEL_PRINT,     sys_kernel_print },
    { NX_SYS_DEBUG_NOP,        sys_debug_nop }
};

#define SYSCALL_COUNT (sizeof(g_syscalls) / sizeof(g_syscalls[0]))

i64 syscall_dispatch(
    u64 syscall_nr,
    u64 a0,
    u64 a1,
    u64 a2,
    u64 a3,
    u64 a4,
    u64 a5
) {
    u64 args[6];
    usize i;

    args[0] = a0;
    args[1] = a1;
    args[2] = a2;
    args[3] = a3;
    args[4] = a4;
    args[5] = a5;

    for (i = 0; i < SYSCALL_COUNT; i++) {
        if (g_syscalls[i].nr == syscall_nr)
            return g_syscalls[i].fn(args);
    }

    return (i64)NsyscallFailed;      /* 알 수 없는 번호: 커널은 영향받지 않는다 */
}

void syscall_handle(struct trap_frame *frame)
{
    i64 result = syscall_dispatch(
        frame->rax,
        frame->rdi,
        frame->rsi,
        frame->rdx,
        frame->r10,
        frame->r8,
        frame->r9
    );

    /* iretq/sysret 후 호출자의 rax 로 전달된다 */
    frame->rax = (u64)result;
}

/* ------------------------------------------------------------------ */
/* syscall / sysret                                                    */
/* ------------------------------------------------------------------ */

#define MSR_EFER         0xC0000080U
#define MSR_STAR         0xC0000081U
#define MSR_LSTAR        0xC0000082U
#define MSR_SFMASK       0xC0000084U
#define MSR_GS_BASE      0xC0000101U
#define MSR_KERNEL_GS    0xC0000102U
#define EFER_SCE         1UL

/* syscall 진입 시 RFLAGS 에서 지울 비트: IF(인터럽트), TF(단일 스텝), DF(방향), AC, NT */
#define SFMASK_VALUE     (0x200UL | 0x100UL | 0x400UL | 0x40000UL | 0x4000UL)

struct cpu_local g_cpu_local;

Nstatus syscall_init(void)
{
    u32 a, b, c, d;

    cpuid(0x80000000U, &a, &b, &c, &d);
    if (a < 0x80000001U)
        return NdeviceMissing;
    cpuid(0x80000001U, &a, &b, &c, &d);
    if (!(d & (1U << 11)))               /* SYSCALL/SYSRET 지원 여부 */
        return NdeviceMissing;

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR, ((u64)STAR_USER_BASE << 48) | ((u64)STAR_KERNEL_BASE << 32));
    wrmsr(MSR_LSTAR, (u64)(usize)syscall_entry);
    wrmsr(MSR_SFMASK, SFMASK_VALUE);

    /*
     * 커널은 GS 를 쓰지 않으므로 평상시 GS.base = 0.
     * syscall_entry.s 가 swapgs 로 GS.base 를 g_cpu_local 로 바꿨다가 sysret 직전에 되돌린다.
     */
    wrmsr(MSR_GS_BASE, 0);
    wrmsr(MSR_KERNEL_GS, (u64)(usize)&g_cpu_local);

    return NSTATUS_OK;
}

void syscall_bad_return(void)
{
    printk("syscall: invalid return address, terminating process\n");
    process_exit_with_code(-1);
}
