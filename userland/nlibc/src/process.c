/*
 * process.c - 프로세스와 신원 (unistd.h, spawn.h, sys/wait.h, signal.h 의 kill, stdlib.h 의 system)
 *
 * Nyxis 는 pid 가 아니라 프로세스 "핸들" 로 기다리고 죽인다. 그래서 fork/posix_spawn 으로 만든 자식의
 * pid -> 핸들 표를 여기에 둔다 (NL_CHILD_MAX 개). waitpid 로 거둔 자식은 표에서 빠지고 핸들도 반납한다.
 * waitpid(-1) 은 NxPoll 의 NX_POLL_EXIT 로 "아무 자식이나 끝나기" 를 기다린다.
 *
 * 현재 디렉터리는 커널이 모르므로 exec/spawn 할 때 환경 변수 PWD 로 넘긴다 (env_with_pwd).
 */
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nl_internal.h"

/* ---------------- 신원 ---------------- */
pid_t getpid(void)
{
    return (pid_t)__nl_getpid();
}

pid_t getppid(void)
{
    return (pid_t)__nl_getppid();
}

static unsigned int id_field(int which)
{
    unsigned int uid = 0;
    unsigned int gid = 0;

    (void)__nl_identity(&uid, &gid, NULL, 0);
    return which ? gid : uid;
}

uid_t getuid(void)  { return id_field(0); }
uid_t geteuid(void) { return id_field(0); }      /* 실효 신원이 따로 없다 */
gid_t getgid(void)  { return id_field(1); }
gid_t getegid(void) { return id_field(1); }

char *getlogin(void)
{
    static char name[64];
    unsigned int uid;
    unsigned int gid;
    const char *env;

    if (__nl_identity(&uid, &gid, name, sizeof(name)) == 0 && name[0] != '\0')
        return name;
    env = getenv("LOGNAME");
    if (env == NULL)
        env = getenv("USER");
    if (env == NULL || strlen(env) >= sizeof(name))
        return NULL;
    strcpy(name, env);
    return name;
}

unsigned int sleep(unsigned int seconds)
{
    (void)__nl_sleep_ms((unsigned long)seconds * 1000UL);
    return 0;
}

void _exit(int status)
{
    __nl_exit(status);                  /* atexit 처리기와 stdio 비우기 없이 */
}

/* ---------------- 자식 표 ---------------- */
struct child {
    pid_t pid;
    long h;
};

static struct child children[NL_CHILD_MAX];
static int nchildren;

static int find_child(pid_t pid)
{
    int i;

    for (i = 0; i < nchildren; i++) {
        if (children[i].pid == pid)
            return i;
    }
    return -1;
}

static void drop_child(int i)
{
    __nl_proc_release(children[i].h);
    children[i] = children[--nchildren];
}

static pid_t add_child(long h)
{
    long p = __nl_proc_pid(h);

    if (p < 0)
        p = 0;
    children[nchildren].pid = (pid_t)p;
    children[nchildren].h = h;
    nchildren++;
    return (pid_t)p;
}

/* envp 에서 PWD 를 빼고 지금의 현재 디렉터리로 다시 넣은 배열 (malloc). 자식이 현재 디렉터리를 물려받는 방법 */
static char **env_with_pwd(char *const envp[], char *pwd_buf, size_t cap)
{
    size_t n = 0;
    size_t k = 0;
    size_t i;
    char **out;
    const char *cwd = __nl_cwd();

    while (envp && envp[n])
        n++;
    out = (char **)malloc((n + 2) * sizeof(char *));
    if (out == NULL)
        return NULL;
    for (i = 0; i < n; i++) {
        if (strncmp(envp[i], "PWD=", 4) != 0)
            out[k++] = envp[i];
    }
    if (cwd[0] != '\0' && strlen(cwd) + 5 <= cap) {
        strcpy(pwd_buf, "PWD=");
        strcat(pwd_buf, cwd);
        out[k++] = pwd_buf;
    }
    out[k] = NULL;
    return out;
}

