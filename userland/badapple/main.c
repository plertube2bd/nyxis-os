/*
 * badapple - 흑백 아스키 아트 + 사각파 비프음 동영상 플레이어 (Bad Apple!! 재생용)
 *
 *   badapple [-m] [-c] [-f] [파일.mp4]
 *     -m   소리 없이 (PC 스피커를 쓰지 않는다)
 *     -c   화음 (아르페지오). 기본은 단음: PC 스피커는 음량을 나눌 수 없어 화음의 음이 모두 같은 크기로 들린다
 *     -f   ntui 창을 전체 화면으로
 *     파일 기본값: 설정 movie (app:/badapple.run/badapple.mp4)
 *
 *   재생 중 키: q / Esc / Ctrl-C 끝, Space 멈춤/계속, m 소리 끄기/켜기, c 단음/화음 바꾸기
 *   설정 sound: mono (기본) / chord
 *
 * 파일은 보통의 MP4 컨테이너이고, 안의 트랙은 Nyxis 자체 형식이다 (nxbw.h, docs/BADAPPLE.md):
 *   비디오 'nxbw'  80x60, 픽셀당 4비트(16단계 흑백), 프레임마다 키/XOR 델타 + deflate (uzlib 로 푼다)
 *   음     'nxsq'  프레임마다 사각파 목소리 (선율, 화성음, 베이스).
 *                  단음(기본): 가장 두드러진 음 하나 = 선율, 선율이 쉬면 베이스 -> nx64_tone
 *                  화음(-c): 모두 -> nx64_chord (스피커는 한 번에 한 음이라 커널이 10ms 마다 음을 바꾸는 아르페지오)
 * 화면은 80x60 픽셀, 흰 글자 / 검은 바탕. 밝기 16단계를 글자 16개(설정 ramp)로 그린다. 글자 칸이 1:2 라서
 * 픽셀 하나를 가로 글자 2개로 그린다: 160x60 칸 = 4:3. 터미널이 작으면 4:3 을 지키며 줄인다
 * (ntui 에서는 창을 160x60 으로 맞춰 달라고 하고, 안 들어가면 전체 화면).
 *
 * 시각은 단조 시계로 맞춘다: 늦으면 그리지 않고 풀기만 해서 따라잡는다 (소리와 화면이 어긋나지 않게).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <nx64.h>

#include "uzlib.h"
#include "mp4.h"
#include "nxbw.h"

#define APP_NAME     "badapple"
#define DISP_W       (NXBW_W * 2)   /* 화면 칸: 픽셀 하나 = 가로 글자 2개 (글자 칸이 1:2 라서). 160x60 = 4:3 */
#define DISP_H       NXBW_H
#define TONE_HOLD_MS 250U       /* 소리를 프레임마다 이만큼 늘린다: 플레이어가 멈추면 커널이 곧 끈다 */
                                /* nxsq 샘플: version 1 = u16 하나, version 2 = u16 여러 개 (화음) */

struct player {
    struct mp4_file mp4;
    struct mp4_track *video;
    struct mp4_track *audio;
    unsigned char frame[NXBW_FRAME_BYTES];
    unsigned char delta[NXBW_FRAME_BYTES + 1];
    char ramp[NXBW_LEVELS];
    nx64_style style;
    int muted;
    int chord;                  /* 0 = 단음 (기본), 1 = 화음 */
    unsigned last_hz[NX64_CHORD_MAX];
    int last_n;
    unsigned long frames_shown;
    unsigned long frames_dropped;
};

static long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    size_t got = 0;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        errno = EINVAL;
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        errno = ENOMEM;
        return NULL;
    }
    while (got < (size_t)n) {
        size_t r = fread(buf + got, 1, (size_t)n - got, f);

        if (r == 0)
            break;
        got += r;
    }
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        errno = EIO;
        return NULL;
    }
    *len = got;
    return buf;
}

