/*
 * start.c - 프로그램 시작/종료: __nl_start, exit, atexit, abort, getenv, system, assert
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include "nl_internal.h"

int errno;
long __nl_start_ns;

/* main 은 (void), (int, char **), (int, char **, char **) 중 무엇으로 정의해도 된다:
 * System V ABI 에서 남는 인자는 무시된다. */
extern int main(int argc, char **argv, char **envp);
void __nl_start(int argc, char **argv, char **envp) __attribute__((noreturn));

void __nl_start(int argc, char **argv, char **envp)
{
    __nl_environ = envp;
    __nl_start_ns = __nl_clock_ns(NL_CLOCK_MONOTONIC);
    __nl_stdio_init();
    exit(main(argc, argv, envp));
}

/* C89 는 최소 32개를 요구한다 */
#define ATEXIT_MAX 32
static void (*atexit_fns[ATEXIT_MAX])(void);
static int atexit_count;

int atexit(void (*func)(void))
{
    if (atexit_count >= ATEXIT_MAX)
        return -1;
    atexit_fns[atexit_count++] = func;
    return 0;
}

void exit(int status)
{
    /* 처리기가 exit 를 다시 부르는 것은 정의되지 않은 동작이지만, 각 처리기를 한 번만 부르도록 먼저 꺼낸다 */
    while (atexit_count > 0)
        atexit_fns[--atexit_count]();
    __nl_stdio_flush_all();
    __nl_exit(status);
}

/* C89 7.10.4.1: SIGABRT 를 일으킨다. 처리기가 돌아오면(또는 무시하면) 그래도 끝낸다.
 * 종료 코드는 SIGABRT 관례(128 + 6). 스트림은 비우지 않는다. */
#define NL_EXIT_ABORT 134

void abort(void)
{
    static const char msg[] = "abort() called\n";

    (void)raise(SIGABRT);
    (void)__nl_write(2, msg, sizeof(msg) - 1);
    __nl_exit(NL_EXIT_ABORT);
}

void __nl_assert_fail(const char *expr, const char *file, int line)
{
    fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", expr, file, line);
    abort();
}
