/*
 * nyx.h - Nyxis 유저 프로그램 공용 런타임 (init, login, nysh, poweroff, elftest 가 링크한다)
 *
 * 표준 C 라이브러리가 아직 없으므로 필요한 것만 둔다: 시스템 콜 래퍼, 문자열/메모리, 콘솔 출력, 줄 입력.
 * 시스템 콜 규약과 상수/구조체는 include/nyx_abi.h. 오류는 음수 Nstatus 그대로 돌려준다.
 * 진입점(crt0.s)이 main(argc, argv, envp) 를 부르고 그 반환값으로 NxProcessExit 한다.
 */
#ifndef NYX_USER_H
#define NYX_USER_H

#include <nyx_abi.h>

/* 커널 Nstatus 오류 값 (include/types.h 의 NSTATUS_ERR_FLAG | code 를 부호 확장한 것) */
#define NX_ERR(code)        ((long)(int)(0x80000000U | (unsigned int)(code)))
#define NX_E_NOT_FOUND      NX_ERR(1)
#define NX_E_INVALID_ARG    NX_ERR(2)
#define NX_E_PERMISSION     NX_ERR(4)
#define NX_E_TIMEOUT        NX_ERR(5)
#define NX_E_BUSY           NX_ERR(6)
#define NX_E_EXISTS         NX_ERR(7)
#define NX_E_OVERFLOW       NX_ERR(8)
#define NX_E_DISCONNECTED   NX_ERR(11)
#define NX_E_UNSUPPORTED    NX_ERR(12)
#define NX_E_BUF_OVERFLOW   NX_ERR(27)
#define NX_E_AUTH           NX_ERR(45)

int main(int argc, char **argv, char **envp);

/* 시스템 콜 */
long nx_syscall(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                unsigned long a3, unsigned long a4);
long nx_open(const char *path, unsigned long flags);
long nx_close(long h);
long nx_read(long h, void *buf, unsigned long len);
long nx_write(long h, const void *buf, unsigned long len);
long nx_seek(long h, long offset, unsigned long whence);
long nx_stat(long h, struct nx_stat *out);
long nx_dup(long h, unsigned long rights);
long nx_sleep(unsigned long ms);
long nx_identity(struct nx_identity *out);
long nx_authenticate(const char *name, const char *password);
long nx_spawn(const char *path, const char *const *argv, const char *const *envp, long token);
long nx_wait(long h, unsigned long timeout_ms, int *code);
long nx_kill(long h);
long nx_process_info(long h, struct nx_procinfo *out);
long nx_poll(struct nx_pollfd *fds, unsigned long n, unsigned long timeout_ms);
long nx_channel_create(const char *path, unsigned long flags);
long nx_channel_send(long h, const void *buf, unsigned long len);
long nx_channel_receive(long h, void *buf, unsigned long len, struct nx_msginfo *info, unsigned long timeout_ms);
long nx_create_node(const char *path, unsigned long type, unsigned long mode);
long nx_remove_node(const char *path);
long nx_rename_node(const char *from, const char *to);
long nx_read_dir(const char *path, unsigned long index, struct nx_dirent *out);
long nx_device_command(long h, unsigned long cmd, unsigned long arg);
long nx_device_map(long h, unsigned long *out_addr);
long nx_set_echo(int on);
long nx_power_off(void);
void nx_exit(int code) __attribute__((noreturn));

/* 문자열/메모리 */
unsigned long nx_strlen(const char *s);
int nx_strcmp(const char *a, const char *b);
int nx_strncmp(const char *a, const char *b, unsigned long n);
void *nx_memset(void *p, int v, unsigned long n);
void *nx_memcpy(void *d, const void *s, unsigned long n);
int nx_memcmp(const void *a, const void *b, unsigned long n);
void nx_wipe(void *p, unsigned long n);      /* 비밀번호 등 (컴파일러가 없애지 못함) */

/* 콘솔 */
void nx_puts(const char *s);                 /* stdout 에 그대로 (개행 추가 없음) */
void nx_putu(unsigned long v);
void nx_puti(long v);

/* stdin 에서 한 줄 읽기. 개행은 떼고 NUL 로 끝낸다. 반환: 길이, EOF 면 -1, 오류면 음수 Nstatus.
 * 줄이 buf 보다 길면 잘린다 (나머지는 버린다) */
long nx_readline(char *buf, unsigned long max);

#endif /* NYX_USER_H */