/* ---------------- fork / exec ---------------- */
pid_t fork(void)
{
    long h;

    if (nchildren >= NL_CHILD_MAX) {
        errno = EAGAIN;                 /* 거둘 수 없는 자식을 만들지 않는다 */
        return -1;
    }
    h = __nl_fork();
    if (h < 0)
        return -1;
    if (h == 0) {
        nchildren = 0;                  /* 부모의 자식은 내 자식이 아니다 (커널도 그 핸들을 물려주지 않았다) */
        return 0;
    }
    return add_child(h);
}

/* path 는 이미 절대 경로 */
static int exec_abs(const char *abs, char *const argv[], char *const envp[])
{
    char pwd[NL_PATH_MAX + 8];
    char **env = env_with_pwd(envp, pwd, sizeof(pwd));

    if (env == NULL)
        return -1;
    (void)__nl_exec(abs, argv, env);    /* 성공하면 돌아오지 않는다 */
    free(env);
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    char abs[NL_PATH_MAX];

    if (__nl_resolve(path, abs, sizeof(abs)) < 0)
        return -1;
    return exec_abs(abs, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    char abs[NL_PATH_MAX];

    if (strchr(file, '/') != NULL || __nl_path_is_absolute(file))
        return execv(file, argv);
    if (__nl_command_path(file, abs, sizeof(abs)) < 0)
        return -1;
    return exec_abs(abs, argv, environ);
}

/* execl 류: 가변 인자를 NULL 로 끝나는 배열로 모은다. with_env 면 NULL 다음 인자가 envp */
#define EXECL_MAX 256

static int execl_common(const char *path, const char *arg0, va_list ap, int mode)
{
    char *argv[EXECL_MAX + 1];
    char *const *envp = environ;
    int n = 0;
    const char *a = arg0;

    while (a != NULL) {
        if (n == EXECL_MAX) {
            errno = E2BIG;
            return -1;
        }
        argv[n++] = (char *)a;
        a = va_arg(ap, const char *);
    }
    argv[n] = NULL;
    if (mode == 2)
        envp = va_arg(ap, char *const *);
    if (mode == 1)
        return execvp(path, argv);
    return execve(path, argv, envp);
}

int execl(const char *path, const char *arg0, ...)
{
    va_list ap;
    int r;

    va_start(ap, arg0);
    r = execl_common(path, arg0, ap, 0);
    va_end(ap);
    return r;
}

int execlp(const char *file, const char *arg0, ...)
{
    va_list ap;
    int r;

    va_start(ap, arg0);
    r = execl_common(file, arg0, ap, 1);
    va_end(ap);
    return r;
}

int execle(const char *path, const char *arg0, ...)
{
    va_list ap;
    int r;

    va_start(ap, arg0);
    r = execl_common(path, arg0, ap, 2);
    va_end(ap);
    return r;
}

/* ---------------- posix_spawn ---------------- */
#define ACT_CLOSE 1
#define ACT_DUP2  2
#define ACT_OPEN  3

struct __nl_spawn_action {
    int kind;
    int fd;
    int newfd;          /* dup2 */
    int oflag;          /* open */
    mode_t mode;
    char *path;         /* open: malloc 한 사본 */
};

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
    fa->__count = 0;
    fa->__cap = 0;
    fa->__actions = NULL;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
    int i;

    for (i = 0; i < fa->__count; i++)
        free(fa->__actions[i].path);
    free(fa->__actions);
    fa->__actions = NULL;
    fa->__count = fa->__cap = 0;
    return 0;
}

static struct __nl_spawn_action *new_action(posix_spawn_file_actions_t *fa, int kind, int fd)
{
    struct __nl_spawn_action *a;

