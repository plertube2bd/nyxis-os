/*
 * pwd - 현재 디렉터리 출력 (nlibc 가 들고 있는 값: 시작값은 환경 변수 PWD, 없으면 face:/)
 */
#include <stdio.h>
#include <unistd.h>
#include "common.h"

int main(void)
{
    char buf[CU_PATH_MAX];

    cu_prog = "pwd";
    if (!getcwd(buf, sizeof(buf))) {
        cu_warn("getcwd");
        return 1;
    }
    puts(buf);
    return 0;
}
