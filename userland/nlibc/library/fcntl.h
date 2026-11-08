/*
 * fcntl.h - open() 과 플래그
 *
 * O_APPEND 는 nlibc 가 흉내 낸다 (쓰기 전마다 끝으로 옮긴다: 여러 프로세스가 같은 파일에 덧붙이면
 * 원자적이지 않다). O_TRUNC 는 지우고 다시 만든다 (커널에 자르기가 없다: README 의 제약).
 * FD_CLOEXEC/O_CLOEXEC 인 fd 는 exec 때 닫히고 posix_spawn 의 자식에게도 물려주지 않는다.
 */
#ifndef _NL_FCNTL_H
#define _NL_FCNTL_H

#include <sys/types.h>

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_CLOEXEC   0x80000

#define F_DUPFD     0
#define F_DUPFD_CLOEXEC 1030
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define FD_CLOEXEC  1

int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#endif /* _NL_FCNTL_H */
