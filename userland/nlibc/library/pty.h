/*
 * pty.h - 의사 터미널 (BSD/glibc 의 openpty)
 *
 * openpty 는 커널의 NxPtyCreate 로 주 끝(master)과 종속 끝(slave)을 만든다.
 *   - 주 끝에 쓴 바이트는 키 입력이 된다 (종속 끝의 줄 편집기 또는 raw 모드). 주 끝을 읽으면 종속 끝에 쓴 출력과
 *     에코가 그대로 나온다: 터미널 에뮬레이터가 해석한다.
 *   - 종속 끝은 isatty 가 1 인 터미널이다 (tcsetattr, TIOCGWINSZ, tcsetpgrp, Ctrl-C = SIGINT).
 *     자식의 0/1/2 로 넘기면 (posix_spawn 의 dup2 등) 그 pty 가 자식의 제어 터미널(dev:/tty)이 된다.
 *   - 크기는 ioctl(master, TIOCSWINSZ, &ws). 바뀌면 포그라운드 그룹이 SIGWINCH 를 받는다.
 *   - 주 끝을 닫으면 종속 끝 쪽 프로세스는 SIGHUP 을 받고 읽기는 EOF. 종속 끝 쪽이 모두 끝나면 주 끝 읽기가 0 (EOF).
 * name 은 NULL 이어도 되고, 주면 "pty" 를 쓴다 (경로 이름이 없다). termp 는 ICANON 이 꺼져 있으면 raw 로 시작한다.
 * 성공 0, 실패 -1 + errno.
 */
#ifndef _NL_PTY_H
#define _NL_PTY_H

#include <termios.h>
#include <sys/ioctl.h>

int openpty(int *amaster, int *aslave, char *name, const struct termios *termp, const struct winsize *winp);

#endif /* _NL_PTY_H */
