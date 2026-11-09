/*
 * poll.c - poll (의미는 library/poll.h). fd 를 핸들로 바꿔 플랫폼 계층의 __nl_poll 에 넘긴다.
 */
#include <poll.h>
#include <errno.h>

#include "nl_internal.h"

#define POLL_MAX 16

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    struct __nl_pollent ent[POLL_MAX];
    int map[POLL_MAX];                  /* ent[k] 가 가리키는 fds 번호 */
    int n = 0;
    int ready = 0;
    int r;
    nfds_t i;

    if (nfds > POLL_MAX || (nfds > 0 && fds == NULL)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < nfds; i++) {
        long h;

        fds[i].revents = 0;
        if (fds[i].fd < 0)
            continue;
        h = __nl_fd_to_handle(fds[i].fd);
        if (h < 0) {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        ent[n].h = h;
        ent[n].events = ((fds[i].events & (POLLIN | POLLPRI)) ? NL_POLL_IN : 0) |
                        ((fds[i].events & POLLOUT) ? NL_POLL_OUT : 0);
        ent[n].revents = 0;
        map[n] = (int)i;
        n++;
    }

    if (n == 0) {
        if (ready == 0 && timeout != 0)
            (void)__nl_sleep_ms(timeout < 0 ? 0x7FFFFFFFUL : (unsigned long)timeout);
        return ready;
    }
    errno = 0;
    r = __nl_poll(ent, n, ready > 0 ? 0 : (long)timeout);
    if (r < 0)
        return -1;

    for (r = 0; r < n; r++) {
        short rev = 0;

        if (ent[r].revents & NL_POLL_IN)
            rev |= (short)(fds[map[r]].events & (POLLIN | POLLPRI) ? POLLIN : 0);
        if (ent[r].revents & NL_POLL_OUT)
            rev |= (short)(fds[map[r]].events & POLLOUT ? POLLOUT : 0);
        if (ent[r].revents & NL_POLL_HUP)
            rev |= POLLHUP;                 /* events 와 상관없이 늘 알린다 (POSIX) */
        fds[map[r]].revents = rev;
        if (rev)
            ready++;
    }
    return ready;
}
