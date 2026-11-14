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
#include "kernel/process/spawn.h"
#include "kernel/process/umem.h"
#include "kernel/mm/kheap.h"
#include "console/inputs/tty.h"
#include "console/inputs/inputs_base.h"
#include "kernel/syscall/nspath.h"
#include "kernel/security/auth.h"
#include "kernel/ipc/channel.h"
#include "kernel/ipc/pipe.h"
#include "kernel/mm/shm.h"
#include "kernel/security/perm.h"
#include "kernel/syscall/syscall_internal.h"
#include "kernel/dev/device.h"
#include "kernel/acpi/acpi.h"
#include "drivers/ahci/ahci.h"
#include "kernel/process/signal.h"
#include "kernel/info/info.h"
#include "kernel/crypto/sha256.h"
#include "kernel/timer/pit/pit_base.h"
#include "kernel/timer/rtc/rtc.h"
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
NX_STATIC_ASSERT(nx_identity_size, sizeof(struct nx_identity) == 64);
NX_STATIC_ASSERT(nx_pollfd_size, sizeof(struct nx_pollfd) == 16);
NX_STATIC_ASSERT(nx_msginfo_size, sizeof(struct nx_msginfo) == 16);
NX_STATIC_ASSERT(nx_dirent_size, sizeof(struct nx_dirent) == 256);
NX_STATIC_ASSERT(nx_devinfo_size, sizeof(struct nx_devinfo) == 40);

/* 한 번에 커널 버퍼로 옮기는 크기 (커널 스택 사용량을 작게 유지) */
#define IO_CHUNK 512U

typedef i64 (*syscall_fn)(const u64 *a);

/* 현재 프로세스의 핸들 테이블 (현재 프로세스가 없으면 NULL) */
nx_handle_table_t *cur_handles(void)
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
    if (a[0] == NX_CLOCK_MONOTONIC)
        return (i64)timer_get_ns();
    if (a[0] == NX_CLOCK_REALTIME) {
        u64 ns;
        Nstatus st = rtc_realtime_ns(&ns);

        return NSTATUS_IS_ERR(st) ? (i64)st : (i64)ns;
    }
    return (i64)NinvalidArg;
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

/* 사용자 경로를 복사해 이름공간 규칙으로 해석한다 (nspath.h) */
static Nstatus user_path(u64 uptr, char *kpath, usize kmax, u32 *kind)
{
    char upath[NX_PATH_MAX];
    Nstatus status = copy_string_from_user(upath, uptr, sizeof(upath));

    if (NSTATUS_IS_ERR(status))
        return status;
    return nsp_translate(upath, kpath, kmax, kind);
}

/* 사용자 경로 -> VFS 경로 (장치/서비스 경로는 거부) + 커널 전용 경로 검사 */
Nstatus user_vfs_path(u64 uptr, char *kpath, usize kmax, u32 op)
{
    u32 kind = 0;
    Nstatus status = user_path(uptr, kpath, kmax, &kind);

    if (NSTATUS_IS_ERR(status))
        return status;
    if (kind == NSP_KIND_INFO)
        return Npermission;                 /* info:/ 는 읽기 전용 (만들기/지우기/실행 불가) */
    if (kind != NSP_KIND_VFS)
        return Nunsupported;
    return nsp_check_protected(kpath, op);
}

/* 콘솔 핸들의 터미널 (NULL = 물리 콘솔) */
static tty_t *slot_tty(const nx_handle_t *slot)
{
    return slot->tty ? slot->tty : tty_console();
}

/*
 * 표준 스트림 장치 (누구나 연다):
 *   dev:/tty                    콘솔 터미널 자체. 새 콘솔 핸들 (리디렉션과 무관하게 화면/키보드)
 *   dev:/stdin|stdout|stderr    호출자의 0/1/2 번 핸들이 가리키는 객체. 열린 파일/파이프를 공유하는
 *                               새 핸들이다 (dup 와 같다: 파일이면 오프셋도 공유). 리디렉션된 프로세스는
 *                               파일/파이프를 받는다. 그 번호가 닫혀 있으면 NnotFound.
 * 요청한 읽기/쓰기는 원래 핸들에 그 권한이 있어야 한다 (없으면 Npermission). close-on-exec 는 물려받지 않는다.
 */
static i64 open_stream(const char *name, u64 flags, nx_handle_table_t *table)
{
    nx_handle_t *src;
    nx_handle_t *slot;
    u64 handle;
    u32 want = 0;
    u32 index;
    Nstatus status;

    if (flags & NX_O_READ)
        want |= (u32)NX_RIGHT_READ;
    if (flags & NX_O_WRITE)
        want |= (u32)NX_RIGHT_WRITE;

    if (strcmp(name, "tty") == 0) {
        status = nx_handle_alloc(table, NX_HANDLE_CONSOLE, want | (u32)(NX_RIGHT_STAT | NX_RIGHT_DUP), &handle, &slot);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        slot->console_fd = (flags & NX_O_READ) ? 0U : 1U;     /* 0 = 입력을 받는 콘솔 핸들 (poll, 에코 설정) */
        slot->tty = current_process->ctty;                    /* 제어 터미널 (NULL = 물리 콘솔) */
        tty_retain(slot->tty);
        return (i64)handle;
    }

    index = (strcmp(name, "stdin") == 0) ? 0U : (strcmp(name, "stdout") == 0) ? 1U : 2U;
    src = &table->slots[index];
    if (src->type == NX_HANDLE_FREE)
        return (i64)NnotFound;
    if ((src->rights & want) != want)
        return (i64)Npermission;

    /* 읽기/쓰기는 요청한 것만, 나머지 권한(STAT/SEEK/DUP 등)은 원래 핸들 그대로 */
    status = nx_handle_alloc(table, src->type,
                             src->rights & ~((u32)(NX_RIGHT_READ | NX_RIGHT_WRITE) & ~want), &handle, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = nx_handle_share(src, slot, true);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(slot);
        return (i64)status;
    }
    return (i64)handle;
}

static i64 open_device(const char *name, u64 flags, nx_handle_table_t *table)
{
    nx_device_t *dev = nNULL;
    nx_handle_t *slot;
    u64 handle;
    u32 rights = (u32)(NX_RIGHT_SEEK | NX_RIGHT_STAT | NX_RIGHT_DEVICE);
    bool write = (flags & NX_O_WRITE) ? true : false;
    Nstatus status;

    if (flags & NX_O_READ)
        rights |= (u32)NX_RIGHT_READ;
    if (write)
        rights |= (u32)NX_RIGHT_WRITE;

    if (nsp_dev_is_public(name))
        return open_stream(name, flags, table);

    status = device_open(name, write, &dev);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    status = nx_handle_alloc(table, NX_HANDLE_DEVICE, rights, &handle, &slot);
    if (NSTATUS_IS_ERR(status)) {
        device_close(dev, dev->owner, write);
        return (i64)status;
    }
    slot->dev = dev;
    slot->dev_write = write;
    slot->dev_offset = 0;
    return (i64)handle;
}

static i64 open_service(const char *name, u64 flags, nx_handle_table_t *table)
{
    channel_t *c = nNULL;
    nx_handle_t *slot;
    u64 handle;
    Nstatus status;

    if (flags != NX_O_WRITE)
        return (i64)NinvalidArg;            /* 서비스는 보내기 전용으로 연다 (받는 쪽은 NxChannelCreate) */

    status = channel_connect(name, &c);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    status = nx_handle_alloc(table, NX_HANDLE_CHANNEL, (u32)(NX_RIGHT_SEND | NX_RIGHT_STAT | NX_RIGHT_DUP),
                             &handle, &slot);
    if (NSTATUS_IS_ERR(status)) {
        channel_release(c, false);
        return (i64)status;
    }
    slot->chan = c;
    slot->chan_receiver = false;
    return (i64)handle;
}