    if (fd < 0)
        return NULL;
    if (fa->__count == fa->__cap) {
        int cap = fa->__cap ? fa->__cap * 2 : 4;
        struct __nl_spawn_action *n = (struct __nl_spawn_action *)realloc(fa->__actions, (size_t)cap * sizeof(*n));

        if (n == NULL)
            return NULL;
        fa->__actions = n;
        fa->__cap = cap;
    }
    a = &fa->__actions[fa->__count++];
    memset(a, 0, sizeof(*a));
    a->kind = kind;
    a->fd = fd;
    return a;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{
    if (fd < 0)
        return EBADF;
    return new_action(fa, ACT_CLOSE, fd) ? 0 : ENOMEM;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd)
{
    struct __nl_spawn_action *a;

    if (fd < 0 || newfd < 0)
        return EBADF;
    a = new_action(fa, ACT_DUP2, fd);
    if (a == NULL)
        return ENOMEM;
    a->newfd = newfd;
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path,
                                     int oflag, mode_t mode)
{
    struct __nl_spawn_action *a;
    char *copy;

    if (fd < 0)
        return EBADF;
    copy = (char *)malloc(strlen(path) + 1);
    if (copy == NULL)
        return ENOMEM;
    strcpy(copy, path);
    a = new_action(fa, ACT_OPEN, fd);
    if (a == NULL) {
        free(copy);
        return ENOMEM;
    }
    a->path = copy;
    a->oflag = oflag;
    a->mode = mode;
    return 0;
}

int posix_spawnattr_init(posix_spawnattr_t *attr) { attr->__flags = 0; return 0; }
int posix_spawnattr_destroy(posix_spawnattr_t *attr) { (void)attr; return 0; }

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
    if (flags != 0)
        return EINVAL;                  /* 지원하는 플래그가 없다 */
    attr->__flags = flags;
    return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags)
{
    *flags = attr->__flags;
    return 0;
}

/* fork 한 자식에서 파일 동작을 적용한다. 실패하면 errno */
static int apply_actions(const posix_spawn_file_actions_t *fa)
{
    int i;

    for (i = 0; i < fa->__count; i++) {
        const struct __nl_spawn_action *a = &fa->__actions[i];

        if (a->kind == ACT_CLOSE) {
            if (close(a->fd) < 0)
                return -1;
        } else if (a->kind == ACT_DUP2) {
            if (a->fd == a->newfd) {        /* POSIX: 같은 번호면 FD_CLOEXEC 만 끈다 */
                if (fcntl(a->fd, F_SETFD, 0) < 0)
                    return -1;
            } else if (dup2(a->fd, a->newfd) < 0) {
                return -1;
            }
        } else {
            int fd = open(a->path, a->oflag, a->mode);

            if (fd < 0)
                return -1;
            if (fd != a->fd) {
                if (dup2(fd, a->fd) < 0)
                    return -1;
                (void)close(fd);
            }
        }
    }
    return 0;
}

/* 파일 동작이 있을 때: fork -> 동작 -> exec. exec 가 실패하면 CLOEXEC 파이프로 errno 를 부모에게 보낸다
 * (exec 가 성공하면 파이프가 저절로 닫혀 부모는 EOF 를 받는다) */
static int spawn_with_actions(pid_t *pid, const char *abs, const posix_spawn_file_actions_t *fa,
                              char *const argv[], char *const envp[])
{
    long ph[2];
    int rfd;
    int wfd;
    pid_t child;
    int err = 0;
    int status;

    if (__nl_pipe(ph, 1) < 0)
        return errno;
    rfd = __nl_handle_to_fd(ph[0]);
    wfd = __nl_handle_to_fd(ph[1]);
    child = fork();
    if (child < 0) {
        err = errno;
        (void)close(rfd);
        (void)close(wfd);
        return err;
    }
    if (child == 0) {
        (void)close(rfd);
        if (apply_actions(fa) == 0)
            (void)exec_abs(abs, argv, envp);
        err = errno;
        (void)write(wfd, &err, sizeof(err));
        _exit(127);
    }
    (void)close(wfd);
    if (read(rfd, &err, sizeof(err)) == (ssize_t)sizeof(err)) {
        (void)close(rfd);
        (void)waitpid(child, &status, 0);   /* 실패한 자식을 거둔다 */
        return err ? err : EIO;
    }
    (void)close(rfd);
    if (pid)
        *pid = child;
    return 0;
}