/* zlib 스트림 [z, z+zlen) 을 out 에 정확히 NXBW_FRAME_BYTES 로 푼다 (uzlib, adler32 확인). 성공 0 */
static int inflate_frame(const unsigned char *z, size_t zlen, unsigned char *out)
{
    struct uzlib_uncomp d;
    int r;

    uzlib_uncompress_init(&d, NULL, 0);     /* 사전 = 출력 버퍼 자체 (프레임 전체를 한 번에 푼다) */
    d.source = z;
    d.source_limit = z + zlen;
    d.source_read_cb = NULL;
    if (uzlib_zlib_parse_header(&d) < 0)
        return -1;
    d.dest_start = d.dest = out;
    d.dest_limit = out + NXBW_FRAME_BYTES;
    r = uzlib_uncompress_chksum(&d);
    if (r == TINF_OK && d.dest == d.dest_limit) {
        d.dest_limit = out + NXBW_FRAME_BYTES + 1;     /* 블록 끝과 adler32 까지 읽게 (더 나오면 오류) */
        r = uzlib_uncompress_chksum(&d);
    }
    if (r != TINF_DONE || d.dest != out + NXBW_FRAME_BYTES)
        return -1;
    return 0;
}

/* 샘플 i 를 풀어 p->frame 을 그 프레임으로. 성공 0 */
static int decode_frame(struct player *p, unsigned long i)
{
    const unsigned char *s = p->mp4.data + p->video->offset[i];
    unsigned long len = p->video->size[i];
    int k;

    if (len < 1)
        return -1;
    switch (s[0]) {
    case NXBW_KEY:
        return inflate_frame(s + 1, len - 1, p->frame);
    case NXBW_DELTA:
        if (inflate_frame(s + 1, len - 1, p->delta) < 0)
            return -1;
        for (k = 0; k < NXBW_FRAME_BYTES; k++)
            p->frame[k] ^= p->delta[k];
        return 0;
    case NXBW_SAME:
        return 0;
    default:
        return -1;
    }
}

/* 샘플 i 의 화음 (u16 큰 끝 주파수들, 0 과 범위 밖은 뺀다). 음 수를 돌려준다 */
static int audio_chord(const struct player *p, unsigned long i, unsigned *hz)
{
    const unsigned char *s;
    unsigned long nv, k;
    int n = 0;

    if (!p->audio || i >= p->audio->nsamples)
        return 0;
    s = p->mp4.data + p->audio->offset[i];
    nv = p->audio->size[i] / 2;
    if (nv > NX64_CHORD_MAX)
        nv = NX64_CHORD_MAX;
    for (k = 0; k < nv; k++) {
        unsigned h = mp4_be16(s + 2 * k);

        if (h >= NX64_TONE_MIN_HZ && h <= NX64_TONE_MAX_HZ)
            hz[n++] = h;
    }
    return n;
}

/* 화음을 PC 스피커로 (커널이 10ms 아르페지오로 낸다). 같은 화음이면 길이만 늘린다 */
static void set_chord(struct player *p, const unsigned *hz, int n)
{
    if (p->muted)
        n = 0;
    if (n == 0) {
        if (p->last_n != 0)
            (void)nx64_tone_off();
    } else {
        (void)nx64_chord(hz, n, TONE_HOLD_MS);
    }
    memcpy(p->last_hz, hz, sizeof(unsigned) * (size_t)n);
    p->last_n = n;
}

/*
 * 지금 프레임을 그린다. 글자 칸은 가로 1 : 세로 2 (콘솔 글꼴 8x16) 라서 픽셀 하나를 가로 글자 2개로 그린다:
 * 80x60 픽셀 -> 160x60 칸 = 1280x960 화소 = 영상과 같은 4:3. 터미널이 작으면 그 비율대로 줄인다.
 */
static void draw(struct player *p, int paused)
{
    int cols = nx64_tui_cols();
    int rows = nx64_tui_rows();
    int w = DISP_W, h = DISP_H;
    int ox, oy;
    int x, y;

    if (cols < w || rows < h) {
        /* 칸 비율 DISP_W : DISP_H (= 화면 4:3) 을 지키며 들어가는 가장 큰 크기 */
        if ((long)cols * DISP_H <= (long)rows * DISP_W) {
            w = cols;
            h = (int)((long)cols * DISP_H / DISP_W);
        } else {
            h = rows;
            w = (int)((long)rows * DISP_W / DISP_H);
        }
        if (w < 1)
            w = 1;
        if (h < 1)
            h = 1;
    }
    ox = (cols - w) / 2;
    oy = (rows - h) / 2;

    if (ox > 0 || oy > 0)
        nx64_tui_clear(p->style);
    for (y = 0; y < h; y++) {
        int sy = y * NXBW_H / h;

        for (x = 0; x < w; x++) {
            int sx = x * NXBW_W / w;

            nx64_tui_put(ox + x, oy + y, p->ramp[NXBW_PIXEL(p->frame, sx, sy)], p->style);
        }
    }
    if (paused)
        nx64_tui_text(ox, oy + h - 1, " PAUSED - space: play, q: quit ",
                      nx64_style_make(NX64_BLACK, NX64_BRIGHT_WHITE, 0));
}

