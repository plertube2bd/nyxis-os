/*
 * termios.h - 터미널 설정 (POSIX 의 작은 부분집합, Nyxis 콘솔용)
 *
 * Nyxis 콘솔은 두 가지 입력 모드만 있다 (nyx_abi.h 의 NX_DEVCMD_TTY_MODE):
 *   정규(ICANON)  커널 줄 편집기가 한 줄씩 넘긴다. ECHO 로 에코를 켜고 끈다 (stdin 만).
 *   비정규        줄 편집/에코 없이 바이트를 바로 넘긴다 ("raw"). 방향키 등은 VT 시퀀스(ESC [ A ...), Enter 는 '\r'.
 *                 VMIN == 0 이면 read 는 VTIME/10 초 동안 입력을 기다리다 없으면 0 을 돌려준다.
 *                 VMIN > 0 이면 입력이 하나라도 올 때까지 기다린다 (VMIN 개를 다 모으지는 않는다).
 * 그 밖의 플래그(ICRNL, OPOST, ISIG, IXON, 속도 ...)는 저장만 하고 효과가 없다: 출력은 늘 그대로 나가고,
 * Ctrl-C 같은 신호 글자는 없다. 설정은 프로세스마다 기억하며, 비정규 모드로 두고 끝나면 커널이 되돌린다.
 */
#ifndef _NL_TERMIOS_H
#define _NL_TERMIOS_H

typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

/* c_cc 번호 */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP    10

/* c_iflag */
#define IGNBRK   0000001
#define BRKINT   0000002
#define IGNPAR   0000004
#define PARMRK   0000010
#define INPCK    0000020
#define ISTRIP   0000040
#define INLCR    0000100
#define IGNCR    0000200
#define ICRNL    0000400
#define IXON     0002000
#define IXANY    0004000
#define IXOFF    0010000

/* c_oflag */
#define OPOST    0000001
#define ONLCR    0000004

/* c_cflag */
#define CSIZE    0000060
#define CS5      0000000
#define CS6      0000020
#define CS7      0000040
#define CS8      0000060
#define CSTOPB   0000100
#define CREAD    0000200
#define PARENB   0000400
#define PARODD   0001000
#define HUPCL    0002000
#define CLOCAL   0004000

/* c_lflag */
#define ISIG     0000001
#define ICANON   0000002
#define ECHO     0000010
#define ECHOE    0000020
#define ECHOK    0000040
#define ECHONL   0000100
#define NOFLSH   0000200
#define TOSTOP   0000400
#define IEXTEN   0100000

/* tcsetattr 의 optional_actions (Nyxis 에서는 모두 TCSAFLUSH 처럼: 읽지 않은 입력을 버린다) */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* 속도 (저장만 한다) */
#define B0       0
#define B9600    13
#define B38400   15
#define B115200  4098

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int optional_actions, const struct termios *t);
void cfmakeraw(struct termios *t);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int cfsetispeed(struct termios *t, speed_t s);
int cfsetospeed(struct termios *t, speed_t s);

#endif /* _NL_TERMIOS_H */
