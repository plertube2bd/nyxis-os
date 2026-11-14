/*
 * nl_internal.h - nlibc 내부 인터페이스 (설치되는 헤더가 아니다)
 *
 * 계층:
 *   표준 함수 (string.c, stdio.c, ...)
 *     -> 플랫폼 계층 __nl_* (아래 선언, nxsys.c 가 Nyxis 시스템 콜로 구현)
 *
 * 플랫폼 계층만 바꾸면 같은 libc 를 다른 커널 위에서도 돌릴 수 있다 — 호스트 테스트
 * (tests/host/nlibc/sys_linux.c)가 이 경계를 Linux 시스템 콜로 바꿔 끼운다.
 *
 * 규약: 실패하면 errno 를 설정하고 -1 (포인터면 NULL) 을 돌려준다. 핸들은 long.
 */
#ifndef NL_INTERNAL_H
#define NL_INTERNAL_H

#include <stddef.h>
#include <stdio.h>

/* ---------------- 플랫폼 계층 ---------------- */
#define NL_FD_MAX   64      /* fd 번호 상한 (Nyxis: NX_MAX_HANDLES) */
#define NL_O_READ   0x01
#define NL_O_WRITE  0x02
#define NL_O_CREAT  0x04    /* 없으면 만든다 */
#define NL_O_TRUNC  0x08    /* 길이를 0 으로 (커널에 자르기가 없으면 지우고 다시 만든다) */
#define NL_O_EXCL   0x10    /* NL_O_CREAT 와 함께: 이미 있으면 EEXIST */
#define NL_O_APPEND 0x20    /* 쓰기마다 끝으로 (커널에 없으면 흉내 낸다) */

/* path 는 이미 절대 경로(__nl_resolve 를 거친 것)다. mode 는 새로 만들 때의 권한 */
long __nl_open(const char *path, int flags, unsigned int mode);
/* 열린 핸들의 열기 플래그 (NL_O_*). F_GETFL/F_SETFL 용. 모르면 -1 + EBADF */
int __nl_handle_flags(long h);
int __nl_handle_set_append(long h, int on);
int __nl_close(long h);
long __nl_read(long h, void *buf, size_t n);
long __nl_write(long h, const void *buf, size_t n);
long __nl_seek(long h, long offset, int whence);   /* whence: SEEK_SET/CUR/END -> 새 위치 */
int __nl_is_console(long h);                       /* 대화형 장치면 1 (줄 단위 버퍼링을 고르는 데 쓴다) */
/* 터미널 (termios.c). 실패하면 -1 + errno */
int __nl_tty_mode(long h, int raw);                /* 1 = raw (줄 편집/에코 없음), 0 = 줄 단위 */
int __nl_tty_echo(long h, int on);                 /* 줄 단위 모드의 에코 */
int __nl_tty_winsize(long h, unsigned short ws[4]); /* 행, 열, x 픽셀, y 픽셀 */
int __nl_wait_input(long h, long timeout_ms);      /* 읽을 입력이 있으면 1, 시간 초과 0 */
/* 여러 핸들을 함께 기다린다 (poll.c). events/revents 는 NL_POLL_*. timeout_ms 음수 = 무기한.
 * 준비된 항목 수, 시간 초과 0, 오류 -1 + errno (신호면 EINTR) */
#define NL_POLL_IN  1
#define NL_POLL_OUT 2
#define NL_POLL_HUP 4
struct __nl_pollent {
    long h;
    int events;
    int revents;
};
int __nl_poll(struct __nl_pollent *p, int n, long timeout_ms);
extern long __nl_tty_read_timeout_ms;              /* posix_fd.c 의 read 가 본다 */
void *__nl_sbrk(long increment);                   /* 이전 brk, 실패하면 NULL + ENOMEM */
int __nl_remove(const char *path);
int __nl_rename(const char *from, const char *to);
/* 시계: NL_CLOCK_MONOTONIC(부팅 후 나노초) / NL_CLOCK_REALTIME(1970-01-01 UTC 이후 나노초).
 * 지원하지 않으면 -1 + errno */
