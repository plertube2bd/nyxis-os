/*
 * hello.c - nlibc 예제 (app:/hello.run). SDK 드라이버로 빌드한다:
 *
 *   out/nlibc/bin/nlcc -O2 -o hello.run hello.c                  (한 번에)
 *   out/nlibc/bin/nlcc -O2 -c hello.c && out/nlibc/bin/nlld -o hello.run hello.o   (따로)
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

int main(int argc, char **argv)
{
    time_t now = time(NULL);
    char when[64];

    printf("Hello from nlibc!\n");                      /* gcc 가 puts 로 바꾼다 (hosted 모드) */
    printf("argv[0] = %s, argc = %d\n", argv[0], argc);
    printf("sqrt(2) = %.15f, pi = %.10f\n", sqrt(2.0), 4.0 * atan(1.0));
    if (now != (time_t)-1 && strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", gmtime(&now)))
        printf("now: %s\n", when);
    else
        printf("now: unknown (no real-time clock)\n");
    return EXIT_SUCCESS;
}
