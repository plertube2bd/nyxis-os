/*
 * spawn.h - posix_spawn (Nyxis 에는 fork/exec 가 없고 NxProcessCreate 하나로 새 프로그램을 띄운다)
 *
 * posix_spawnp 는 이름에 ':' 나 '/' 가 없으면 app:/<이름>.run 을 실행한다 (nysh 와 같은 규칙).
 * 자식은 FD_CLOEXEC 가 아닌 fd 를 같은 번호로 물려받고(NX_SPAWN_INHERIT), 현재 디렉터리는 PWD 로 물려받는다.
 * 파일 동작(adddup2/addclose/addopen)이 있으면 fork 한 자식에서 동작을 적용한 뒤 exec 한다: 실행 실패도
 * (CLOEXEC 파이프로 알려 받아) posix_spawn 의 반환값으로 정확히 돌려준다. 속성 플래그는 지원하지 않는다.
 * 동시에 기다릴 수 있는 자식은 NL_CHILD_MAX 개.
 */
#ifndef _NL_SPAWN_H
#define _NL_SPAWN_H

#include <sys/types.h>

#define NL_CHILD_MAX 16

struct __nl_spawn_action;

typedef struct {
    int __count;
    int __cap;
    struct __nl_spawn_action *__actions;   /* malloc 한 배열 (destroy 가 해제) */
} posix_spawn_file_actions_t;

typedef struct {
    short __flags;
} posix_spawnattr_t;

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp, char *const argv[], char *const envp[]);
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp, char *const argv[], char *const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path,
                                     int oflag, mode_t mode);
int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags);

#endif /* _NL_SPAWN_H */