static int open_movie(struct player *p, const char *path)
{
    unsigned char *data;
    size_t len = 0;
    const char *why = "?";
    const unsigned char *conf;
    size_t conf_len = 0;

    data = load_file(path, &len);
    if (!data) {
        fprintf(stderr, APP_NAME ": %s: %s\n", path, strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr, APP_NAME ": the movie is built on the host: make badapple-movie (docs/BADAPPLE.md)\n");
        return -1;
    }
    if (mp4_parse(&p->mp4, data, len, &why) < 0) {
        fprintf(stderr, APP_NAME ": %s: %s\n", path, why);
        free(data);
        return -1;
    }
    p->video = mp4_find(&p->mp4, NXBW_FOURCC('v', 'i', 'd', 'e'));
    p->audio = mp4_find(&p->mp4, NXBW_FOURCC('s', 'o', 'u', 'n'));
    if (!p->video || p->video->codec != NXBW_VIDEO_FOURCC) {
        fprintf(stderr, APP_NAME ": %s: no 'nxbw' video track (this player plays only Nyxis nxbw movies)\n", path);
        return -1;
    }
    conf = mp4_entry_child(p->video, 78, NXBW_VCONF_FOURCC, &conf_len);
    if (!conf || conf_len < 12 || conf[0] != 1 || conf[1] != NXBW_BPP || conf[2] != NXBW_LEVELS ||
        conf[3] != 'z' || mp4_be16(conf + 4) != NXBW_W || mp4_be16(conf + 6) != NXBW_H) {
        fprintf(stderr, APP_NAME ": %s: unsupported nxbw configuration\n", path);
        return -1;
    }
    if (p->audio && p->audio->codec != NXBW_AUDIO_FOURCC)
        p->audio = NULL;                /* 다른 소리 트랙은 무시한다 */
    if (p->video->timescale == 0 || p->video->sample_delta == 0) {
        fprintf(stderr, APP_NAME ": %s: bad frame rate\n", path);
        return -1;
    }
    return 0;
}

/* ntui 창이면 160x60 칸으로 (안 들어가면 전체 화면) */
static void fit_window(int fullscreen)
{
    struct nx64_win w;

    if (!nx64_ntui_present())
        return;
    (void)nx64_win_title("Bad Apple!!");
    if (!fullscreen && nx64_win_info(&w) == 0 && (w.area_cols < DISP_W + 2 || w.area_rows < DISP_H + 2))
        fullscreen = 1;                 /* 창 영역에 테두리까지 들어가지 않는다 */
    if (fullscreen) {
        (void)nx64_win_set_state(NX64_WIN_FULLSCREEN);
    } else {
        (void)nx64_win_resize(DISP_W, DISP_H);
        (void)nx64_win_center();
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: " APP_NAME " [-m] [-c] [-f] [movie.mp4]\n"
                    "  -m  no sound    -c  chords (arpeggio) instead of one note    -f  fullscreen window\n"
                    "  keys: q quit, space pause, m mute, c mono/chords\n");
}

