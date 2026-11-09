/*
 * poll.h - 여러 fd 를 함께 기다린다 (POSIX poll)
 *
 * Nyxis 에서 기다릴 수 있는 것 (NxPoll): 파이프(읽기 끝 POLLIN, 쓰기 끝 POLLOUT, 상대가 모두 닫히면 POLLHUP),
 * 콘솔 입력(POLLIN: 줄 단위 모드면 완성된 줄, raw 모드면 바이트 하나라도), 채널. 일반 파일은 늘 준비된 것으로 본다.
 * 한 번에 최대 16 개 (넘으면 EINVAL). fd 가 음수면 건너뛰고, 열려 있지 않으면 POLLNVAL.
 * timeout: ms, 0 = 확인만, 음수 = 무기한. 신호가 오면 -1 + EINTR.
 */
#ifndef _NL_POLL_H
#define _NL_POLL_H

#define POLLIN   0x001
#define POLLPRI  0x002      /* 받지만 일어나지 않는다 */
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif /* _NL_POLL_H */
