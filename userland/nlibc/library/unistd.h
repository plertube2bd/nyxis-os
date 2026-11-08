/*
 * unistd.h - POSIX 기본 함수 (nlibc 가 Nyxis 위에서 제공하는 부분)
 *
 * fd 는 Nyxis 핸들의 슬롯 번호다 (0/1/2 = stdin/stdout/stderr, 새 fd 는 가장 낮은 빈 번호).
 * 경로: "이름공간:/..." (app:/ face:/ tmp:/ ...) 는 절대 경로, 나머지는 현재 디렉터리 기준의 상대 경로.
 * 현재 디렉터리는 nlibc 가 관리한다 (커널에는 그런 개념이 없다): 시작값은 환경 변수 PWD,
 * 없으면 face:/ (사용자 홈). posix_spawn/system 으로 만든 자식은 PWD 로 물려받는다.
 *
 * fork 는 부모 메모리를 즉시 복사한다 (쓰기 시 복사가 아직 없다): 큰 프로그램이면 posix_spawn 이 훨씬 싸다.
 * fork 한 자식은 stdio 버퍼도 복사본을 가지므로, exec 하지 않고 끝낼 때는 _exit 를 쓴다 (exit 는 버퍼를 한 번 더 비운다).
 * exec 는 FD_CLOEXEC 가 아닌 fd 와 현재 디렉터리(PWD)를 새 프로그램에 넘긴다.
 *
 * 없는 것: chown, setuid 류, 프로세스 그룹, 진짜 신호 (signal.h 머리말).
 */
#ifndef _NL_UNISTD_H
#define _NL_UNISTD_H

#define _NL_WANT_NULL
#include "_nl_types.h"
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* access() */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

extern char **environ;

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int dup(int fd);
int dup2(int fd, int fd2);
int pipe(int fds[2]);
int isatty(int fd);

int unlink(const char *path);
int rmdir(const char *path);
int access(const char *path, int mode);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);

pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *arg0, ...);
int execlp(const char *file, const char *arg0, ...);
int execle(const char *path, const char *arg0, ...);     /* ..., (char *)0, char *const envp[] */
void _exit(int status) __attribute__((__noreturn__));

pid_t getpid(void);
pid_t getppid(void);
/*
 * 프로세스 그룹 (콘솔의 Ctrl-C 가 함께 끝내는 묶음). setpgid 는 자신(pid 0) 또는 자기 자식만, pgid 0 = 새 그룹.
 * tcsetpgrp: 콘솔의 포그라운드 그룹. Nyxis 에서는 pgid 0 = "포그라운드 작업 없음" (Ctrl-C 가 아무것도 끝내지
 * 않고 빈 줄을 넘긴다: 셸이 프롬프트에서 쓴다). 다른 사용자의 프로세스가 든 그룹은 정할 수 없다 (EPERM).
 * 커널에 신호가 없으므로 Ctrl-C 로 끝난 프로세스는 처리기 없이 끝나고 waitpid 에는 SIGINT 로 끝난 것으로 보인다.
 */
pid_t getpgrp(void);
pid_t getpgid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgid);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
char *getlogin(void);

unsigned int sleep(unsigned int seconds);
int pause(void);                 /* 신호가 와서 처리기가 돌아올 때까지 잔다. 늘 -1 + EINTR */

#endif /* _NL_UNISTD_H */