#define NL_CLOCK_MONOTONIC 0
#define NL_CLOCK_REALTIME  1
long __nl_clock_ns(int which);
long __nl_getpid(void);                            /* 이 프로세스의 번호 (tmpnam 의 이름에 쓴다) */
const char *__nl_tmpdir(void);                     /* 임시 파일 디렉터리 접두어 ('/' 로 끝남) */
void __nl_exit(int status) __attribute__((noreturn));

/* ---- POSIX 계층이 쓰는 것 ---- */
/* fd <-> 핸들. fd 는 nlibc 가 연 핸들에만 있다 */
long __nl_fd_to_handle(int fd);                    /* 없으면 -1 + EBADF */
int __nl_handle_to_fd(long h);
/* 열린 파일을 공유하는 복제. how: NL_DUP_ANY(가장 낮은 fd), NL_DUP_TO(정확히 arg 번 fd, 열려 있으면 닫고 = dup2),
 * NL_DUP_MIN(arg 이상의 가장 낮은 fd = F_DUPFD) */
#define NL_DUP_ANY 0
#define NL_DUP_TO  1
#define NL_DUP_MIN 2
long __nl_dup_ex(long h, int how, int arg, int cloexec);
int __nl_handle_cloexec(long h, int set);          /* set: -1 = 조회만, 0/1 = 바꾸기. 반환: 지금 값 또는 -1 */
int __nl_pipe(long out[2], int cloexec);           /* out[0] 읽기, out[1] 쓰기 */
int __nl_openpty(long out[2], int cloexec);        /* 의사 터미널: out[0] 주 끝, out[1] 종속 끝. 없으면 ENOSYS */
int __nl_tty_set_winsize(long h, const unsigned short ws[4]);  /* 행, 열, x/y 픽셀 (pty 의 어느 끝이든) */
/* 장치 (nx64 의 화면): NxDeviceCommand / NxDeviceMapMMIO. 없는 플랫폼은 -1 / NULL + ENOSYS */
long __nl_devcmd(long h, unsigned long cmd, unsigned long arg);
void *__nl_map_mmio(long h);

#define NL_T_FILE  1
#define NL_T_DIR   2
#define NL_T_CHR   3        /* 콘솔, 문자 장치 */
#define NL_T_BLK   4
#define NL_T_FIFO  5        /* 채널 */
#define NL_T_OTHER 6        /* 프로세스/토큰 핸들 등 */
struct __nl_stat {
    int type;
    unsigned long size;
    int readable;
    int writable;
    unsigned int mode;      /* 권한 비트 (0777), 모르면 0 */
    unsigned int uid;
    unsigned int gid;
};
int __nl_fstat(long h, struct __nl_stat *st);
int __nl_stat_path(const char *path, struct __nl_stat *st);
int __nl_mkdir(const char *path, unsigned int mode);
int __nl_rmdir(const char *path);                  /* 빈 디렉터리 */
int __nl_unlink(const char *path);                 /* 파일 */
int __nl_access(const char *path, int amode);      /* unistd.h 의 R_OK/W_OK/X_OK/F_OK */
int __nl_chmod(const char *path, unsigned int mode);
unsigned int __nl_umask(unsigned int mask);        /* 이전 값 */
/* index 번째 항목. 반환 1 = 있음, 0 = 끝, -1 = 오류 */
int __nl_readdir(const char *path, unsigned long index, char *name, size_t cap, int *type);
int __nl_sleep_ms(unsigned long ms);
/* 신원. name 은 모르면 빈 문자열 */
int __nl_identity(unsigned int *uid, unsigned int *gid, char *name, size_t cap);