static int spawn_abs(pid_t *pid, const char *abs, const posix_spawn_file_actions_t *fa,
                     const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    char pwd[NL_PATH_MAX + 8];
    char **env;
    long h;
    int saved = errno;
    int err;
    pid_t p;

    if (attrp && attrp->__flags != 0)
        return ENOTSUP;
    if (nchildren >= NL_CHILD_MAX)
        return EAGAIN;
    if (fa && fa->__count > 0) {
        err = spawn_with_actions(pid, abs, fa, argv, envp);
        errno = saved;
        return err;
    }
    env = env_with_pwd(envp, pwd, sizeof(pwd));
    if (env == NULL)
        return ENOMEM;
    h = __nl_spawn(abs, argv, env, 1);  /* fd 를 물려준다 (FD_CLOEXEC 제외) */
    free(env);
    if (h < 0) {
        err = errno;
        errno = saved;
        return err;
    }
    p = add_child(h);
    if (pid)
        *pid = p;
    errno = saved;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    char abs[NL_PATH_MAX];
    int saved = errno;
    int err;

    if (__nl_resolve(path, abs, sizeof(abs)) < 0) {
        err = errno;
        errno = saved;
        return err;
    }
    return spawn_abs(pid, abs, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    char abs[NL_PATH_MAX];
    int saved = errno;
    int err;

    if (strchr(file, '/') != NULL || __nl_path_is_absolute(file))
        return posix_spawn(pid, file, file_actions, attrp, argv, envp);
    if (__nl_command_path(file, abs, sizeof(abs)) < 0) {
        err = errno;
        errno = saved;
        return err;
    }
    return spawn_abs(pid, abs, file_actions, attrp, argv, envp);
}

/* ---------------- 기다리기 ---------------- */
pid_t waitpid(pid_t pid, int *status, int options)
{
    int nohang = (options & WNOHANG) != 0;
    int st = 0;
    int i;
    int r;
    pid_t done;

    if (options & ~WNOHANG) {
        errno = EINVAL;
        return -1;
    }
    if (pid > 0) {
        i = find_child(pid);
        if (i < 0) {
            errno = ECHILD;
            return -1;
        }
    } else {                                    /* 프로세스 그룹이 없다: 0 과 음수는 "아무 자식" */
        long hs[NL_CHILD_MAX];

        if (nchildren == 0) {
            errno = ECHILD;
            return -1;
        }
        for (i = 0; i < nchildren; i++)
            hs[i] = children[i].h;
        i = __nl_proc_wait_any(hs, nchildren, nohang);
        if (i == -2)
            return 0;
        if (i < 0)
            return -1;
    }
    r = __nl_proc_wait(children[i].h, nohang, &st);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;                               /* WNOHANG 이고 아직 실행 중 */
    if (status)
        *status = st;
    done = children[i].pid;
    drop_child(i);
    return done;
}

pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}

