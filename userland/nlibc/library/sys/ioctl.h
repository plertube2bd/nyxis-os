/*
 * sys/ioctl.h - 장치 제어 (터미널 창 크기 TIOCGWINSZ / TIOCSWINSZ 만)
 */
#ifndef _NL_SYS_IOCTL_H
#define _NL_SYS_IOCTL_H

struct winsize {
    unsigned short ws_row;      /* 글자 칸 행 수 */
    unsigned short ws_col;      /* 글자 칸 열 수 */
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413       /* arg = struct winsize *: 터미널 크기 (콘솔은 화면, 프레임버퍼가 없으면 24 x 80) */
#define TIOCSWINSZ 0x5414       /* arg = const struct winsize *: pty(주 끝 또는 종속 끝)의 크기를 정한다.
                                   포그라운드 그룹이 SIGWINCH 를 받는다. 물리 콘솔은 EINVAL */

/* 지원하지 않는 요청은 -1 + EINVAL, 터미널이 아닌 fd 는 -1 + ENOTTY */
int ioctl(int fd, unsigned long request, ...);

#endif /* _NL_SYS_IOCTL_H */
