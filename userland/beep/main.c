/*
 * beep - PC 스피커로 사각파 비프음 (nx64api 의 nx64_beep / nx64_tone 예제)
 *
 *   beep                       440 Hz, 200 ms
 *   beep -f 880 -l 100         주파수(Hz), 길이(ms)
 *   beep -f 523 -l 120 -r 3 -d 80     3 번, 사이 80 ms 쉼
 *   beep -n -f 659 -l 120 -n -f 784 -l 240     -n: 다음 음 (각 음은 앞의 설정을 이어받는다)
 *   beep -c 523,659,784 -l 600 화음 (4음까지. 커널이 10ms 마다 음을 바꾸는 아르페지오로 낸다)
 *   beep -s                    지금 나는 소리를 끈다
 *   길이와 쉼은 0..60000 ms
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <nx64.h>

struct note {
    unsigned int chord[NX64_CHORD_MAX];
    int nchord;                         /* 0 = 화음 아님 (hz 하나) */
    unsigned long hz;
    unsigned long ms;
    unsigned long reps;
    unsigned long delay;
};

static int num(const char *s, unsigned long *out)
{
    char *end;

    errno = 0;
    *out = strtoul(s, &end, 10);
    return (errno || end == s || *end != '\0') ? -1 : 0;
}

/* "523,659,784" -> n->chord. 성공 0 */
static int parse_chord(const char *s, struct note *n)
{
    n->nchord = 0;
    while (*s) {
        char *end;
        unsigned long v;

        if (n->nchord == NX64_CHORD_MAX)
            return -1;
        errno = 0;
        v = strtoul(s, &end, 10);
        if (errno || end == s || v > NX64_TONE_MAX_HZ || (*end != ',' && *end != '\0'))
            return -1;
        n->chord[n->nchord++] = (unsigned int)v;
        s = *end == ',' ? end + 1 : end;
    }
    return n->nchord > 0 ? 0 : -1;
}

/* 화음을 ms 동안 내고 기다린다 */
static int chord_beep(const struct note *n)
{
    if (nx64_chord(n->chord, n->nchord, (unsigned)n->ms) < 0)
        return -1;
    (void)nx64_beep(0, (unsigned)n->ms);     /* 쉼 = 기다리기 */
    return nx64_tone_off();
}

static int play(const struct note *n)
{
    unsigned long r;

    for (r = 0; r < n->reps; r++) {
        if ((n->nchord ? chord_beep(n) : nx64_beep((unsigned)n->hz, (unsigned)n->ms)) < 0) {
            fprintf(stderr, "beep: %lu Hz %lu ms: %s\n", n->hz, n->ms, strerror(errno));
            return -1;
        }
        if (r + 1 < n->reps && n->delay > 0)
            (void)nx64_beep(0, (unsigned)n->delay);      /* hz 0 = 쉼 */
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct note n;
    int i;

    n.nchord = 0;
    n.hz = 440;
    n.ms = 200;
    n.reps = 1;
    n.delay = 100;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        unsigned long *dst = NULL;

        if (strcmp(a, "-s") == 0)
            return nx64_tone_off() < 0 ? 1 : 0;
        if (strcmp(a, "-n") == 0 || strcmp(a, "--new") == 0) {
            if (play(&n) < 0)
                return 1;
            continue;
        }
        if (strcmp(a, "-c") == 0) {
            if (i + 1 >= argc || parse_chord(argv[i + 1], &n) < 0) {
                fprintf(stderr, "beep: -c: up to %d frequencies separated by commas (e.g. 523,659,784)\n",
                        NX64_CHORD_MAX);
                return 2;
            }
            i++;
            continue;
        }
        if (strcmp(a, "-f") == 0) {
            n.nchord = 0;
            dst = &n.hz;
        } else if (strcmp(a, "-l") == 0)
            dst = &n.ms;
        else if (strcmp(a, "-r") == 0)
            dst = &n.reps;
        else if (strcmp(a, "-d") == 0 || strcmp(a, "-D") == 0)
            dst = &n.delay;
        if (!dst || i + 1 >= argc || num(argv[i + 1], dst) < 0 || n.ms > NX64_TONE_MAX_MS ||
            n.delay > NX64_TONE_MAX_MS) {
            fprintf(stderr, "usage: beep [-f hz | -c hz,hz,...] [-l ms] [-r reps] [-d delay_ms] [-n ...] | beep -s\n"
                            "  hz %u..%u, ms up to %u\n", NX64_TONE_MIN_HZ, NX64_TONE_MAX_HZ, NX64_TONE_MAX_MS);
            return 2;
        }
        i++;
    }
    return play(&n) < 0 ? 1 : 0;
}
