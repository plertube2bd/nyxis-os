/*
 * termios.c - tcgetattr/tcsetattr/ioctl (Nyxis 콘솔). 의미는 library/termios.h 참고.
 */
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "nl_internal.h"

static struct termios g_tio;
static int g_tio_init = 0;

/* 처음 상태: 정규 모드 + 에코 (커널 콘솔의 기본값과 같다) */
static void tio_defaults(void)
{
    if (g_tio_init)
        return;
    memset(&g_tio, 0, sizeof(g_tio));
    g_tio.c_iflag = BRKINT | ICRNL | IXON;
    g_tio.c_oflag = OPOST | ONLCR;
    g_tio.c_cflag = CS8 | CREAD;
    g_tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    g_tio.c_cc[VINTR] = 0x03;
    g_tio.c_cc[VERASE] = 0x7F;
    g_tio.c_cc[VKILL] = 0x15;
    g_tio.c_cc[VEOF] = 0x04;
    g_tio.c_cc[VMIN] = 1;
    g_tio.c_cc[VTIME] = 0;
    g_tio.c_ispeed = B38400;
    g_tio.c_ospeed = B38400;
    g_tio_init = 1;
}

/* fd 의 콘솔 핸들. 콘솔이 아니면 -1 + ENOTTY */
static long tty_handle(int fd)
{
    long h = __nl_fd_to_handle(fd);

    if (h < 0)
        return -1;
    if (!__nl_is_console(h)) {
        errno = ENOTTY;
        return -1;
    }
    return h;
}

int tcgetattr(int fd, struct termios *t)
{
    if (!t) {
        errno = EINVAL;
        return -1;
    }
    if (tty_handle(fd) < 0)
        return -1;
    tio_defaults();
    *t = g_tio;
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *t)
{
    long h;
    int raw;

    if (!t || optional_actions < TCSANOW || optional_actions > TCSAFLUSH) {
        errno = EINVAL;
        return -1;
    }
    h = tty_handle(fd);
    if (h < 0)
        return -1;
    tio_defaults();

    raw = (t->c_lflag & ICANON) ? 0 : 1;
    if (__nl_tty_mode(h, raw) < 0)
        return -1;
    if (!raw)
        (void)__nl_tty_echo(h, (t->c_lflag & ECHO) ? 1 : 0);   /* stdin 이 아니면 커널이 거절한다: 무시 */

    g_tio = *t;
    __nl_tty_read_timeout_ms = (raw && t->c_cc[VMIN] == 0) ? (long)t->c_cc[VTIME] * 100L : -1L;
    return 0;
}

void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(const struct termios *t)
{
    return t->c_ispeed;
}

speed_t cfgetospeed(const struct termios *t)
{
    return t->c_ospeed;
}

int cfsetispeed(struct termios *t, speed_t s)
{
    t->c_ispeed = s;
    return 0;
}

int cfsetospeed(struct termios *t, speed_t s)
{
    t->c_ospeed = s;
    return 0;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    struct winsize *ws;
    unsigned short v[4];
    long h;

    h = tty_handle(fd);
    if (h < 0)
        return -1;
    if (request != TIOCGWINSZ) {
        errno = EINVAL;
        return -1;
    }
    va_start(ap, request);
    ws = va_arg(ap, struct winsize *);
    va_end(ap);
    if (!ws) {
        errno = EFAULT;
        return -1;
    }
    if (__nl_tty_winsize(h, v) < 0)
        return -1;
    ws->ws_row = v[0];
    ws->ws_col = v[1];
    ws->ws_xpixel = v[2];
    ws->ws_ypixel = v[3];
    return 0;
}
