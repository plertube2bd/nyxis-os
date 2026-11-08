/*
 * clear - 터미널 화면을 지우고 커서를 맨 위 왼쪽으로 옮긴다
 *
 * ANSI 시퀀스 ESC[H (커서 1행 1열) + ESC[2J (화면 전체 지우기) 를 표준 출력으로 쓴다.
 * Nyxis 콘솔은 커널의 터미널 해석기(nxkernel/console/outputs/ansi.h)가 이를 처리한다.
 */
#include <stdio.h>
#include "common.h"

int main(int argc, char **argv)
{
    (void)argv;
    cu_prog = "clear";
    if (argc > 1) {
        fputs("usage: clear\n", stderr);
        return 2;
    }
    fputs("\033[H\033[2J", stdout);
    return (fflush(stdout) == 0) ? 0 : 1;
}
