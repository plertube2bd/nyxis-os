/*
 * term.c - 터미널: raw 모드, 키 입력
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "ntui.h"

static int g_tty = -1;                  /* 콘솔 fd (dev:/tty, 안 되면 stdin) */
static struct termios g_orig;           /* 되돌릴 원래 설정 */

/* raw 모드를 끄고 원래대로. 프로세스가 죽으면 커널도 cooked 로 되돌린다 */
static void ttyRestore(void)
{
    if (g_tty >= 0)
        tcsetattr(g_tty, TCSAFLUSH, &g_orig);
}

/*
 * raw 모드를 켠다. tcsetattr 는 부를 때마다 읽지 않은 입력을 버리므로(커널 tty_set_raw) 여기서 한 번만.
 * VMIN=0, VTIME=1: read() 가 100ms 동안 바이트가 없으면 0 을 돌려준다 (ESC 단독 키를 구분하려고).
 * stdio(fgetc) 에는 VTIME 이 적용되지 않으므로 입력은 read() 로 읽는다.
 */
int ttyOpen(void)
{
    struct termios t;
    int fd;

    if (g_tty >= 0)
        return 0;
    fd = open("dev:/tty", O_RDWR);
    if (fd < 0)
        fd = 0;
    if (tcgetattr(fd, &g_orig) < 0)
        goto fail;
    t = g_orig;
    cfmakeraw(&t);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSAFLUSH, &t) < 0)
        goto fail;
    g_tty = fd;                         /* raw 가 된 뒤에만: 실패하면 다음 호출이 다시 시도한다 */
    atexit(ttyRestore);
    return 0;

fail:
    if (fd > 0)
        close(fd);
    return -1;
}

/* 콘솔 fd (ttyOpen 이후. 그 전이면 -1) */
int ttyFd(void)
{
    return g_tty;
}

/* 100ms 안에 1 바이트. 시간 초과면 -1, 읽기 오류면 -2 */
static int readb(void)
{
    unsigned char c;
    ssize_t n = read(g_tty, &c, 1);

    if (n == 1)
        return c;
    return n == 0 ? -1 : -2;
}

/*
 * 키 하나를 기다려 돌려준다. Enter = '\r', Backspace = 0x7F/0x08, Ctrl-C = 0x03, Ctrl-D = 0x04.
 * 방향키 등의 VT 시퀀스는 KEY_* 로 묶는다. 실패하면 EOF.
 */

int getch(void)
{
    int c, a, b;

    if (ttyOpen() < 0)
        return EOF;
    fflush(stdout);                     /* 그린 내용이 먼저 화면에 나가도록 */

    while ((c = readb()) == -1)     /* 키가 올 때까지 */
        ;
    if (c < 0)
        return EOF;                     /* 읽기 오류: 계속 돌면 CPU 를 다 쓴다 */
    if (c != 27)
        return c;

    /* PS/2 는 시퀀스가 한꺼번에 들어오고, 시리얼은 바이트가 따로 온다: 이어지는 바이트는 100ms 만 기다린다 */
    if ((a = readb()) < 0 || a != '[')
        return KEY_ESC;
    if ((b = readb()) < 0)
        return KEY_ESC;

    switch (b) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '1': case '3': case '4': case '5': case '6': case '7': case '8':
        if (readb() != '~')
            return KEY_ESC;
        switch (b) {
        case '1': case '7': return KEY_HOME;   /* 시리얼 터미널마다 다르다 */
        case '4': case '8': return KEY_END;
        case '3': return KEY_DEL;
        case '5': return KEY_PGUP;
        case '6': return KEY_PGDN;
        }
    }
    return KEY_ESC;
}
