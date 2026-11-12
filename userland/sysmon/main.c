/*
 * sysmon - 시스템 모니터 (nx64api 예제: <nx64.h> 의 TUI 와 시스템 부분만으로 만든다)
 *
 *   sysmon          전체 화면. Nyxis OS 데스크톱에서는 메뉴의 System monitor 나 Super+R 로 "sysmon" 을 실행 (터미널 창)
 *   q / Esc         끝
 *
 * 보이는 것: 사용자@호스트, OS/ABI, 업타임(1초마다), 시간대, 메모리 막대 셋:
 *   Used       지금 방식: (RAM - 남음) / 물리 RAM 전체 (할당기 밖의 커널/initrd/tmp:/ 포함)
 *   Reserved   할당기 밖의 RAM (커널 이미지, initrd, 펌웨어) / 물리 RAM 전체
 *   Allocator  옛 Nyxis 방식: (관리 - 남음) / 할당기(pfa)가 관리하는 RAM
 * 설정 (nx64_setting_get: 사용자 값 roi:/<uid>/app/sysmon/<키>, 기본값 app:/sysmon.run/<키>):
 *   refresh_ms   다시 그리는 간격 (기본 1000, 100~10000)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nx64.h>

struct state {
    struct nx64_identity me;
    char host[64];
    char version[32];
    char abi[32];
    char zone[64];
    unsigned long last_second;
};

static void fmt_kib(unsigned long kib, char *out, size_t cap)
{
    if (kib >= 10UL * 1024UL)
        snprintf(out, cap, "%lu MiB", kib / 1024UL);
    else
        snprintf(out, cap, "%lu KiB", kib);
}

static void draw(void *ctx)
{
    struct state *st = ctx;
    struct nx64_memory m;
    unsigned long up = nx64_uptime_ms() / 1000UL;
    int cols = nx64_tui_cols();
    int w = cols - 4 < 72 ? cols - 4 : 72;
    int x = (cols - w) / 2;
    int y = 2;
    char a[32], b[32], c[32];
    unsigned long pct;

    nx64_tui_clear(NX64_DEFAULT);
    nx64_tui_box(x, y, w, 19, "sysmon", NX64_FRAME);
    nx64_tui_textf(x + 2, y + 2, NX64_ACCENT, " %s@%s ", st->me.name, st->host);
    nx64_tui_textf(x + 2, y + 4, NX64_DEFAULT, "OS        Nyxis %s (ABI %s)", st->version, st->abi);
    nx64_tui_textf(x + 2, y + 5, NX64_DEFAULT, "Uptime    %lud %02lu:%02lu:%02lu",
                   up / 86400UL, up / 3600UL % 24UL, up / 60UL % 60UL, up % 60UL);
    nx64_tui_textf(x + 2, y + 6, NX64_DEFAULT, "Zone      %s", st->zone);

    if (nx64_memory(&m) == 0 && m.ram_kib > 0) {
        int bw = w - 4;

        fmt_kib(m.used_kib, a, sizeof(a));
        fmt_kib(m.ram_kib, b, sizeof(b));
        fmt_kib(m.reserved_kib, c, sizeof(c));
        pct = m.used_kib * 100UL / m.ram_kib;
        nx64_tui_textf(x + 2, y + 8, NX64_DEFAULT, "Used      %s / %s (%lu%%)", a, b, pct);
        nx64_tui_bar(x + 2, y + 9, bw, m.used_kib, m.ram_kib, nx64_style_make(231, 54, 0),
                     nx64_style_make(250, 236, 0));
        nx64_tui_textf(x + 2, y + 11, NX64_DEFAULT, "Reserved  %s", c);
        nx64_tui_text(x + 22, y + 11, "(kernel, initrd, firmware)", NX64_DIM);
        nx64_tui_bar(x + 2, y + 12, bw, m.reserved_kib, m.ram_kib, nx64_style_make(231, 23, 0),
                     nx64_style_make(250, 236, 0));
        if (m.managed_kib > 0) {
            fmt_kib(m.alloc_kib, a, sizeof(a));
            fmt_kib(m.managed_kib, b, sizeof(b));
            pct = m.alloc_kib * 100UL / m.managed_kib;
            nx64_tui_textf(x + 2, y + 14, NX64_DEFAULT, "Allocator %s / %s (%lu%%)", a, b, pct);
            nx64_tui_bar(x + 2, y + 15, bw, m.alloc_kib, m.managed_kib, nx64_style_make(231, 28, 0),
                         nx64_style_make(250, 236, 0));
        }
    } else {
        nx64_tui_text(x + 2, y + 8, "Memory    (unavailable)", NX64_DIM);
    }
    nx64_tui_text(x + 2, y + 17, "q: quit", NX64_DIM);
}

static int key(void *ctx, int k)
{
    (void)ctx;
    return !(k == 'q' || k == 'Q' || k == NX64_KEY_ESC || k == NX64_KEY_CTRL('c'));
}

/* 초가 바뀌었으면 다시 그린다 */
static int tick(void *ctx)
{
    struct state *st = ctx;
    unsigned long s = nx64_uptime_ms() / 1000UL;

    if (s == st->last_second)
        return 0;
    st->last_second = s;
    return 1;
}

int main(int argc, char **argv)
{
    struct state st;
    struct nx64_app app;
    char v[16];
    long refresh = 1000;

    (void)argv;
    if (argc > 1) {
        fputs("usage: sysmon\n", stderr);
        return 2;
    }
    memset(&st, 0, sizeof(st));
    if (nx64_whoami(&st.me) < 0)
        strcpy(st.me.name, "?");
    if (nx64_info("hostname", st.host, sizeof(st.host)) < 0)
        strcpy(st.host, "nyxis");
    if (nx64_info("version", st.version, sizeof(st.version)) < 0)
        strcpy(st.version, "?");
    if (nx64_info("abi", st.abi, sizeof(st.abi)) < 0)
        strcpy(st.abi, "?");
    if (nx64_info("timezone", st.zone, sizeof(st.zone)) < 0)
        strcpy(st.zone, "UTC");
    if (nx64_setting_get("sysmon", "refresh_ms", v, sizeof(v)) > 0) {
        refresh = strtol(v, NULL, 10);
        if (refresh < 100 || refresh > 10000)
            refresh = 1000;
    }

    app.draw = draw;
    app.key = key;
    app.tick = tick;
    app.tick_ms = (int)refresh;
    if (nx64_tui_run(&app, &st) < 0) {
        fputs("sysmon: no terminal\n", stderr);
        return 1;
    }
    return 0;
}