/* info:/ 가상 파일: 열 때 내용을 만들어 핸들에 보관한다 (info.h) */
static i64 open_info(const char *name, u64 flags, nx_handle_table_t *table)
{
    nx_handle_t *slot;
    char *buf = nNULL;
    usize len = 0;
    u32 type = 0;
    u64 handle;
    Nstatus status;

    if (flags & NX_O_WRITE)
        return (i64)Npermission;
    status = info_open(name, &type, &buf, &len);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    status = nx_handle_alloc(table, NX_HANDLE_INFO,
                             (u32)(NX_RIGHT_READ | NX_RIGHT_SEEK | NX_RIGHT_STAT | NX_RIGHT_DUP), &handle, &slot);
    if (NSTATUS_IS_ERR(status)) {
        kfree(buf);
        return (i64)status;
    }
    slot->info_buf = buf;
    slot->info_len = len;
    slot->info_offset = 0;
    slot->info_dir = (type == INFO_NODE_DIR);
    return (i64)handle;
}

/* NxOpen(const char *path, flags) -> 핸들 */
static i64 sys_open(const u64 *a)
{
    char kpath[NSP_PATH_MAX];
    nx_handle_table_t *table = cur_handles();
    handle_t file;
    nx_handle_t *slot;
    u64 handle;
    u64 flags = a[1];
    u64 flags_nodes;
    u32 rights;
    u32 kind = 0;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if ((flags & ~NX_O_MASK) != 0 || flags == 0)
        return (i64)NinvalidArg;
    if ((flags & NX_O_TRUNC) && !(flags & NX_O_WRITE))
        return (i64)NinvalidArg;               /* 자르기는 쓰기와 함께만 */

    status = user_path(a[0], kpath, sizeof(kpath), &kind);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    flags_nodes = flags;
    flags &= ~(u64)NX_O_TRUNC;                  /* 장치/서비스/info 에는 의미가 없다 */
    if (kind == NSP_KIND_DEV)
        return open_device(kpath, flags, table);
    if (kind == NSP_KIND_SVC)
        return open_service(kpath, flags, table);
    if (kind == NSP_KIND_INFO)
        return open_info(kpath, flags, table);

    status = nsp_check_protected(kpath, NSP_OP_OPEN);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    status = vfs_open(kpath, (u32)(flags & (NX_O_READ | NX_O_WRITE)), &file);
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
        if (!NSTATUS_IS_ERR(status) && (flags_nodes & NX_O_TRUNC))
            status = vfs_truncate(&file);       /* 내용만 0 바이트로 (자식은 그대로) */
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

    slot->of = nx_ofile_new(&file);
    if (!slot->of) {
        nx_handle_discard(slot);
        (void)vfs_close(&file);
        return (i64)NoutOfMemory;
    }
    slot->of->append = (flags & NX_O_APPEND) ? true : false;
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

    if (slot->type == NX_HANDLE_CONSOLE) {
        /* 콘솔 입력: 완성된 줄이 생길 때까지 잠든다 (tty.h). 한 번에 최대 한 줄. 0 = EOF(Ctrl-D) */
        usize got = 0;

        status = tty_read(slot_tty(slot), (char *)chunk, (usize)(len < IO_CHUNK ? len : IO_CHUNK), &got);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        status = copy_to_user(buf, chunk, got);   /* 실패하면 이 줄은 버려진다 (사용자 버퍼가 잘못됨) */
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        return (i64)got;
    }

    if (slot->type == NX_HANDLE_PTY)
        return pty_master_read_user(slot->pty, buf, len);

    if (slot->type == NX_HANDLE_DEVICE && slot->dev->kind == NX_DEV_CONSOLE)
        return device_console_read_user(slot->dev, current_process, buf, len);

    if (slot->type == NX_HANDLE_DEVICE) {
        if (slot->dev->kind == NX_DEV_BLOCK && (len % NX_BLOCK_SECTOR) != 0)
            return (i64)NinvalidArg;        /* 블록 장치는 섹터 단위 */
        while (total < len) {
            usize want = (usize)((len - total) < IO_CHUNK ? (len - total) : IO_CHUNK);
            usize got = 0;

            status = device_read(slot->dev, &slot->dev_offset, chunk, want, &got);
            if (NSTATUS_IS_ERR(status))
                return total ? (i64)total : (i64)status;
            if (got == 0)
                break;
            status = copy_to_user(buf + total, chunk, got);
            if (NSTATUS_IS_ERR(status))
                return total ? (i64)total : (i64)status;
            total += got;
            if (got < want || slot->dev->kind != NX_DEV_BLOCK)
                break;                      /* 스트림은 지금 있는 만큼만 */
        }
        return (i64)total;
    }

    if (slot->type == NX_HANDLE_INFO) {
        u64 n;

        if (slot->info_dir)
            return (i64)NinvalidArg;        /* 디렉터리는 NxReadDir 로 */
        if (slot->info_offset >= slot->info_len)
            return 0;
        n = slot->info_len - slot->info_offset;
        if (n > len)
            n = len;
        status = copy_to_user(buf, slot->info_buf + slot->info_offset, (usize)n);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        slot->info_offset += n;
        return (i64)n;
    }

    if (slot->type == NX_HANDLE_PIPE) {
        if (slot->pipe_write)
            return (i64)Npermission;
        return pipe_read_user(slot->pipe, buf, len);
    }

    if (slot->type != NX_HANDLE_FILE)
        return (i64)Nunsupported;

    while (total < len) {
        usize want = (usize)((len - total) < IO_CHUNK ? (len - total) : IO_CHUNK);
        usize got = 0;

        status = vfs_read(&slot->of->file, chunk, want, &got);
        if (NSTATUS_IS_ERR(status))
            return total ? (i64)total : (i64)status;
        if (got == 0)
            break;                          /* EOF */

        status = copy_to_user(buf + total, chunk, got);
        if (NSTATUS_IS_ERR(status)) {
            slot->of->file.offset -= got;       /* 사용자에게 전달하지 못한 바이트는 읽지 않은 것으로 되돌린다 */
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
    if (slot->type == NX_HANDLE_DEVICE && slot->dev->kind == NX_DEV_BLOCK && (len % NX_BLOCK_SECTOR) != 0)
        return (i64)NinvalidArg;
    if (slot->type == NX_HANDLE_PIPE) {
        if (!slot->pipe_write)
            return (i64)Npermission;
        return pipe_write_user(slot->pipe, buf, len);
    }
    if (slot->type == NX_HANDLE_PTY)
        return pty_master_write_user(slot->pty, buf, len);
    if (slot->type == NX_HANDLE_FILE && slot->of->append) {
        /* 끝으로 옮기고 바로 쓴다: 커널 안에서는 선점되지 않으므로 다른 쓰기가 끼어들지 않는다 */
        vfs_stat_t st;

        status = vfs_fstat(&slot->of->file, &st);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        slot->of->file.offset = st.size;
    }

    while (total < len) {
        usize n = (usize)((len - total) < IO_CHUNK ? (len - total) : IO_CHUNK);

        status = copy_from_user(chunk, buf + total, n);
        if (NSTATUS_IS_ERR(status))
            return total ? (i64)total : (i64)status;

        if (slot->type == NX_HANDLE_CONSOLE) {
            /* 터미널 장치로 바로 (printk 를 거치지 않는다). pty 는 주 끝이 읽을 때까지 잠들 수 있다 */
            usize done = 0;

            status = tty_write(slot_tty(slot), (const char *)chunk, n, &done);
            if (NSTATUS_IS_ERR(status))
                return total ? (i64)total : (i64)status;
            if (done < n) {
                total += done;
                break;
            }
        } else if (slot->type == NX_HANDLE_DEVICE) {
            usize done = 0;

            status = device_write(slot->dev, current_process, slot->dev_write, &slot->dev_offset, chunk, n, &done);
            if (NSTATUS_IS_ERR(status))
                return total ? (i64)total : (i64)status;
            if (done < n) {
                total += done;
                break;
            }
        } else if (slot->type == NX_HANDLE_FILE) {
            usize written = 0;

            status = vfs_write(&slot->of->file, chunk, n, &written);
            if (NSTATUS_IS_ERR(status))
                return total ? (i64)total : (i64)status;
            if (written < n) {
                total += written;
                break;
            }
        } else {
            return (i64)Nunsupported;
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
    u64 *pos;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;

    status = nx_handle_lookup(table, a[0], (u32)NX_RIGHT_SEEK, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (slot->type == NX_HANDLE_FILE)
        pos = &slot->of->file.offset;
    else if (slot->type == NX_HANDLE_DEVICE && slot->dev->kind == NX_DEV_BLOCK)
        pos = &slot->dev_offset;
    else if (slot->type == NX_HANDLE_INFO && !slot->info_dir)
        pos = &slot->info_offset;
    else
        return (i64)Nunsupported;

    switch (a[2]) {
    case NX_SEEK_SET:
        base = 0;
        break;
    case NX_SEEK_CUR:
        base = (i64)*pos;
        break;
    case NX_SEEK_END:
        if (slot->type == NX_HANDLE_DEVICE) {
            base = (i64)slot->dev->size;
        } else if (slot->type == NX_HANDLE_INFO) {
            base = (i64)slot->info_len;
        } else {
            status = vfs_fstat(&slot->of->file, &st);
            if (NSTATUS_IS_ERR(status))
                return (i64)status;
            base = (i64)st.size;
        }
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

    *pos = (u64)target;
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
    } else if (slot->type == NX_HANDLE_PROCESS) {
        st.type = NX_TYPE_PROCESS;
    } else if (slot->type == NX_HANDLE_TOKEN) {
        st.type = NX_TYPE_TOKEN;
    } else if (slot->type == NX_HANDLE_CHANNEL) {
        st.type = NX_TYPE_CHANNEL;
        st.size = slot->chan->count;
    } else if (slot->type == NX_HANDLE_DEVICE) {
        st.type = NX_TYPE_DEVICE;
        st.size = slot->dev->size;
    } else if (slot->type == NX_HANDLE_INFO) {
        st.type = slot->info_dir ? NX_TYPE_DIR : NX_TYPE_FILE;
        st.size = slot->info_len;
        st.mode = slot->info_dir ? 0555U : 0444U;   /* 누구나 읽기만 */
    } else if (slot->type == NX_HANDLE_SHM) {
        st.type = NX_TYPE_SHM;
        st.size = slot->shm->size;
  } else if (slot->type == NX_HANDLE_PTY) {
        st.type = NX_TYPE_PTY;
    } else if (slot->type == NX_HANDLE_PIPE) {
        st.type = NX_TYPE_PIPE;
        st.size = slot->pipe->count;
        st.mode = slot->pipe_write ? 0200U : 0400U;
        st.uid = current_process ? current_process->uid : 0;
        st.gid = current_process ? current_process->gid : 0;
    } else {
        vfs_stat_t vs;

        status = vfs_fstat(&slot->of->file, &vs);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        perm_fill_stat(&vs, &st);
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

    /* 토큰(일회용), 장치(독점 소유), 채널 수신 핸들(받는 쪽은 하나)은 복제할 수 없다 */
    if (slot->type == NX_HANDLE_TOKEN || slot->type == NX_HANDLE_DEVICE ||
        (slot->type == NX_HANDLE_CHANNEL && slot->chan_receiver))
        return (i64)Nunsupported;

    status = nx_handle_alloc(table, slot->type, slot->rights & (u32)mask, &handle, &dup);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    /* 예전 의미 그대로: 파일은 새로 열어 오프셋만 복사한다 (공유는 NxDuplicateHandleEx 의 NX_DUP_SHARE) */
    status = nx_handle_share(slot, dup, false);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(dup);
        return (i64)status;
    }

    return (i64)handle;
}

/* ------------------------------------------------------------------ */
/* Process                                                             */
/* ------------------------------------------------------------------ */

/*
 * 사용자 문자열 포인터 배열(NULL 로 끝남)을 blob 에 이어 붙인다. user_vec 이 0 이면 빈 배열.
 * blob 의 사용량(*used, 최대 NX_ARG_MAX)과 항목 수(*count, argv+envp 합계 최대 NX_ARGV_MAX)를 갱신한다.
 */
Nstatus collect_user_strings(u64 user_vec, char *blob, usize *used, u32 *count)
{
    u64 i;

    if (user_vec == 0)
        return NSTATUS_OK;

    for (i = 0; ; i++) {
        u64 ptr = 0;
        Nstatus status = copy_from_user(&ptr, user_vec + i * 8UL, sizeof(ptr));

        if (NSTATUS_IS_ERR(status))
            return status;
        if (ptr == 0)
            return NSTATUS_OK;
        if (*count >= NX_ARGV_MAX || *used >= NX_ARG_MAX)
            return Noverflow;

        status = copy_string_from_user(blob + *used, ptr, (usize)(NX_ARG_MAX - *used));
        if (status == NpathTooLong)
            return Noverflow;               /* 문자열 합계가 NX_ARG_MAX 를 넘는다 */
        if (NSTATUS_IS_ERR(status))
            return status;

        *used += strlen(blob + *used) + 1U;
        (*count)++;
    }
}

/*
 * NxProcessCreate(const char *path, const char *const *argv, const char *const *envp, token) -> 프로세스 핸들
 *   token 이 NX_NO_TOKEN 이 아니면 그 토큰(NX_RIGHT_SPAWN)의 신원으로 시작하고, 성공하면 토큰은 소모된다.
 */
static i64 sys_process_create(const u64 *a)
{
    char path[NSP_PATH_MAX];
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    nx_handle_t *token_slot = nNULL;
    spawn_args_t args;
    process_t *proc = nNULL;
    char *blob;
    usize used = 0;
    u32 argc = 0;
    u32 total = 0;
    u64 handle;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if (a[4] & ~(NX_SPAWN_INHERIT | NX_SPAWN_CONSOLE_SESSION | NX_SPAWN_CRITICAL))
        return (i64)NinvalidArg;
    if ((a[4] & NX_SPAWN_CRITICAL) && (current_process->uid != 0 || !current_process->critical))
        return (i64)Npermission;            /* 보호 프로세스는 보호 프로세스(init)만 만든다 */
    if ((a[4] & NX_SPAWN_CONSOLE_SESSION) && current_process->uid != 0)
        return (i64)Npermission;            /* 콘솔 세션은 로그인(root)만 만든다 */

    status = user_vfs_path(a[0], path, sizeof(path), NSP_OP_OPEN);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    if (a[3] != NX_NO_TOKEN) {
        status = nx_handle_lookup(table, a[3], 0, &token_slot);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        if (token_slot->type != NX_HANDLE_TOKEN || !token_slot->token)
            return (i64)NinvalidArg;
        if (!(token_slot->rights & (u32)NX_RIGHT_SPAWN))
            return (i64)Npermission;
    }

    blob = (char *)kpage_alloc();           /* NX_ARG_MAX(4KiB) 를 커널 스택에 두지 않는다 */
    if (!blob)
        return (i64)NoutOfMemory;

    status = collect_user_strings(a[1], blob, &used, &total);
    argc = total;
    if (!NSTATUS_IS_ERR(status))
        status = collect_user_strings(a[2], blob, &used, &total);
    if (NSTATUS_IS_ERR(status)) {
        kpage_free(blob);
        return (i64)status;
    }

    /* 핸들 자리를 먼저 확보한다: 프로세스를 만든 뒤에 핸들이 모자라면 아무도 참조하지 않는 프로세스가 생긴다 */
    status = nx_handle_alloc(table, NX_HANDLE_PROCESS,
                             (u32)(NX_RIGHT_WAIT | NX_RIGHT_KILL | NX_RIGHT_STAT | NX_RIGHT_DUP),
                             &handle, &slot);
    if (NSTATUS_IS_ERR(status)) {
        kpage_free(blob);
        return (i64)status;
    }

    args.argc = argc;
    args.envc = total - argc;
    args.size = used;
    args.strings = blob;

    status = process_spawn(path, &args, &proc);
    kpage_free(blob);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(slot);
        return (i64)status;
    }

    if (token_slot) {
        /* 아직 한 번도 실행되지 않은(READY) 새 프로세스에 토큰의 신원을 입힌다. 시스템 콜 안은 인터럽트가
         * 꺼져 있고 커널은 비선점이므로 그 사이에 자식이 실행될 수 없다. 그리고 토큰을 소모한다 (일회용). */
        proc->uid = token_slot->token->uid;
        proc->gid = token_slot->token->gid;
        memcpy(proc->user_name, token_slot->token->name, sizeof(proc->user_name));
        (void)nx_handle_close(table, a[3]);
    }

    slot->proc = proc;                      /* process_spawn 이 준 참조를 핸들이 소유한다 */

    if (a[4] & NX_SPAWN_INHERIT) {
        /* 자식은 아직 실행되지 않았다: CLOEXEC 가 아닌 핸들을 같은 슬롯에 물려준다 (세대 0) */
        status = nx_handles_inherit(&proc->handles, table, false, true);
        if (NSTATUS_IS_ERR(status)) {
            (void)process_kill(proc, NX_EXIT_KILLED);
            (void)nx_handle_close(table, handle);
            return (i64)status;
        }
    }
    proc->console_session = (a[4] & NX_SPAWN_CONSOLE_SESSION) ? true : false;
    proc->critical = (a[4] & NX_SPAWN_CRITICAL) ? true : false;
    process_adopt_ctty(proc, current_process->ctty, (a[4] & NX_SPAWN_INHERIT) ? false : true);
    return (i64)handle;
}

/* 프로세스 핸들을 찾아 대상 프로세스를 돌려준다 */
Nstatus lookup_process(u64 handle, u32 rights, process_t **out)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    Nstatus status;

    if (!table)
        return NnotInitialized;

    /* 종류를 먼저 본다: 프로세스 핸들이 아니면 권한과 무관하게 NinvalidArg (오류가 원인을 정확히 말하도록) */
    status = nx_handle_lookup(table, handle, 0, &slot);
    if (NSTATUS_IS_ERR(status))
        return status;
    if (slot->type != NX_HANDLE_PROCESS || !slot->proc)
        return NinvalidArg;
    if ((slot->rights & rights) != rights)
        return Npermission;

    *out = slot->proc;
    return NSTATUS_OK;
}

/* NxProcessKill(handle) */
static i64 sys_process_kill(const u64 *a)
{
    process_t *proc;
    Nstatus status = lookup_process(a[0], (u32)NX_RIGHT_KILL, &proc);

    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (signal_protected(proc, NX_SIGKILL, current_process->pid))
        return (i64)Npermission;            /* 보호 프로세스: 부모 말고는 못 끝낸다 */

    return (i64)process_kill(proc, (i32)NX_EXIT_KILLED);   /* 자기 자신의 핸들은 얻을 수 없다 */
}

/* NxProcessGroup(NX_PID_SELF 또는 프로세스 핸들, pgid) -> 그룹 번호 (nyx_abi.h) */
static i64 sys_process_group(const u64 *a)
{
    process_t *proc = current_process;
    u64 flags;

    if (!proc)
        return (i64)NnotInitialized;
    if (a[0] != NX_PID_SELF) {
        Nstatus status = lookup_process(a[0], (u32)NX_RIGHT_KILL, &proc);

        if (NSTATUS_IS_ERR(status))
            return (i64)status;
    }
    if (a[1] == NX_PGID_QUERY)
        return (i64)proc->pgid;
    if (a[1] >= NX_PGID_QUERY)
        return (i64)NinvalidArg;

    flags = irq_save();
    proc->pgid = a[1] ? (u32)a[1] : proc->pid;
    irq_restore(flags);
    return (i64)proc->pgid;
}

/* NxProcessWait(handle, timeout_ms, i32 *out_code) */
static i64 sys_process_wait(const u64 *a)
{
    process_t *proc;
    u64 timeout_ms = a[1];
    u64 ticks = 0;
    i32 code = 0;
    Nstatus status = lookup_process(a[0], (u32)NX_RIGHT_WAIT, &proc);

    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    if (timeout_ms == 0) {
        /* 기다리지 않고 확인만 */
        if (proc->state != PROCESS_TERMINATED)
            return (i64)Ntimeout;
        code = proc->exit_code;
    } else {
        if (timeout_ms != NX_WAIT_INFINITE) {
            u32 hz = timer_get_hz();

            if (timeout_ms > NX_WAIT_MAX_MS)
                return (i64)NinvalidArg;
            if (hz == 0)
                return (i64)Nunsupported;
            ticks = (timeout_ms * (u64)hz + 999UL) / 1000UL;     /* 올림, 최소 1 tick */
        }

        status = process_wait(proc, ticks, &code);   /* 핸들이 참조를 가지므로 기다리는 동안 proc 는 유효 */
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
    }

    if (a[2] != 0) {
        status = copy_to_user(a[2], &code, sizeof(code));
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
    }
    return 0;
}

/* NxVirtualAlloc(increment) -> 새 brk (brk 방식 힙 늘리기/줄이기, 자세한 규약은 nyx_abi.h) */
static i64 sys_virtual_alloc(const u64 *a)
{
    u64 brk = 0;
    Nstatus status;

    if (!current_process)
        return (i64)NnotInitialized;

    status = umem_brk(current_process, (i64)a[0], &brk);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    return (i64)brk;
}

/* NxProcessExit(exit_code) - 반환하지 않는다 */
static i64 sys_process_exit(const u64 *a)
{
    process_exit_with_code((i32)(a[0] & 0xFFFFFFFFUL));
}

/* NxProcessInfo(NX_PID_SELF 또는 프로세스 핸들(NX_RIGHT_STAT), struct nx_procinfo *out) */
static i64 sys_process_info(const u64 *a)
{
    struct nx_procinfo pi;
    process_t *p = current_process;

    if (!p)
        return (i64)NnotInitialized;

    if (a[0] != NX_PID_SELF) {
        Nstatus status = lookup_process(a[0], (u32)NX_RIGHT_STAT, &p);

        if (NSTATUS_IS_ERR(status))
            return (i64)status;
    }

    memset(&pi, 0, sizeof(pi));
    pi.pid = p->pid;
    pi.state = p->state;
    pi.handle_count = nx_handles_count(&p->handles);
    pi.parent_pid = p->parent_pid;

    return (i64)copy_to_user(a[1], &pi, sizeof(pi));
}

/* NxProcessList(struct nx_procent *out, max) -> 살아 있는 프로세스 수 */
static i64 sys_process_list(const u64 *a)
{
    struct nx_procent *buf;
    u32 max = a[1] > PROCESS_MAX ? PROCESS_MAX : (u32)a[1];
    u32 n;
    Nstatus status = NSTATUS_OK;

    if (PROCESS_MAX * sizeof(struct nx_procent) > PAGE_SIZE)
        return (i64)Noverflow;
    buf = (struct nx_procent *)kpage_alloc();
    if (!buf)
        return (i64)NoutOfMemory;
    n = process_list_snapshot(buf, max);
    if (max > 0)
        status = copy_to_user(a[0], buf, (usize)(n < max ? n : max) * sizeof(struct nx_procent));
    kpage_free(buf);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    return (i64)n;
}

/* ------------------------------------------------------------------ */
/* 신원 / 인증 / 전원                                                   */
/* ------------------------------------------------------------------ */

/*
 * NxGetIdentity(struct nx_identity *out, token)
 *   token == NX_NO_TOKEN: 호출한 프로세스의 신원.
 *   그 외: 토큰 핸들(NX_RIGHT_STAT)이 담은 신원을 조회만 한다 (토큰은 소모되지 않는다).
 *   login 이 셸을 띄우기 전에 그 사용자의 uid 로 roi:/<uid>/shell 을 읽으려고 쓴다.
 */
static i64 sys_get_identity(const u64 *a)
{
    struct nx_identity id;
    process_t *self = current_process;

    if (!self)
        return (i64)NnotInitialized;
    memset(&id, 0, sizeof(id));
    if (a[1] != NX_NO_TOKEN) {
        nx_handle_table_t *table = cur_handles();
        nx_handle_t *slot;
        Nstatus status = nx_handle_lookup(table, a[1], 0, &slot);

        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        if (slot->type != NX_HANDLE_TOKEN || !slot->token)
            return (i64)NinvalidArg;
        if (!(slot->rights & (u32)NX_RIGHT_STAT))
            return (i64)Npermission;
        id.uid = slot->token->uid;
        id.gid = slot->token->gid;
        memcpy(id.name, slot->token->name,
               sizeof(slot->token->name) < sizeof(id.name) ? sizeof(slot->token->name) : sizeof(id.name));
    } else {
        id.uid = self->uid;
        id.gid = self->gid;
        memcpy(id.name, self->user_name,
               sizeof(self->user_name) < sizeof(id.name) ? sizeof(self->user_name) : sizeof(id.name));
    }
    id.name[sizeof(id.name) - 1] = '\0';
    return (i64)copy_to_user(a[0], &id, sizeof(id));
}

/* NxPowerOff() - uid 0 만. 정책(누가 끄게 할지)은 init 이 정한다 */
static i64 sys_power_off(const u64 *a)
{
    (void)a;
    if (!current_process || current_process->uid != 0)
        return (i64)Npermission;
    /* 디스크의 쓰기 캐시를 매체로 (disk:/ 의 내용을 잃지 않도록) */
    if (NSTATUS_IS_ERR(ahci_flush_all()))
        printk_warn("power off: disk cache flush failed\n");
    printk("\nSystem power off.\n");
    return (i64)acpi_power_off();
}

/* NxAuthenticate(const char *name, const char *password) -> 토큰 핸들 */
static i64 sys_authenticate(const u64 *a)
{
    char name[NX_USER_NAME_MAX];
    char password[AUTH_PASSWORD_MAX + 1U];
    nx_handle_table_t *table = cur_handles();
    auth_identity_t id;
    auth_identity_t *tok;
    nx_handle_t *slot;
    u64 handle;
    u64 flags;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    status = copy_string_from_user(name, a[0], sizeof(name));
    if (NSTATUS_IS_ERR(status))
        return (i64)(status == NpathTooLong ? Nauthentication : status);
    status = copy_string_from_user(password, a[1], sizeof(password));
    if (NSTATUS_IS_ERR(status)) {
        crypto_wipe(password, sizeof(password));
        return (i64)(status == NpathTooLong ? NinvalidArg : status);
    }

    /* PBKDF2 는 수십 ms 걸린다: 그동안 인터럽트(타이머/키보드)를 막지 않는다. 커널 코드는 비선점이고
     * 이 계산은 부팅 후 바뀌지 않는 사용자 표만 읽으므로 IRQ 가 끼어들어도 안전하다. */
    flags = irq_save();
    sti();
    status = auth_verify(name, password, &id);
    irq_restore(flags);
    crypto_wipe(password, sizeof(password));

    if (NSTATUS_IS_ERR(status)) {
        u32 hz = timer_get_hz();

        if (status == Nauthentication && hz) {
            /* 신호로 줄일 수 없다 (자기에게 신호를 보내 무차별 대입의 지연을 건너뛰지 못하게) */
            current_process->sig_hold = true;
            (void)process_sleep_ticks(((u64)NX_AUTH_FAIL_DELAY_MS * hz + 999UL) / 1000UL);
            current_process->sig_hold = false;
        }
        return (i64)status;
    }

    tok = (auth_identity_t *)kmalloc(sizeof(*tok));
    if (!tok) {
        crypto_wipe(&id, sizeof(id));
        return (i64)NoutOfMemory;
    }
    *tok = id;
    crypto_wipe(&id, sizeof(id));

    status = nx_handle_alloc(table, NX_HANDLE_TOKEN, (u32)(NX_RIGHT_SPAWN | NX_RIGHT_STAT), &handle, &slot);
    if (NSTATUS_IS_ERR(status)) {
        crypto_wipe(tok, sizeof(*tok));
        kfree(tok);
        return (i64)status;
    }
    slot->token = tok;
    return (i64)handle;
}

/* ------------------------------------------------------------------ */
/* NxPoll / 채널                                                        */
/* ------------------------------------------------------------------ */

/* timeout_ms -> tick (0 = 확인만, NX_WAIT_INFINITE = 무기한). 잘못된 값이면 NinvalidArg */
static Nstatus timeout_to_ticks(u64 ms, bool *infinite, u64 *ticks)
{
    u32 hz = timer_get_hz();

    *infinite = false;
    *ticks = 0;
    if (ms == NX_WAIT_INFINITE) {
        *infinite = true;
        return NSTATUS_OK;
    }
    if (ms == 0)
        return NSTATUS_OK;
    if (ms > NX_WAIT_MAX_MS)
        return NinvalidArg;
    if (hz == 0)
        return Nunsupported;
    *ticks = (ms * (u64)hz + 999UL) / 1000UL;
    return NSTATUS_OK;
}

/* NxPoll(struct nx_pollfd *fds, n, timeout_ms) -> 준비된 항목 수 */
static i64 sys_poll(const u64 *a)
{
    struct nx_pollfd fds[NX_POLL_MAX];
    nx_handle_table_t *table = cur_handles();
    u64 n = a[1];
    u64 deadline = 0;
    u64 ticks;
    bool infinite;
    u64 flags;
    u32 ready = 0;
    u32 i;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if (n == 0 || n > NX_POLL_MAX)
        return (i64)NinvalidArg;
    status = timeout_to_ticks(a[2], &infinite, &ticks);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = copy_from_user(fds, a[0], (usize)(n * sizeof(fds[0])));
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (ticks)
        deadline = timer_get_tick() + ticks;

    flags = irq_save();
    for (;;) {
        waitq_t *qs[NX_POLL_MAX];
        u32 nq = 0;
        u64 now;

        ready = 0;
        for (i = 0; i < n; i++) {
            nx_handle_t *slot;
            u32 bits = 0;

            fds[i].revents = 0;
            status = nx_handle_lookup(table, fds[i].handle, 0, &slot);
            if (NSTATUS_IS_ERR(status)) {
                irq_restore(flags);
                return (i64)status;
            }

            if (slot->type == NX_HANDLE_PROCESS) {
                if (slot->proc->state == PROCESS_TERMINATED)
                    bits = NX_POLL_EXIT;
                qs[nq++] = &slot->proc->exit_waiters;
            } else if (slot->type == NX_HANDLE_CHANNEL) {
                bits = channel_poll_bits(slot->chan, slot->chan_receiver);
                qs[nq++] = slot->chan_receiver ? &slot->chan->recv_q : &slot->chan->send_q;
            } else if (slot->type == NX_HANDLE_PIPE) {
                bits = pipe_poll_bits(slot->pipe, slot->pipe_write);
                qs[nq++] = pipe_waitq(slot->pipe, slot->pipe_write);
            } else if (slot->type == NX_HANDLE_CONSOLE && slot->console_fd == 0) {
                tty_t *t = slot_tty(slot);

                if (tty_input_ready(t))
                    bits = NX_POLL_IN;
                if (tty_hung_up(t))
                    bits |= NX_POLL_HUP;
                qs[nq++] = tty_waitq(t);
            } else if (slot->type == NX_HANDLE_PTY) {
                bits = pty_master_poll_bits(slot->pty);
                qs[nq++] = pty_master_waitq(slot->pty);
            } else if (slot->type == NX_HANDLE_DEVICE && slot->dev->kind == NX_DEV_CONSOLE) {
                if (keyboard_events_ready())
                    bits = NX_POLL_IN;
                qs[nq++] = keyboard_waitq();
            } else {
                irq_restore(flags);
                return (i64)Nunsupported;   /* 기다릴 수 없는 종류 */
            }

            fds[i].revents = bits & (fds[i].events | NX_POLL_HUP);
            if (fds[i].revents)
                ready++;
        }

        now = timer_get_tick();
        if (ready || (!infinite && (ticks == 0 || now >= deadline)))
            break;

        status = waitq_sleep_multi_locked(qs, nq, infinite ? 0 : deadline - now);
        if (NSTATUS_IS_ERR(status) && status != Ntimeout) {
            irq_restore(flags);
            return (i64)status;
        }
    }
    irq_restore(flags);

    status = copy_to_user(a[0], fds, (usize)(n * sizeof(fds[0])));
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    return (i64)ready;
}

/* NxChannelCreate(const char *path, flags) -> 수신 핸들 */
static i64 sys_channel_create(const u64 *a)
{
    char kpath[NSP_PATH_MAX];
    nx_handle_table_t *table = cur_handles();
    channel_t *c = nNULL;
    nx_handle_t *slot;
    u64 handle;
    u32 kind = 0;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    status = user_path(a[0], kpath, sizeof(kpath), &kind);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (kind != NSP_KIND_SVC)
        return (i64)NinvalidArg;

    status = nx_handle_alloc(table, NX_HANDLE_CHANNEL, (u32)(NX_RIGHT_RECV | NX_RIGHT_STAT), &handle, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = channel_create(kpath, (u32)a[1], &c);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(slot);
        return (i64)status;
    }
    slot->chan = c;
    slot->chan_receiver = true;
    return (i64)handle;
}

static Nstatus lookup_channel(u64 handle, u32 rights, nx_handle_t **out)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    Nstatus status;

    if (!table)
        return NnotInitialized;
    status = nx_handle_lookup(table, handle, 0, &slot);
    if (NSTATUS_IS_ERR(status))
        return status;
    if (slot->type != NX_HANDLE_CHANNEL || !slot->chan)
        return NinvalidArg;
    if ((slot->rights & rights) != rights)
        return Npermission;
    *out = slot;
    return NSTATUS_OK;
}

/* NxChannelSend(h, const void *buf, len) */
static i64 sys_channel_send(const u64 *a)
{
    u8 buf[NX_CHAN_MSG_MAX];
    nx_handle_t *slot;
    Nstatus status = lookup_channel(a[0], (u32)NX_RIGHT_SEND, &slot);

    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (a[2] > NX_CHAN_MSG_MAX)
        return (i64)NinvalidArg;
    status = copy_from_user(buf, a[1], (usize)a[2]);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    return (i64)channel_send(slot->chan, buf, (usize)a[2]);
}

/*
 * NxChannelReceive / NxChannelReceiveEx 공통. ex 이면 붙은 핸들을 받는 쪽 테이블에 넣고 info 는 nx_msginfo_ex,
 * 아니면 붙은 핸들은 닫는다 (인터럽트를 켠 뒤에: 파일이면 디스크를 건드릴 수 있다).
 */
static i64 do_channel_receive(const u64 *a, bool ex)
{
    u8 buf[NX_CHAN_MSG_MAX];
    struct nx_msginfo info;
    struct nx_msginfo_ex info_ex;
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    nx_handle_t *dst = nNULL;
    nx_handle_t got_h;
    bool got = false;
    u64 dst_handle = NX_HANDLE_INVALID_VALUE;
    usize len = (usize)(a[2] < NX_CHAN_MSG_MAX ? a[2] : NX_CHAN_MSG_MAX);
    u64 ticks;
    u64 deadline;
    bool infinite;
    u64 flags;
    Nstatus status = lookup_channel(a[0], (u32)NX_RIGHT_RECV, &slot);

    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = timeout_to_ticks(a[4], &infinite, &ticks);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    deadline = timer_get_tick() + ticks;
    memset(&info, 0, sizeof(info));
    memset(&got_h, 0, sizeof(got_h));

    flags = irq_save();
    for (;;) {
        u64 now;

        /* 핸들이 붙은 메시지면 받을 자리를 먼저 잡는다 (없으면 메시지는 큐에 남는다) */
        if (ex && !dst && channel_head_has_handle_locked(slot->chan)) {
            /* 자리표: 빈 info 디렉터리 핸들 (잠든 동안 끝나도 닫기에 문제가 없다) */
            status = nx_handle_alloc(table, NX_HANDLE_INFO, 0, &dst_handle, &dst);
            if (NSTATUS_IS_ERR(status))
                break;
            dst->info_dir = true;
        }
        status = channel_take_handle_locked(slot->chan, buf, len, &info, &got_h, &got);
        if (status != Ntimeout)
            break;                          /* 받았거나 버퍼가 작다 */
        now = timer_get_tick();
        if (!infinite && (ticks == 0 || now >= deadline))
            break;                          /* Ntimeout */
        status = waitq_sleep_locked(&slot->chan->recv_q, infinite ? 0 : deadline - now);
        if (NSTATUS_IS_ERR(status) && status != Ntimeout)
            break;
    }
    if (got && dst) {
        u32 gen = dst->generation;

        *dst = got_h;                       /* 객체 참조를 그대로 옮긴다 */
        dst->generation = gen;
        dst->hflags = 0;
        got = false;
    } else if (dst) {
        nx_handle_discard(dst);             /* 자리를 잡았지만 핸들이 오지 않았다 */
        dst = nNULL;
        dst_handle = NX_HANDLE_INVALID_VALUE;
    }
    irq_restore(flags);
    if (got)
        nx_handle_release(&got_h, nNULL);   /* NxChannelReceive: 받을 수 없으니 닫는다 */
    if (!dst)
        dst_handle = NX_HANDLE_INVALID_VALUE;

    if (a[3] != 0) {
        Nstatus st2;

        if (ex) {
            info_ex.sender_pid = info.sender_pid;
            info_ex.sender_uid = info.sender_uid;
            info_ex.sender_gid = info.sender_gid;
            info_ex.length = info.length;
            info_ex.handle = NSTATUS_IS_ERR(status) ? NX_HANDLE_INVALID_VALUE : dst_handle;
            st2 = copy_to_user(a[3], &info_ex, sizeof(info_ex));
        } else {
            st2 = copy_to_user(a[3], &info, sizeof(info));
        }
        if (NSTATUS_IS_ERR(st2))
            return (i64)st2;                /* 받은 핸들은 테이블에 남는다 (프로세스가 끝나면 닫힌다) */
    }
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = copy_to_user(a[1], buf, info.length);   /* 이미 큐에서 꺼냈다: 실패하면 그 메시지는 버려진다 */
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    return (i64)info.length;
}

/* NxChannelReceive(h, void *buf, len, struct nx_msginfo *info, timeout_ms) -> 메시지 바이트 수 */
static i64 sys_channel_receive(const u64 *a)
{
    return do_channel_receive(a, false);
}

/* NxChannelReceiveEx(h, void *buf, len, struct nx_msginfo_ex *info, timeout_ms) */
static i64 sys_channel_receive_ex(const u64 *a)
{
    return do_channel_receive(a, true);
}

/* NxChannelSendHandle(h, buf, len, handle) */
static i64 sys_channel_send_handle(const u64 *a)
{
    u8 buf[NX_CHAN_MSG_MAX];
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    nx_handle_t *src;
    nx_handle_t copy;
    Nstatus status = lookup_channel(a[0], (u32)NX_RIGHT_SEND, &slot);

    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (a[2] > NX_CHAN_MSG_MAX)
        return (i64)NinvalidArg;
    status = copy_from_user(buf, a[1], (usize)a[2]);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = nx_handle_lookup(table, a[3], 0, &src);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (src->type == NX_HANDLE_PROCESS || src->type == NX_HANDLE_TOKEN || src->type == NX_HANDLE_DEVICE ||
        (src->type == NX_HANDLE_CHANNEL && src->chan_receiver))
        return (i64)Nunsupported;           /* 보낼 수 없는 종류 */
    if (!(src->rights & (u32)NX_RIGHT_DUP))
        return (i64)Npermission;

    memset(&copy, 0, sizeof(copy));
    status = nx_handle_share(src, &copy, true);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    copy.type = src->type;
    copy.rights = src->rights;
    status = channel_send_handle(slot->chan, buf, (usize)a[2], &copy);
    if (NSTATUS_IS_ERR(status))
        nx_handle_release(&copy, nNULL);
    return (i64)status;
}

/* ------------------------------------------------------------------ */
/* 공유 메모리                                                           */
/* ------------------------------------------------------------------ */

/* NxSharedMemoryCreate(size, flags) -> 핸들 */
static i64 sys_shm_create(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    nx_shm_t *shm;
    u64 handle;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if (a[1] & ~NX_DUP_CLOEXEC)
        return (i64)NinvalidArg;
    status = nx_handle_alloc(table, NX_HANDLE_SHM, (u32)(NX_RIGHT_READ | NX_RIGHT_WRITE | NX_RIGHT_STAT | NX_RIGHT_DUP),
                             &handle, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = shm_create(a[0], &shm);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(slot);
        return (i64)status;
    }
    slot->shm = shm;
    slot->hflags = (a[1] & NX_DUP_CLOEXEC) ? (u32)NX_HFLAG_CLOEXEC : 0U;
    return (i64)handle;
}

/* NxSharedMemoryMap(h, u64 *out_addr) -> 0 */
static i64 sys_shm_map(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    u64 va;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    status = nx_handle_lookup(table, a[0], (u32)NX_RIGHT_READ, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (slot->type != NX_HANDLE_SHM || !slot->shm)
        return (i64)NinvalidArg;
    if (!slot->shm_va) {
        va = NX_USER_SHM_BASE + (a[0] & 0xFFFFFFFFUL) * NX_SHM_SLOT_SIZE;   /* 슬롯마다 자리가 정해져 있다 */
        status = shm_map(slot->shm, current_process, va, (slot->rights & (u32)NX_RIGHT_WRITE) ? true : false);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        slot->shm_va = va;
    }
    return (i64)copy_to_user(a[1], &slot->shm_va, sizeof(slot->shm_va));
}

/* ------------------------------------------------------------------ */
/* 파일시스템 노드                                                      */
/* ------------------------------------------------------------------ */

/* NxCreateNode(path, type, mode) */
static i64 sys_create_node(const u64 *a)
{
    char kpath[NSP_PATH_MAX];
    Nstatus status;

    if (a[2] & ~0777UL)
        return (i64)NinvalidArg;            /* set-uid 류 비트는 허용하지 않는다 */
    status = user_vfs_path(a[0], kpath, sizeof(kpath), NSP_OP_MODIFY);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    /* 보안 계층: 호출자의 umask 를 적용한다 */
    if (a[1] == NX_NODE_FILE)
        return (i64)vfs_create(kpath, perm_apply_umask((u32)a[2]));
    if (a[1] == NX_NODE_DIR)
        return (i64)vfs_mkdir(kpath, perm_apply_umask((u32)a[2]));
    return (i64)NinvalidArg;
}

/* NxRemoveNode(path) */
static i64 sys_remove_node(const u64 *a)
{
    char kpath[NSP_PATH_MAX];
    Nstatus status = user_vfs_path(a[0], kpath, sizeof(kpath), NSP_OP_MODIFY);

    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    return (i64)vfs_unlink(kpath);
}

/* NxRenameNode(old_path, new_path) */
static i64 sys_rename_node(const u64 *a)
{
    char from[NSP_PATH_MAX];
    char to[NSP_PATH_MAX];
    Nstatus status = user_vfs_path(a[0], from, sizeof(from), NSP_OP_MODIFY);

    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = user_vfs_path(a[1], to, sizeof(to), NSP_OP_MODIFY);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    return (i64)vfs_rename(from, to);
}

/* NxReadDir(path, index, struct nx_dirent *out) */
static i64 sys_read_dir(const u64 *a)
{
    char kpath[NSP_PATH_MAX];
    struct nx_dirent de;
    u32 type = 0;
    u32 kind = 0;
    Nstatus status = user_path(a[0], kpath, sizeof(kpath), &kind);

    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    memset(&de, 0, sizeof(de));
    if (kind == NSP_KIND_INFO) {
        status = info_readdir(kpath, a[1], de.name, sizeof(de.name), &type);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        de.type = (type == INFO_NODE_DIR) ? NX_TYPE_DIR : NX_TYPE_FILE;
        return (i64)copy_to_user(a[2], &de, sizeof(de));
    }
    if (kind == NSP_KIND_DEV) {
        /* dev:/ 목록: 누구나 열 수 있는 표준 스트림 장치, 그다음 (root 에게만) 등록된 장치 */
        static const char *const pub[] = { "stdin", "stdout", "stderr", "tty" };
        const char *name = nNULL;
        u64 i = a[1];

        if (kpath[0] != '\0')
            return (i64)Nunsupported;           /* dev:/ 아래에는 디렉터리가 없다 */
        if (i < 4U)
            name = pub[i];
        else if (current_process && current_process->uid == 0 && i - 4U < 0xFFFFFFFFUL)
            name = device_name_at((u32)(i - 4U));
        if (!name)
            return (i64)NnotFound;
        memcpy(de.name, name, strlen(name) + 1U);
        de.type = NX_TYPE_FILE;
        return (i64)copy_to_user(a[2], &de, sizeof(de));
    }
    if (kind != NSP_KIND_VFS)
        return (i64)Nunsupported;
    /* OPEN 규칙: 커널 전용 경로 자체만 막는다 (조상 디렉터리의 목록은 볼 수 있다 - 이름만 보인다) */
    status = nsp_check_protected(kpath, NSP_OP_OPEN);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    status = vfs_readdir(kpath, a[1], de.name, sizeof(de.name), &type);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    de.type = (type == 2U) ? NX_TYPE_DIR : NX_TYPE_FILE;
    return (i64)copy_to_user(a[2], &de, sizeof(de));
}

/* ------------------------------------------------------------------ */
/* 장치                                                                 */
/* ------------------------------------------------------------------ */

/* NxDeviceCommand(h, cmd, arg) */
static i64 sys_device_command(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    status = nx_handle_lookup(table, a[0], 0, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    if (slot->type == NX_HANDLE_PTY) {
        /* pty 주 끝: 창 크기, 포그라운드 그룹 조회 (터미널 에뮬레이터가 단축키로 신호를 보낼 때) */
        struct nx_winsize ws;

        if (a[1] == NX_DEVCMD_TTY_FOREGROUND) {
            if (a[2] != NX_PGID_QUERY)
                return (i64)Nunsupported;   /* 정하는 것은 종속 끝(작업을 띄우는 셸)이 한다 */
            return (i64)tty_get_foreground(pty_tty(slot->pty));
        }

        if (a[1] == NX_DEVCMD_TTY_WINSIZE) {
            tty_get_winsize(pty_tty(slot->pty), &ws);
            return (i64)copy_to_user(a[2], &ws, sizeof(ws));
        }
        if (a[1] == NX_DEVCMD_TTY_SET_WINSIZE) {
            status = copy_from_user(&ws, a[2], sizeof(ws));
            if (NSTATUS_IS_ERR(status))
                return (i64)status;
            pty_set_winsize(slot->pty, &ws);
            return 0;
        }
        return (i64)Nunsupported;
    }

    if (slot->type == NX_HANDLE_CONSOLE) {
        /* 콘솔(또는 pty 종속 끝): 에코(stdin 만), 입력 모드, 창 크기 */
        tty_t *t = slot_tty(slot);

        if (a[1] == NX_DEVCMD_TTY_ECHO) {
            if (slot->console_fd != 0)
                return (i64)Nunsupported;
            tty_set_echo(t, a[2] ? true : false);
            return 0;
        }
        if (a[1] == NX_DEVCMD_TTY_MODE) {
            if (a[2] != NX_TTY_MODE_COOKED && a[2] != NX_TTY_MODE_RAW)
                return (i64)NinvalidArg;
            tty_set_raw(t, a[2] == NX_TTY_MODE_RAW ? true : false, current_process->pid);
            return 0;
        }
        if (a[1] == NX_DEVCMD_TTY_SET_WINSIZE) {
            struct nx_winsize ws;

            if (!t->pty)
                return (i64)Nunsupported;   /* 물리 콘솔의 크기는 화면이 정한다 */
            status = copy_from_user(&ws, a[2], sizeof(ws));
            if (NSTATUS_IS_ERR(status))
                return (i64)status;
            pty_set_winsize(t->pty, &ws);
            return 0;
        }
        if (a[1] == NX_DEVCMD_TTY_FOREGROUND) {
            u32 pgid;

            if (a[2] == NX_PGID_QUERY)
                return (i64)tty_get_foreground(t);
            if (a[2] > 0xFFFFFFFFUL)
                return (i64)NinvalidArg;
            pgid = (u32)a[2];
            /* 다른 사용자의 프로세스가 든 그룹을 Ctrl-C 의 대상으로 만들 수 없다 (root 는 예외) */
            if (pgid != 0 && current_process->uid != 0 &&
                !process_group_owned_by(pgid, current_process->uid))
                return (i64)Npermission;
            tty_set_foreground(t, pgid);
            return 0;
        }
        if (a[1] == NX_DEVCMD_TTY_WINSIZE) {
            struct nx_winsize ws;

            tty_get_winsize(t, &ws);
            return (i64)copy_to_user(a[2], &ws, sizeof(ws));
        }
        return (i64)Nunsupported;
    }

    if (slot->type != NX_HANDLE_DEVICE)
        return (i64)NinvalidArg;
    if (!(slot->rights & (u32)NX_RIGHT_DEVICE))
        return (i64)Npermission;

    if (a[1] == NX_DEVCMD_INFO) {
        struct nx_devinfo info;

        device_info(slot->dev, &info);
        return (i64)copy_to_user(a[2], &info, sizeof(info));
    }
    if (a[1] == NX_DEVCMD_IOPERM)
        return (i64)device_ioperm(slot->dev, current_process, a[2] ? true : false);
    if (a[1] == NX_DEVCMD_CONSOLE_FONT)
        return (i64)device_console_font(slot->dev, current_process, a[2]);
    return (i64)Nunsupported;
}

/* NxDeviceMapMMIO(h, u64 *out_addr) */
static i64 sys_device_map_mmio(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    u64 va = 0;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    status = nx_handle_lookup(table, a[0], (u32)NX_RIGHT_DEVICE, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    if (slot->type != NX_HANDLE_DEVICE)
        return (i64)NinvalidArg;
    status = device_map_mmio(slot->dev, current_process, &va);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    return (i64)copy_to_user(a[1], &va, sizeof(va));
}

/* ------------------------------------------------------------------ */
/* Debug                                                               */
/* ------------------------------------------------------------------ */

/*
 * 디버그 시스템 콜 (0x0300 구역, NX_SYS_DEBUG_FIRST..NX_SYS_DEBUG_LAST). make DEBUG_SYSCALLS=1 (또는 SELFTEST=1)
 * 일 때만 커널에 들어간다. 기본 빌드에는 코드가 없고 그 번호는 NsyscallFailed 다 (syscall_dispatch 도 막는다).
 */
#ifdef NYX_DEBUG_SYSCALLS
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
#endif /* NYX_DEBUG_SYSCALLS */

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
    { NX_SYS_GET_IDENTITY,     sys_get_identity },
    { NX_SYS_POWER_OFF,        sys_power_off },
    { NX_SYS_AUTHENTICATE,     sys_authenticate },

    { NX_SYS_OPEN,             sys_open },
    { NX_SYS_CLOSE,            sys_close },
    { NX_SYS_READ,             sys_read },
    { NX_SYS_WRITE,            sys_write },
    { NX_SYS_SEEK,             sys_seek },
    { NX_SYS_STAT,             sys_stat },
    { NX_SYS_DUPLICATE_HANDLE, sys_duplicate_handle },
    { NX_SYS_POLL,             sys_poll },
    { NX_SYS_DUPLICATE_HANDLE_EX, sys_duplicate_handle_ex },
    { NX_SYS_HANDLE_FLAGS,     sys_handle_flags },
    { NX_SYS_PIPE_CREATE,      sys_pipe_create },
    { NX_SYS_PTY_CREATE,       sys_pty_create },
    { NX_SYS_STAT_PATH,        sys_stat_path },
    { NX_SYS_SET_MODE,         sys_set_mode },
    { NX_SYS_SET_UMASK,        sys_set_umask },

    { NX_SYS_CHANNEL_CREATE,   sys_channel_create },
    { NX_SYS_CHANNEL_SEND,     sys_channel_send },
    { NX_SYS_CHANNEL_RECEIVE,  sys_channel_receive },
    { NX_SYS_CHANNEL_SEND_HANDLE, sys_channel_send_handle },
    { NX_SYS_CHANNEL_RECEIVE_EX,  sys_channel_receive_ex },
    { NX_SYS_SHM_CREATE,       sys_shm_create },
    { NX_SYS_SHM_MAP,          sys_shm_map },

    { NX_SYS_CREATE_NODE,      sys_create_node },
    { NX_SYS_REMOVE_NODE,      sys_remove_node },
    { NX_SYS_RENAME_NODE,      sys_rename_node },
    { NX_SYS_READ_DIR,         sys_read_dir },

    { NX_SYS_DEVICE_COMMAND,   sys_device_command },
    { NX_SYS_DEVICE_MAP_MMIO,  sys_device_map_mmio },

    { NX_SYS_PROCESS_CREATE,   sys_process_create },
    { NX_SYS_PROCESS_EXIT,     sys_process_exit },
    { NX_SYS_PROCESS_FORK,     sys_process_fork },
    { NX_SYS_PROCESS_EXEC,     sys_process_exec },
    { NX_SYS_PROCESS_KILL,     sys_process_kill },
    { NX_SYS_PROCESS_GROUP,    sys_process_group },
    { NX_SYS_SIGNAL_ACTION,    sys_signal_action },
    { NX_SYS_SIGNAL_MASK,      sys_signal_mask },
    { NX_SYS_SIGNAL_SEND,      sys_signal_send },
    { NX_SYS_SIGNAL_SEND_PID,  sys_signal_send_pid },
    { NX_SYS_PROCESS_LIST,     sys_process_list },
    { NX_SYS_SIGNAL_RETURN,    sys_signal_return },
    { NX_SYS_SIGNAL_SUSPEND,   sys_signal_suspend },
    { NX_SYS_SIGNAL_PENDING,   sys_signal_pending },
    { NX_SYS_PROCESS_WAIT,     sys_process_wait },
    { NX_SYS_PROCESS_INFO,     sys_process_info },

    { NX_SYS_VIRTUAL_ALLOC,    sys_virtual_alloc }

#ifdef NYX_DEBUG_SYSCALLS
    ,
    { NX_SYS_KERNEL_PRINT,     sys_kernel_print },
    { NX_SYS_DEBUG_NOP,        sys_debug_nop }
#endif
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

#ifndef NYX_DEBUG_SYSCALLS
    /* 디버그 구역은 표에 무엇이 있든 막는다 (나중에 #ifdef 밖에 디버그 콜을 실수로 넣어도 열리지 않게) */
    if (syscall_nr >= NX_SYS_DEBUG_FIRST && syscall_nr <= NX_SYS_DEBUG_LAST)
        return (i64)NsyscallFailed;
#endif

    for (i = 0; i < SYSCALL_COUNT; i++) {
        if (g_syscalls[i].nr == syscall_nr)
            return g_syscalls[i].fn(args);
    }

    return (i64)NsyscallFailed;      /* 알 수 없는 번호: 커널은 영향받지 않는다 */
}

u64 syscall_handle(struct trap_frame *frame)
{
    i64 result;
    u64 use_iret = 0;

    if (current_process)
        current_process->uframe = frame;    /* fork/exec 가 사용자 레지스터를 읽고 바꾼다 (kernel/exec/exec.h) */
    result = syscall_dispatch(
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

    /* 사용자 모드로 돌아가기 직전: 신호 (kernel/process/signal.h) */
    if (current_process) {
        signal_deliver(frame);
        if (current_process->sig_iret) {
            current_process->sig_iret = false;
            use_iret = 1;
        }
    }
    return use_iret;
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
    process_exit_with_code((i32)NX_EXIT_FAULT);
}
