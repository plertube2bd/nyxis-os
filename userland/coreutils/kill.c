/*
 * kill [-s 신호 | -신호] pid... - 프로세스에 신호를 보낸다 (기본 SIGTERM)
 * kill -l                       - 신호 이름 목록
 *
 * 신호는 이름(TERM, SIGTERM, KILL, INT, HUP ...)이나 번호로. pid 는 핸들 없이 그대로 (nx64_signal_pid =
 * NxSignalSendPid): 자기 프로세스에만 보낼 수 있고 root 는 모두. 보호 프로세스(init, login)는 root 도 안 된다.
 * 하나라도 실패하면 종료 코드 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <nx64.h>
#include "common.h"

static const struct {
    const char *name;
    int sig;
} g_sigs[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "KILL", SIGKILL }, { "USR1", SIGUSR1 },
    { "USR2", SIGUSR2 }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "CHLD", SIGCHLD },
    { "CONT", SIGCONT }, { "STOP", SIGSTOP }, { "WINCH", SIGWINCH },
};
#define SIG_COUNT ((int)(sizeof(g_sigs) / sizeof(g_sigs[0])))

/* 이름이나 번호 -> 신호. 모르면 -1 */
static int parseSig(const char *s)
{
    char *end;
    long v;
    int i;

    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    for (i = 0; i < SIG_COUNT; i++) {
        if (strcmp(s, g_sigs[i].name) == 0)
            return g_sigs[i].sig;
    }
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 31)
        return -1;
    return (int)v;
}

int main(int argc, char **argv)
{
    int sig = SIGTERM;
    int i = 1;
    int rc = 0;

    cu_prog = "kill";
    if (argc == 2 && strcmp(argv[1], "-l") == 0) {
        for (i = 0; i < SIG_COUNT; i++)
            printf("%2d %s\n", g_sigs[i].sig, g_sigs[i].name);
        return 0;
    }
    if (i < argc && strcmp(argv[i], "-s") == 0) {
        if (i + 1 >= argc || (sig = parseSig(argv[i + 1])) < 0) {
            cu_msg("unknown signal '%s'", i + 1 < argc ? argv[i + 1] : "");
            return 2;
        }
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] != '\0' && strcmp(argv[i], "--") != 0) {
        if ((sig = parseSig(argv[i] + 1)) < 0) {
            cu_msg("unknown signal '%s'", argv[i] + 1);
            return 2;
        }
        i++;
    }
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        cu_msg("usage: kill [-s SIGNAL | -SIGNAL] pid...   kill -l");
        return 2;
    }
    for (; i < argc; i++) {
        char *end;
        unsigned long pid = strtoul(argv[i], &end, 10);

        if (end == argv[i] || *end != '\0') {
            cu_msg("'%s': not a process id", argv[i]);
            rc = 1;
            continue;
        }
        if (nx64_signal_pid(pid, sig) < 0) {
            cu_msg("%lu: %s", pid, strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