int main(int argc, char **argv)
{
    static struct player pl;
    struct player *p = &pl;
    char path[256];
    char ramp[64];
    char sound[16];
    const char *file = NULL;
    int fullscreen = 0;
    int i;
    unsigned long n, cur = 0;
    long start, paused_at = 0;
    int paused = 0, quit = 0, bad = 0;
    unsigned long ts, delta;

    if (nx64_setting_get(APP_NAME, "sound", sound, sizeof(sound)) > 0 && strcmp(sound, "chord") == 0)
        p->chord = 1;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0)
            p->muted = 1;
        else if (strcmp(argv[i], "-c") == 0)
            p->chord = 1;
        else if (strcmp(argv[i], "-f") == 0)
            fullscreen = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else
            file = argv[i];
    }
    if (!file) {
        if (nx64_setting_get(APP_NAME, "movie", path, sizeof(path)) <= 0)
            strcpy(path, "app:/badapple.run/badapple.mp4");
        file = path;
    }
    memcpy(p->ramp, " .,:;-=+*ox%#&@M", NXBW_LEVELS);
    if (nx64_setting_get(APP_NAME, "ramp", ramp, sizeof(ramp)) >= NXBW_LEVELS)
        memcpy(p->ramp, ramp, NXBW_LEVELS);
    for (i = 0; i < NXBW_LEVELS; i++)
        if (p->ramp[i] < 32 || p->ramp[i] > 126)
            p->ramp[i] = '?';
    p->style = nx64_style_make(NX64_BRIGHT_WHITE, NX64_BLACK, 0);

    uzlib_init();
    if (open_movie(p, file) < 0)
        return 1;
    n = p->video->nsamples;
    ts = p->video->timescale;
    delta = p->video->sample_delta;

    if (decode_frame(p, 0) < 0) {
        fprintf(stderr, APP_NAME ": %s: frame 0 is damaged\n", file);
        return 1;
    }
    fit_window(fullscreen);
    if (nx64_tui_begin() < 0) {
        fprintf(stderr, APP_NAME ": needs a terminal\n");
        return 1;
    }

    start = now_ms();
    while (!quit && cur < n) {
        long t = (paused ? paused_at : now_ms()) - start;
        unsigned long due = t <= 0 ? 0 : (unsigned long)((double)t * ts / (1000.0 * delta));
        long next_ms;
        int key;

        if (!paused && due > cur) {
            /* 늦었으면 사이 프레임은 풀기만 한다 (델타라 건너뛸 수 없다) */
            if (due >= n)
                due = n - 1;
            p->frames_dropped += due - cur - 1;
            while (cur < due) {
                cur++;
                if (decode_frame(p, cur) < 0) {
                    bad = 1;
                    quit = 1;
                    break;
                }
            }
        }
        if (quit)
            break;
        {
            unsigned chord[NX64_CHORD_MAX];
            int nc = paused ? 0 : audio_chord(p, cur, chord);

            if (!p->chord && nc > 1)
                nc = 1;         /* 단음: 첫 목소리 = 선율 (쉬면 화성음도 없으므로 베이스) */

            set_chord(p, chord, nc);
        }
        draw(p, paused);
        nx64_tui_flush();
        p->frames_shown++;
        if (!paused && cur + 1 >= n)
            break;

        /* 다음 프레임 시각까지 키를 기다린다 */
        next_ms = (long)((double)(cur + 1) * delta * 1000.0 / ts);
        t = next_ms - (now_ms() - start);
        key = nx64_tui_key(paused ? -1 : (t > 0 ? (int)t : 0));
        if (key == 'q' || key == 'Q' || key == NX64_KEY_ESC || key == NX64_KEY_CTRL('c') || key == -1) {
            quit = 1;
        } else if (key == ' ') {
            if (paused) {
                start += now_ms() - paused_at;     /* 멈춘 동안은 시간에서 뺀다 */
                paused = 0;
            } else {
                paused_at = now_ms();
                paused = 1;
            }
        } else if (key == 'm' || key == 'M') {
            p->muted = !p->muted;
        } else if (key == 'c' || key == 'C') {
            p->chord = !p->chord;
        } else if (key == NX64_KEY_RESIZE) {
            nx64_tui_invalidate();
        }
    }

    (void)nx64_tone_off();
    nx64_tui_end();
    if (bad) {
        fprintf(stderr, APP_NAME ": %s: frame %lu is damaged\n", file, cur);
        return 1;
    }
    printf("%s: %lu/%lu frames, %lu shown, %lu dropped\n", APP_NAME, cur + 1, n, p->frames_shown,
           p->frames_dropped);
    mp4_free(&p->mp4);
    return 0;
}
