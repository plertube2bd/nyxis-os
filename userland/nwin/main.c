/*
 * nwin - 지금 터미널 창(ntui 의 nterm)을 다룬다 (nx64api 의 nx64_win_* 예제이자 셸 명령)
 *
 *   nwin                      창 정보 (위치, 크기, 상태, 창 영역)
 *   nwin size 열x행           안쪽 크기 (예: nwin size 100x30, 한쪽만: 100x0)
 *   nwin move X Y             창의 왼쪽 위를 화면 칸 X, Y 로
 *   nwin center               창 영역의 가운데로
 *   nwin normal|max|full|min  상태
 *   nwin title 제목           창 제목
 *   nwin demo                 TUI 예제: 방향키로 옮기기, +/- 로 크기, m/f/n 상태, c 가운데, q 끝.
 *                             끝나면 창이 처음 모양으로 돌아간다 (nx64 가 되돌린다)
 * 명령으로 바꾼 것은 그대로 남긴다 (nx64_win_keep). ntui 의 터미널 창 밖에서는 실패한다 (종료 코드 1).
 */
#include <nx64.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *state_name(int s)
{
    switch (s) {
    case NX64_WIN_MAXIMIZED:  return "maximized";
    case NX64_WIN_FULLSCREEN: return "fullscreen";
    case NX64_WIN_MINIMIZED:  return "minimized";
    default:                  return "normal";
    }
}

static int fail(const char *what)
{
    fprintf(stderr, "nwin: %s: %s\n", what, strerror(errno));
    return 1;
}

/* ---------------- demo ---------------- */

struct demo {
    struct nx64_win w;
    int have;
    char msg[64];
};

static void demo_draw(void *ctx)
{
    struct demo *d = (struct demo *)ctx;
    int cols = nx64_tui_cols();
    int rows = nx64_tui_rows();

    d->have = nx64_win_info(&d->w) == 0;
    nx64_tui_clear(NX64_DEFAULT);
    nx64_tui_box(0, 0, cols, rows, "nwin demo", NX64_FRAME);
    nx64_tui_textf(2, 2, NX64_ACCENT, " %d x %d ", cols, rows);
    if (d->have) {
        nx64_tui_textf(2, 4, NX64_DEFAULT, "window at %d,%d  %s", d->w.x, d->w.y, state_name(d->w.state));
        nx64_tui_textf(2, 5, NX64_DIM, "area %d x %d", d->w.area_cols, d->w.area_rows);
    }
    nx64_tui_text(2, 7, "arrows move   + - size   c center", NX64_DEFAULT);
    nx64_tui_text(2, 8, "m max  f full  n normal   q quit", NX64_DEFAULT);
    nx64_tui_text(2, rows - 2, d->msg, NX64_DIM);
}

static int demo_key(void *ctx, int k)
{
    struct demo *d = (struct demo *)ctx;
    int x = d->have ? d->w.x : 0;
    int y = d->have ? d->w.y : 0;
    int r = 0;

    d->msg[0] = '\0';
    switch (k) {
    case 'q': case NX64_KEY_ESC:
        return 0;
    case NX64_KEY_LEFT:  r = nx64_win_move(x > 2 ? x - 2 : 0, y); break;
    case NX64_KEY_RIGHT: r = nx64_win_move(x + 2, y); break;
    case NX64_KEY_UP:    r = nx64_win_move(x, y > 1 ? y - 1 : 0); break;
    case NX64_KEY_DOWN:  r = nx64_win_move(x, y + 1); break;
    case '+':            r = nx64_win_resize(nx64_tui_cols() + 4, nx64_tui_rows() + 2); break;
    case '-':            r = nx64_win_resize(nx64_tui_cols() > 40 ? nx64_tui_cols() - 4 : 36,
                                             nx64_tui_rows() > 12 ? nx64_tui_rows() - 2 : 10); break;
    case 'c':            r = nx64_win_center(); break;
    case 'm':            r = nx64_win_set_state(NX64_WIN_MAXIMIZED); break;
    case 'f':            r = nx64_win_set_state(NX64_WIN_FULLSCREEN); break;
    case 'n':            r = nx64_win_set_state(NX64_WIN_NORMAL); break;
    default:
        break;
    }
    if (r < 0)
        snprintf(d->msg, sizeof(d->msg), "failed: %s", strerror(errno));
    return 1;
}

static int demo(void)
{
    struct demo d;
    struct nx64_app app = { demo_draw, demo_key, NULL, 0 };

    nx64_ntui_require("nwin demo");     /* ntui 의 터미널 창이 아니면 여기서 끝난다 */
    memset(&d, 0, sizeof(d));
    (void)nx64_win_title("nwin demo");
    return nx64_tui_run(&app, &d) < 0 ? 1 : 0;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "info";
    struct nx64_win w;

    if (strcmp(cmd, "demo") == 0)
        return demo();
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        printf("usage: nwin [size COLSxROWS | move X Y | center | normal | max | full | min | title TEXT | demo]\n");
        return 0;
    }
    if (!nx64_ntui_present()) {
        fprintf(stderr, "nwin: not in an ntui terminal window\n");
        return 1;
    }
    nx64_win_keep();                    /* 셸 명령: 바꾼 것을 남긴다 */

    if (strcmp(cmd, "info") == 0) {
        if (nx64_win_info(&w) < 0)
            return fail("info");
        printf("position %d,%d\nsize %dx%d\nstate %s\narea %dx%d\n", w.x, w.y, w.cols, w.rows,
               state_name(w.state), w.area_cols, w.area_rows);
        return 0;
    }
    if (strcmp(cmd, "size") == 0 && argc == 3) {
        int cols;
        int rows;

        if (sscanf(argv[2], "%dx%d", &cols, &rows) != 2 || cols < 0 || rows < 0) {
            fprintf(stderr, "nwin: size COLSxROWS (0 keeps that side)\n");
            return 2;
        }
        return nx64_win_resize(cols, rows) < 0 ? fail("size") : 0;
    }
    if (strcmp(cmd, "move") == 0 && argc == 4)
        return nx64_win_move(atoi(argv[2]), atoi(argv[3])) < 0 ? fail("move") : 0;
    if (strcmp(cmd, "center") == 0)
        return nx64_win_center() < 0 ? fail("center") : 0;
    if (strcmp(cmd, "normal") == 0)
        return nx64_win_set_state(NX64_WIN_NORMAL) < 0 ? fail("normal") : 0;
    if (strcmp(cmd, "max") == 0)
        return nx64_win_set_state(NX64_WIN_MAXIMIZED) < 0 ? fail("max") : 0;
    if (strcmp(cmd, "full") == 0)
        return nx64_win_set_state(NX64_WIN_FULLSCREEN) < 0 ? fail("full") : 0;
    if (strcmp(cmd, "min") == 0)
        return nx64_win_set_state(NX64_WIN_MINIMIZED) < 0 ? fail("min") : 0;
    if (strcmp(cmd, "title") == 0 && argc == 3)
        return nx64_win_title(argv[2]) < 0 ? fail("title") : 0;
    fprintf(stderr, "nwin: unknown command (nwin -h)\n");
    return 2;
}