/* 서비스 채널 (svc:/, nx64api). 없는 플랫폼은 -1 + ENOSYS. 핸들은 __nl_close 로 닫는다 */
long __nl_channel_create(const char *path, int public_);       /* 받는 쪽 핸들 */
long __nl_channel_open(const char *path);                      /* 보내는 쪽 핸들 */
long __nl_channel_send(long h, const void *buf, size_t len);
/* sender: pid, uid, gid (커널이 붙인다). timeout_ms < 0 이면 끝없이. 메시지 길이 */
long __nl_channel_receive(long h, void *buf, size_t cap, unsigned long sender[3], long timeout_ms);
/* 핸들을 붙여 보내기/받기 (컴포지터 연결). 받은 핸들은 fd 로 등록해 *out_fd 에 (없으면 -1) */
int __nl_channel_send_handle(long h, const void *buf, size_t len, long attach);
long __nl_channel_receive_handle(long h, void *buf, size_t cap, unsigned long sender[3], long timeout_ms, int *out_fd);
/* 핸들을 fd 로 등록한다 (poll/read/close 에 쓰게). fd, 실패 -1 */
int __nl_adopt_handle(long h, int flags);
/* 공유 메모리: 핸들 (fd 로 등록됨) / 매핑 주소 */
long __nl_shm_create(size_t size, int cloexec);
void *__nl_shm_map(long h);

/* 프로세스. 핸들 h 는 플랫폼이 정한 값 (Nyxis: 프로세스 핸들, Linux: pid) */
/* inherit: 부모의 fd 를 같은 번호로 물려준다 (FD_CLOEXEC 제외) */
long __nl_spawn(const char *path, char *const argv[], char *const envp[], int inherit);
long __nl_proc_pid(long h);
long __nl_fork(void);                              /* 부모: 자식 핸들, 자식: 0, 실패: -1 */
int __nl_exec(const char *path, char *const argv[], char *const envp[]);   /* 실패할 때만 돌아온다 */
long __nl_getppid(void);
/* 끝날 때까지(nohang 이면 확인만) 기다린다. 반환 1 = 끝남(*wstatus 는 POSIX 형식), 0 = 아직, -1 = 오류 */
int __nl_proc_wait(long h, int nohang, int *wstatus);
/* hs[0..n) 중 끝난 것 하나의 번호, 아직 없으면(nohang) -2, 오류 -1 */
int __nl_proc_wait_any(const long *hs, int n, int nohang);
int __nl_proc_kill(long h);
/*
 * 신호 (library/signal.h). 집합은 비트 (sig - 1). flags 는 SA_* (Linux 와 같은 값). h = -1 이면 자기 자신.
 * 실패하면 -1 + errno. __nl_sigsuspend 는 늘 -1 + EINTR.
 */
struct __nl_sigact {
    unsigned long handler;      /* SIG_DFL(0) / SIG_IGN(1) / 함수 */
    unsigned long mask;
    int flags;
};
int __nl_sigaction(int sig, const struct __nl_sigact *act, struct __nl_sigact *old);
int __nl_sigprocmask(int how, const unsigned long *set, unsigned long *old);
int __nl_sigsend(long h, int sig);
int __nl_sigpending(unsigned long *set);
int __nl_sigsuspend(const unsigned long *set);
/* 프로세스 그룹: h = -1 이면 자신. pgid = -1 이면 조회, 0 이면 새 그룹(대상 pid). 반환 = 그룹 번호, 오류 -1 */
long __nl_proc_group(long h, long pgid);
/* 콘솔의 포그라운드 그룹: pgid = -1 이면 조회, 0 이면 없음(Nyxis). 반환 = 지금 값(조회) 또는 0, 오류 -1 */
long __nl_tty_foreground(long h, long pgid);
void __nl_proc_release(long h);                    /* 기다린 뒤 핸들 반납 */
/* 시스템의 프로세스 목록 (nx64 의 ntop). 살아 있는 프로세스 수 (max 보다 클 수 있다), 없는 플랫폼은 -1 + ENOSYS */
struct __nl_procent {
    unsigned long pid, ppid, pgid, uid;
    int state;                  /* 0 READY 1 RUNNING 2 BLOCKED */
    unsigned int flags;         /* 1 사용자 프로세스, 2 콘솔 세션, 4 pty 터미널, 8 보호 프로세스 */
    unsigned long cpu_ticks, start_tick, mem_kib;
    char name[32];
    char user[32];
};
int __nl_proc_list(struct __nl_procent *out, int max);
int __nl_signal_pid(unsigned long pid, int sig);   /* 핸들 없이 pid 로 (같은 uid 또는 root) */
unsigned long __nl_timer_hz(void);                 /* cpu_ticks 의 단위 (모르면 100) */
int __nl_check_password(const char *name, const char *password);   /* 맞으면 0, 틀리면 -1 + EACCES */

