/*
 * launch.c - 창 프로그램 띄우기
 *
 * 모두 따로 도는 프로세스다 (Wayland 의 클라이언트처럼): 환경 변수 NWP_DISPLAY(server.c 가 정한다)를 물려받아 ntui 에
 * 연결하고 자기 창을 연다. 명령은 터미널 창 프로그램 nterm 으로 띄운다 (nterm -e "명령").
 * ntui 가 연 fd(화면, 채널, 클라이언트 파이프, 버퍼)는 모두 close-on-exec 라 띄운 프로그램에게 가지 않는다.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>

#include "ntui.h"

extern char **environ;

int launch(const char *const *argv)
{
    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ);

    if (err != 0) {
        char msg[96];

        snprintf(msg, sizeof(msg), "cannot run %s: %s", argv[0], strerror(err));
        ntuiToast(msg);
        return -1;
    }
    return 0;
}

int launchCommand(const char *line, int flags, const char *title, int cols, int rows)
{
    const char *argv[12];
    char geom[24];
    char name[32];
    int n = 0;

    while (*line == ' ')
        line++;
    if (title == NULL) {
        size_t k = 0;

        while (line[k] != '\0' && line[k] != ' ' && k < sizeof(name) - 1) {
            name[k] = line[k];
            k++;
        }
        name[k] = '\0';
        title = name;
    }
    argv[n++] = "nterm";
    if (flags & LAUNCH_MAXIMIZED)
        argv[n++] = "-m";
    if (flags & LAUNCH_AUTOCLOSE)
        argv[n++] = "-x";
    if (cols > 0 && rows > 0) {
        snprintf(geom, sizeof(geom), "%dx%d", cols, rows);
        argv[n++] = "-g";
        argv[n++] = geom;
    }
    argv[n++] = "-t";
    argv[n++] = title;
    argv[n++] = "-e";
    argv[n++] = line;
    argv[n] = NULL;
    return launch(argv);
}

/* 명령의 첫 낱말이 자기 창을 여는 프로그램인지 (그런 것은 터미널 창 없이 띄운다) */
static int isWindowProgram(const char *line)
{
    static const char *const names[] = { "nterm", "nfiles", "nclock", "nkeys" };
    size_t n = strcspn(line, " ");
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) == n && strncmp(line, names[i], n) == 0)
            return 1;
    }
    return 0;
}

int launchAuto(const char *line, int flags)
{
    while (*line == ' ')
        line++;
    if (isWindowProgram(line)) {
        const char *argv[4];

        argv[0] = "nysh";               /* 인자만 풀어서 (따옴표 등) 띄운다: 창은 그 프로그램이 연다 */
        argv[1] = "-c";
        argv[2] = line;
        argv[3] = NULL;
        return launch(argv);
    }
    return launchCommand(line, flags, NULL, 0, 0);
}

int launchTerminal(const char *dir)
{
    const char *argv[4];

    argv[0] = "nterm";
    argv[1] = dir != NULL ? "-d" : NULL;
    argv[2] = dir;
    argv[3] = NULL;
    return launch(argv);
}

int launchApp(const char *name)
{
    const char *argv[2];

    argv[0] = name;
    argv[1] = NULL;
    return launch(argv);
}

void launchReap(void)
{
    int status;

    while (waitpid(-1, &status, WNOHANG) > 0)
        ;
}