/* ---------------- 신호 ---------------- */
int kill(pid_t pid, int sig)
{
    int i;

    if (sig < 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (pid == getpid())
        return sig == 0 ? 0 : __nl_sigsend(-1, sig);
    i = find_child(pid);
    if (i < 0) {
        errno = ESRCH;                          /* 자식이 아닌 프로세스에는 보낼 수단(핸들)이 없다 */
        return -1;
    }
    return __nl_sigsend(children[i].h, sig);    /* SIGKILL 은 커널이 바로 끝낸다 */
}

int pause(void)
{
    sigset_t cur;

    if (sigprocmask(SIG_BLOCK, 0, &cur) < 0)
        return -1;
    return sigsuspend(&cur);                     /* 신호가 와서 처리기가 돌아오면 -1 + EINTR */
}

/* ---------------- 프로세스 그룹 ---------------- */
/* pid 0 = 자신, 그 밖에는 fork/posix_spawn 으로 만든 자식만 (핸들이 있어야 한다) */
static long group_target(pid_t pid)
{
    int i;

    if (pid == 0 || pid == getpid())
        return -1;
    i = find_child(pid);
    if (i < 0) {
        errno = ESRCH;
        return -2;
    }
    return children[i].h;
}

int setpgid(pid_t pid, pid_t pgid)
{
    long h = group_target(pid);

    if (h == -2)
        return -1;
    if (pgid < 0) {
        errno = EINVAL;
        return -1;
    }
    return __nl_proc_group(h, (long)pgid) < 0 ? -1 : 0;
}

pid_t getpgid(pid_t pid)
{
    long h = group_target(pid);

    if (h == -2)
        return -1;
    return (pid_t)__nl_proc_group(h, -1);
}

pid_t getpgrp(void)
{
    return (pid_t)__nl_proc_group(-1, -1);
}

pid_t tcgetpgrp(int fd)
{
    long h = __nl_fd_to_handle(fd);

    if (h < 0)
        return -1;
    return (pid_t)__nl_tty_foreground(h, -1);
}

int tcsetpgrp(int fd, pid_t pgid)
{
    long h = __nl_fd_to_handle(fd);

    if (h < 0)
        return -1;
    if (pgid < 0) {
        errno = EINVAL;
        return -1;
    }
    return __nl_tty_foreground(h, (long)pgid) < 0 ? -1 : 0;
}

/* ---------------- system ---------------- */
/*
 * 명령 처리기 대신 nlibc 가 직접 한 줄을 나눠 실행한다: 공백으로 나누고, '...' 는 그대로,
 * "..." 안에서는 \" 와 \\ 만 풀며, 따옴표 밖의 \ 는 다음 글자를 그대로 둔다.
 * 첫 낱말은 posix_spawnp 규칙(app:/<이름>.run)으로 찾는다. 파이프/리디렉션/변수 치환은 없다.
 * 반환: waitpid 의 상태. 실행할 수 없으면 셸처럼 "127 로 종료" 한 상태.
 */
#define SYSTEM_ARGS 64

int system(const char *string)
{
    char buf[1024];
    char *argv[SYSTEM_ARGS + 1];
    int argc = 0;
    const char *s;
    char *o = buf;
    char *end = buf + sizeof(buf) - 1;
    pid_t pid;
    int status;
    int err;

    if (string == NULL)
        return 1;                               /* 명령 처리기가 있다 */
    for (s = string;;) {
        while (*s == ' ' || *s == '\t' || *s == '\n')
            s++;
        if (*s == '\0')
            break;
        if (argc == SYSTEM_ARGS) {
            errno = E2BIG;
            return -1;
        }
        argv[argc++] = o;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
            if (o >= end) {
                errno = E2BIG;
                return -1;
            }
            if (*s == '\'') {
                for (s++; *s && *s != '\'' && o < end; s++)
                    *o++ = *s;
                if (*s == '\'')
                    s++;
            } else if (*s == '"') {
                for (s++; *s && *s != '"' && o < end; s++) {
                    if (*s == '\\' && (s[1] == '"' || s[1] == '\\'))
                        s++;
                    *o++ = *s;
                }
                if (*s == '"')
                    s++;
            } else {
                if (*s == '\\' && s[1])
                    s++;
                *o++ = *s++;
            }
        }
        *o++ = '\0';
    }
    if (argc == 0)
        return 0;
    argv[argc] = NULL;

    (void)fflush(NULL);                         /* 자식은 같은 fd 에 바로 쓴다: 순서를 맞춘다 */
    err = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (err != 0)
        return 127 << 8;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return status;
}