/* 경로 규칙 */
int __nl_path_is_absolute(const char *path);
int __nl_can_list(const char *abs);             /* 자식 목록을 읽을 수 있는 노드인지 (opendir/chdir) */
size_t __nl_path_root_len(const char *abs);        /* "tmp:/" -> 5, "/" -> 1 */
int __nl_initial_cwd(char *buf, size_t cap);       /* 0 성공 */
void __nl_cwd_changed(const char *abs);            /* 플랫폼에 알린다 (Linux: chdir) */
int __nl_command_path(const char *name, char *out, size_t cap);   /* posix_spawnp: 이름 -> 경로 */

/* ---- libc 공통: 현재 디렉터리 (posix_path.c) ---- */
#define NL_PATH_MAX 256
/* in 을 현재 디렉터리 기준으로 절대화하고 "." ".." 를 정리한다. 0 성공, -1 + errno */
int __nl_resolve(const char *in, char *out, size_t cap);
const char *__nl_cwd(void);

/* ---------------- libc 내부 ---------------- */
extern char **__nl_environ;
extern long __nl_start_ns;          /* 프로그램 시작 시각 (MONOTONIC, clock() 의 기준) */

/* stdio */
#define NL_F_READ    0x01   /* 읽기 허용 */
#define NL_F_WRITE   0x02   /* 쓰기 허용 */
#define NL_F_APPEND  0x04   /* 쓰기 전마다 끝으로 */
#define NL_F_EOF     0x08
#define NL_F_ERR     0x10
#define NL_F_MYBUF   0x20   /* buf 를 nlibc 가 malloc 했다 (fclose 에서 해제) */
#define NL_F_INUSE   0x40
#define NL_F_STATIC  0x80   /* stdin/stdout/stderr: 정적 슬롯, 정적 버퍼 */

#define NL_IO_IDLE   0
#define NL_IO_READ   1      /* buf[pos..len) 이 읽지 않은 데이터 */
#define NL_IO_WRITE  2      /* buf[0..pos) 가 아직 쓰지 않은 데이터 */

struct __nl_file {
    int fd;                 /* POSIX fd: 입출력마다 fd 표에서 핸들을 찾는다 (dup2 로 바뀌어도 따라간다) */
    int flags;
    int bufmode;            /* _IOFBF / _IOLBF / _IONBF */
    int io;                 /* NL_IO_* */
    int ungot;              /* ungetc 로 되돌린 문자, 없으면 EOF */
    unsigned char *buf;
    size_t bufsize;
    size_t pos;
    size_t len;
    unsigned char onebyte;  /* _IONBF 읽기용 1바이트 버퍼 */
    char *tmpname;          /* tmpfile: 닫을 때 지울 파일 이름 (malloc), 아니면 NULL */
};

void __nl_stdio_init(void);
void __nl_stdio_flush_all(void);    /* exit 가 부른다 */

/* printf 엔진: 출력 한 조각마다 sink 를 부른다. 반환: 출력한 총 바이트 수, sink 실패 시 -1 */
typedef int (*__nl_sink_fn)(void *ctx, const char *s, size_t n);
int __nl_vformat(__nl_sink_fn sink, void *ctx, const char *fmt, __builtin_va_list ap);

/* 유한한 |v| 의 정확한 십진 전개 (fpconv.c): 값 = 0.D1..Dn * 10^dexp, 끝의 0 없음. 반환 n (0 이면 값 0) */
#define NL_DTOA_MAX 800
int __nl_dtoa(double v, char *digits, int *dexp);

#endif /* NL_INTERNAL_H */
