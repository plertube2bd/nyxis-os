#include "stdio.h"

void helloworld_main(void);

void helloworld_main(void)
{
    printk("hello, world. nyxis os!\n");

    /*
     * 유저 모드에서는 hlt 가 특권 명령이라 #GP 가 발생한다.
     * (예전 코드는 hlt 루프였다) 종료 시스템 콜이 구현될 때까지 pause 로 대기한다.
     */
    for (;;) {
        __asm__ volatile ("pause");
    }
}
