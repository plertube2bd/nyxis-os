/*
 * sys/ioctl.h - 장치 제어 (Nyxis 콘솔의 TIOCGWINSZ 만)
 */
#ifndef _NL_SYS_IOCTL_H
#define _NL_SYS_IOCTL_H

struct winsize {
    unsigned short ws_row;      /* 글자 칸 행 수 */
    unsigned short ws_col;      /* 글자 칸 열 수 */
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413       /* arg = struct winsize *: 콘솔 크기 (프레임버퍼가 없으면 24 x 80) */

/* 지원하지 않는 요청은 -1 + EINVAL, 터미널이 아닌 fd 는 -1 + ENOTTY */
int ioctl(int fd, unsigned long request, ...);

#endif /* _NL_SYS_IOCTL_H */
