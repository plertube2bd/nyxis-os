/*
 * nxbwenc.c - 호스트 도구: 흑백 프레임 + 오디오 -> MP4 (자체 코덱 nxbw + 사각파 음 트랙 nxsq)
 *
 *   nxbwenc -v <frames.gray> -a <audio.s16> [-r 11025] [-f 30] [-k 300] -o <out.mp4>
 *           [-n <notes.txt>] [-w <preview.wav>] [-W <preview-chord.wav>]
 *
 *   frames.gray  NXBW_W x NXBW_H 의 8비트 회색 프레임을 이어 붙인 것
 *                (ffmpeg -i 영상 -vf scale=80:60:flags=area,format=gray -f rawvideo frames.gray)
 *   audio.s16    모노 16비트(little endian) PCM, 샘플 속도 -r
 *                (ffmpeg -i 영상 -ac 1 -ar 11025 -f s16le audio.s16)
 *   -f           초당 프레임 (기본 30)
 *   -k           키 프레임 간격 (기본 300 = 10초)
 *   -M           음을 오디오에서 뽑지 않고 MIDI 악보에서 (선율 하나: 겹치면 나중 음). -a 는 없어도 된다
 *   -t           악보의 0초가 영상의 몇 ms 인가 (기본 0)
 *   -n           뽑은 화음(선율, 화성음, 베이스)을 프레임마다 적은 글 파일 (확인용)
 *   -w           Nyxis 의 badapple 이 내는 소리 그대로 (기본 단음: 프레임마다 가장 두드러진 음 하나) WAV, 확인용
 *   -W           badapple -c (화음) 소리 그대로: 10ms 아르페지오. PC 스피커는 음량을 나눌 수 없어 모든 음이 같은 크기
 *
 * 형식은 userland/badapple/nxbw.h 와 docs/BADAPPLE.md. 압축은 uzlib (third_party/uzlib) 의 deflate 이고,
 * 쓴 샘플은 모두 uzlib 의 inflate 로 다시 풀어 확인한다 (플레이어와 같은 길).
 *
 * 사각파 음 트랙 (화음): 프레임마다 목소리 3개 (선율, 화성음, 베이스). 프레임마다 그 시각의 오디오 2048 샘플
 * (Hann 창)에서 반음마다(E2..A5) 배음 6개의 크기를 더한 "현저도"를 구하고, 선율(A3..A5)과 베이스(E2..G#3)는 각각
 * 비터비(동적 계획)로 고른다: 현저도가 크고 앞 음에서 멀리 뛰지 않는 길. 화성음은 선율 다음으로 두드러진 음.
 * 조용하거나 음높이가 뚜렷하지 않은 곳은 쉼. Nyxis 는 화음을 PC 스피커 하나로 10ms 아르페지오로 낸다 (pcspk.h).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "uzlib.h"
#include "nxbw.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------- 작은 도우미 ---------------- */

static void die(const char *msg)
{
    fprintf(stderr, "nxbwenc: %s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (!p)
        die("out of memory");
    return p;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = xmalloc((size_t)n);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)
        die("read failed");
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* 늘어나는 바이트 버퍼 (MP4 상자 만들기) */
struct buf {
    unsigned char *p;
    size_t len, cap;
};

static void b_put(struct buf *b, const void *data, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;

        while (cap < b->len + n)
            cap *= 2;
        b->p = realloc(b->p, cap);
        if (!b->p)
            die("out of memory");
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void b_u8(struct buf *b, unsigned v) { unsigned char c = (unsigned char)v; b_put(b, &c, 1); }
static void b_u16(struct buf *b, unsigned v) { b_u8(b, v >> 8); b_u8(b, v); }
static void b_u32(struct buf *b, unsigned long v) { b_u16(b, (unsigned)(v >> 16) & 0xFFFF); b_u16(b, (unsigned)v & 0xFFFF); }
static void b_zero(struct buf *b, size_t n) { while (n--) b_u8(b, 0); }
static void b_fourcc(struct buf *b, const char *s) { b_put(b, s, 4); }

/* 상자 열기: 크기 자리를 남기고 그 위치를 돌려준다. 닫을 때 채운다 */
static size_t box_open(struct buf *b, const char *type)
{
    size_t at = b->len;

    b_u32(b, 0);
    b_fourcc(b, type);
    return at;
}

static size_t fullbox_open(struct buf *b, const char *type, unsigned version, unsigned long flags)
{
    size_t at = box_open(b, type);

    b_u32(b, ((unsigned long)version << 24) | (flags & 0xFFFFFF));
    return at;
}

static void box_close(struct buf *b, size_t at)
{
    unsigned long size = (unsigned long)(b->len - at);

    b->p[at] = (unsigned char)(size >> 24);
    b->p[at + 1] = (unsigned char)(size >> 16);
    b->p[at + 2] = (unsigned char)(size >> 8);
    b->p[at + 3] = (unsigned char)size;
}

static void b_matrix(struct buf *b)
{
    b_u32(b, 0x00010000); b_u32(b, 0); b_u32(b, 0);
    b_u32(b, 0); b_u32(b, 0x00010000); b_u32(b, 0);
    b_u32(b, 0); b_u32(b, 0); b_u32(b, 0x40000000);
}

/* ---------------- 압축 (uzlib) ---------------- */

/* src 를 zlib 스트림으로 out 에 붙인다 */
static void zlib_compress_into(struct buf *out, const unsigned char *src, unsigned len)
{
    struct uzlib_comp comp;
    unsigned long adler;
    size_t hash_size;

    memset(&comp, 0, sizeof(comp));
    comp.dict_size = 32768;
    comp.hash_bits = 12;
    hash_size = sizeof(uzlib_hash_entry_t) * ((size_t)1 << comp.hash_bits);
    comp.hash_table = xmalloc(hash_size);
    memset(comp.hash_table, 0, hash_size);

    zlib_start_block(&comp);
    uzlib_compress(&comp, src, len);
    zlib_finish_block(&comp);

    b_u8(out, 0x78);            /* CMF: deflate, 32K 창 */
    b_u8(out, 0x01);            /* FLG: (0x78 * 256 + 0x01) % 31 == 0 */
    b_put(out, comp.outbuf, (size_t)comp.outlen);
    adler = uzlib_adler32(src, len, 1);
    b_u32(out, adler);

    free(comp.outbuf);
    free(comp.hash_table);
}

/* 플레이어와 같은 길로 풀어 본다 */
static void zlib_verify(const unsigned char *z, size_t zlen, const unsigned char *expect, unsigned len)
{
    unsigned char out[NXBW_FRAME_BYTES + 1];
    struct uzlib_uncomp d;
    int r;

    uzlib_uncompress_init(&d, NULL, 0);
    d.source = z;
    d.source_limit = z + zlen;
    d.source_read_cb = NULL;
    if (uzlib_zlib_parse_header(&d) < 0)
        die("verify: bad zlib header");
    d.dest_start = d.dest = out;
    d.dest_limit = out + len;
    r = uzlib_uncompress_chksum(&d);
    if (r == TINF_OK && d.dest == d.dest_limit) {
        d.dest_limit = out + len + 1;       /* 끝 표시와 adler32 까지 읽게 */
        r = uzlib_uncompress_chksum(&d);
    }
    if (r != TINF_DONE || (unsigned)(d.dest - out) != len || memcmp(out, expect, len) != 0)
        die("verify: round trip through uzlib failed");
}

/* ---------------- 영상 ---------------- */

static void quantize(const unsigned char *gray, unsigned char *packed)
{
    int i;

    memset(packed, 0, NXBW_FRAME_BYTES);
    for (i = 0; i < NXBW_W * NXBW_H; i++) {
        unsigned q = ((unsigned)gray[i] * 15U + 127U) / 255U;

        if (i & 1)
            packed[i >> 1] |= (unsigned char)q;
        else
            packed[i >> 1] |= (unsigned char)(q << 4);
    }
}

/* ---------------- 화음 뽑기 ---------------- */

#define WIN        2048
#define ALL_LO     40          /* E2 (82 Hz): 현저도를 구하는 전체 범위 */
#define ALL_HI     81          /* A5 (880 Hz) */
#define NALL       (ALL_HI - ALL_LO + 1)
#define MEL_LO     57          /* 선율 (노래 음역) A3..A5 */
#define MEL_HI     81
#define BASS_LO    40          /* 베이스 E2..G#3 */
#define BASS_HI    56
#define HARMONICS  6
#define PEAK_MIN   1.9         /* 유성 판정: 범위 안 현저도 최대/평균이 이보다 작으면 쉼 */
#define HARM_MIN   0.8         /* 화성음: 선율 다음으로 두드러진 음이 최대의 이만큼은 되어야 */
#define HARM_RUN   3           /* 화성음: 이보다 짧게 이어지면 버린다 (떨림 방지) */

/* 음 트랙의 목소리 (샘플 안의 순서 = 커널 아르페지오의 순서) */
#define V_MELODY   0
#define V_HARMONY  1
#define V_BASS     2
#define NVOICES    3

static double note_hz(int midi)
{
    return 440.0 * pow(2.0, (midi - 69) / 12.0);
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* sal[i * NALL + (midi - ALL_LO)] 의 [lo, hi] 부분을 프레임마다 0..1 로 (out), 튀는 정도는 peak */
static void normalize_range(const double *sal, int nframes, int lo, int hi, double *out, double *peak)
{
    int nn = hi - lo + 1;
    int i, n;

    for (i = 0; i < nframes; i++) {
        const double *row = sal + (size_t)i * NALL + (lo - ALL_LO);
        double best = 0.0, mean = 0.0;

        for (n = 0; n < nn; n++) {
            mean += row[n];
            if (row[n] > best)
                best = row[n];
        }
        mean /= nn;
        peak[i] = mean > 0.0 ? best / mean : 0.0;
        for (n = 0; n < nn; n++)
            out[(size_t)i * nn + n] = best > 0.0 ? row[n] / best : 0.0;
    }
}

/* 비터비: 현저도 합 - 뛴 거리 벌점 (반음당 jump, 한 옥타브까지) 이 가장 큰 길. note[i] = lo + 고른 번호 */
static void viterbi(const double *s, int nframes, int lo, int nn, double jump, int *note)
{
    double *score = xmalloc(sizeof(double) * (size_t)nframes * (size_t)nn);
    int *back = xmalloc(sizeof(int) * (size_t)nframes * (size_t)nn);
    int i, n, m;

    for (n = 0; n < nn; n++) {
        score[n] = s[n];
        back[n] = n;
    }
    for (i = 1; i < nframes; i++) {
        for (n = 0; n < nn; n++) {
            double bs = -1e30;
            int bm = n;

            for (m = 0; m < nn; m++) {
                int d = n > m ? n - m : m - n;
                double v = score[(size_t)(i - 1) * nn + m] - jump * (d > 12 ? 12 : d);

                if (v > bs) {
                    bs = v;
                    bm = m;
                }
            }
            score[(size_t)i * nn + n] = bs + s[(size_t)i * nn + n];
            back[(size_t)i * nn + n] = bm;
        }
    }
    m = 0;
    for (n = 1; n < nn; n++)
        if (score[(size_t)(nframes - 1) * nn + n] > score[(size_t)(nframes - 1) * nn + m])
            m = n;
    for (i = nframes - 1; i >= 0; i--) {
        note[i] = lo + m;
        m = back[(size_t)i * nn + m];
    }
    free(score);
    free(back);
}

/* 3 프레임 중앙값, 그리고 min_run 보다 짧은 음은 앞 음에 붙이거나 (merge) 쉼으로 (!merge) */
static void tidy(int *note, int nframes, int min_run, int merge)
{
    int *t = xmalloc(sizeof(int) * (size_t)nframes);
    int i, k;

    for (i = 0; i < nframes; i++) {
        int v[3];

        for (k = 0; k < 3; k++) {
            int j = i + k - 1;

            v[k] = note[j < 0 ? 0 : j >= nframes ? nframes - 1 : j];
        }
        qsort(v, 3, sizeof(int), cmp_int);
        t[i] = v[1];
    }
    for (i = 0; i < nframes; ) {
        int j = i;

        while (j < nframes && t[j] == t[i])
            j++;
        if (j - i < min_run && t[i] != 0) {
            for (k = i; k < j; k++)
                t[k] = (merge && i > 0) ? t[i - 1] : 0;
        }
        i = j;
    }
    memcpy(note, t, sizeof(int) * (size_t)nframes);
    free(t);
}

/*
 * 프레임마다 MIDI 음 번호 NVOICES 개 (0 = 쉼): voice[i * NVOICES + V_*]
 *   선율   노래 음역에서 비터비 (반주/베이스로 옥타브씩 튀지 않게)
 *   베이스 낮은 음역에서 비터비
 *   화성음 노래 음역에서 선율 다음으로 두드러진 음 (선율과 3반음 이상 떨어지고 옥타브가 아닌 것)
 * 조용한 곳, 음높이가 뚜렷하지 않은 곳, 음역 끝에 붙은 음(그 밖의 음에 끌린 것)은 쉼.
 */
static int *extract_chords(const int16_t *pcm, size_t nsamp, int rate, int fps, int nframes)
{
    const int nmel = MEL_HI - MEL_LO + 1, nbass = BASS_HI - BASS_LO + 1;
    double *sal = xmalloc(sizeof(double) * (size_t)nframes * NALL);
    double *mel = xmalloc(sizeof(double) * (size_t)nframes * (size_t)nmel);
    double *bass = xmalloc(sizeof(double) * (size_t)nframes * (size_t)nbass);
    double *mpeak = xmalloc(sizeof(double) * (size_t)nframes);
    double *bpeak = xmalloc(sizeof(double) * (size_t)nframes);
    double *rms = xmalloc(sizeof(double) * (size_t)nframes);
    double *win = xmalloc(sizeof(double) * WIN);
    double *x = xmalloc(sizeof(double) * WIN);
    int *mnote = xmalloc(sizeof(int) * (size_t)nframes);
    int *bnote = xmalloc(sizeof(int) * (size_t)nframes);
    int *hnote = xmalloc(sizeof(int) * (size_t)nframes);
    int *voice = xmalloc(sizeof(int) * (size_t)nframes * NVOICES);
    double maxrms = 0.0;
    int i, n, h, k;

    for (i = 0; i < WIN; i++)
        win[i] = 0.5 - 0.5 * cos(2.0 * M_PI * i / (WIN - 1));

    /* 현저도: 반음마다 배음 HARMONICS 개의 크기 합 (배음마다 0.84 배 가중) */
    for (i = 0; i < nframes; i++) {
        long center = (long)((double)i * rate / fps + 0.5 * rate / fps);
        long start = center - WIN / 2;
        double e = 0.0;

        for (k = 0; k < WIN; k++) {
            long s = start + k;
            double v = (s >= 0 && (size_t)s < nsamp) ? pcm[s] / 32768.0 : 0.0;

            e += v * v;
            x[k] = v * win[k];
        }
        rms[i] = sqrt(e / WIN);
        if (rms[i] > maxrms)
            maxrms = rms[i];

        for (n = 0; n < NALL; n++) {
            double f0 = note_hz(ALL_LO + n);
            double sv = 0.0, w = 1.0;

            for (h = 1; h <= HARMONICS; h++) {
                double f = f0 * h;
                double re = 0.0, im = 0.0;
                double c, s, wr, wi, t;

                if (f >= rate / 2.0)
                    break;
                /* 회전자 점화식으로 e^{-j 2 pi f k / rate} */
                c = cos(2.0 * M_PI * f / rate);
                s = -sin(2.0 * M_PI * f / rate);
                wr = 1.0;
                wi = 0.0;
                for (k = 0; k < WIN; k++) {
                    re += x[k] * wr;
                    im += x[k] * wi;
                    t = wr * c - wi * s;
                    wi = wr * s + wi * c;
                    wr = t;
                }
                sv += w * sqrt(re * re + im * im);
                w *= 0.84;
            }
            sal[(size_t)i * NALL + n] = sv;
        }
    }

    normalize_range(sal, nframes, MEL_LO, MEL_HI, mel, mpeak);
    normalize_range(sal, nframes, BASS_LO, BASS_HI, bass, bpeak);
    viterbi(mel, nframes, MEL_LO, nmel, 0.035, mnote);
    viterbi(bass, nframes, BASS_LO, nbass, 0.035, bnote);

    for (i = 0; i < nframes; i++) {
        int quiet = rms[i] < maxrms * 0.06;

        if (quiet || mpeak[i] < PEAK_MIN || mnote[i] == MEL_LO)
            mnote[i] = 0;
        if (quiet || bpeak[i] < PEAK_MIN || bnote[i] == BASS_LO || bnote[i] == BASS_HI)
            bnote[i] = 0;

        /* 화성음: 선율이 있을 때만 */
        hnote[i] = 0;
        if (mnote[i]) {
            double best = HARM_MIN;

            for (n = 0; n < nmel; n++) {
                int midi = MEL_LO + n;
                int d = midi > mnote[i] ? midi - mnote[i] : mnote[i] - midi;
                double v = mel[(size_t)i * nmel + n];

                if (d < 3 || d % 12 == 0 || midi == MEL_LO || midi == MEL_HI)
                    continue;
                if (v >= best) {
                    best = v;
                    hnote[i] = midi;
                }
            }
        }
    }

    tidy(mnote, nframes, 2, 1);
    tidy(bnote, nframes, 2, 1);
    tidy(hnote, nframes, HARM_RUN, 0);

    for (i = 0; i < nframes; i++) {
        int *v = voice + (size_t)i * NVOICES;

        v[V_MELODY] = mnote[i];
        v[V_HARMONY] = mnote[i] ? hnote[i] : 0;
        v[V_BASS] = bnote[i];
        /* 같은 음이 둘이면 하나만 (아르페지오에서 한 음이 두 번 나오지 않게) */
        if (v[V_HARMONY] && v[V_HARMONY] == v[V_MELODY])
            v[V_HARMONY] = 0;
        if (v[V_BASS] && (v[V_BASS] == v[V_MELODY] || v[V_BASS] == v[V_HARMONY]))
            v[V_BASS] = 0;
    }

    free(sal);
    free(mel);
    free(bass);
    free(mpeak);
    free(bpeak);
    free(rms);
    free(win);
    free(x);
    free(mnote);
    free(bnote);
    free(hnote);
    return voice;
}

/*
 * 사각파 트랙을 Nyxis 의 PC 스피커와 같은 소리로 (확인용 WAV).
 *   arp = 1: 커널과 똑같이 10ms (타이머 tick) 마다 화음의 다음 음으로 바꾼다 = 어느 순간이든 한 음 (겹치지 않는다)
 *   arp = 0: 프레임마다 가장 두드러진 음 하나 (선율, 없으면 베이스)
 */
#define ARP_TICK_HZ 100
static void write_wav_preview(const char *path, const unsigned *hz, int nframes, int fps, int arp)
{
    const int rate = 22050;
    long total = (long)nframes * rate / fps;
    FILE *f = fopen(path, "wb");
    double phase = 0.0;
    long i;
    unsigned char hdr[44];
    unsigned long data_len = (unsigned long)total * 2;
    unsigned prev_set[NVOICES] = { 0, 0, 0 };
    long tick_base = 0;

    if (!f) {
        perror(path);
        exit(1);
    }
    memcpy(hdr, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\0\0\0\0\0\0\0\0\x02\0\x10\0data\0\0\0\0", 44);
#define LE32(p, v) do { (p)[0] = (unsigned char)(v); (p)[1] = (unsigned char)((v) >> 8); \
                        (p)[2] = (unsigned char)((v) >> 16); (p)[3] = (unsigned char)((v) >> 24); } while (0)
    LE32(hdr + 4, 36 + data_len);
    LE32(hdr + 24, (unsigned long)rate);
    LE32(hdr + 28, (unsigned long)rate * 2);
    LE32(hdr + 40, data_len);
    fwrite(hdr, 1, 44, f);
    for (i = 0; i < total; i++) {
        int fr = (int)(i * fps / rate);
        const unsigned *v = hz + (size_t)(fr < nframes ? fr : nframes - 1) * NVOICES;
        unsigned list[NVOICES];
        int nl = 0, k;
        unsigned h = 0;
        short s = 0;
        unsigned char le[2];
        long tick = i * ARP_TICK_HZ / rate;

        for (k = 0; k < NVOICES; k++)
            if (v[k])
                list[nl++] = v[k];
        if (memcmp(prev_set, v, sizeof(prev_set)) != 0) {
            memcpy(prev_set, v, sizeof(prev_set));
            tick_base = tick;           /* 커널: 화음이 바뀌면 첫 음부터 */
        }
        if (fr < nframes && nl > 0) {
            if (arp)
                h = list[(tick - tick_base) % nl];
            else
                h = v[V_MELODY] ? v[V_MELODY] : list[0];
        }
        if (h) {
            phase += (double)h / rate;
            phase -= floor(phase);
            s = phase < 0.5 ? 6000 : -6000;
        }
        le[0] = (unsigned char)s;
        le[1] = (unsigned char)((unsigned short)s >> 8);
        fwrite(le, 1, 2, f);
    }
    fclose(f);
}

/* ---------------- MIDI 악보 (-M) ---------------- */

/*
 * 표준 MIDI 파일 (형식 0/1) 의 음을 모두 읽는다. 템포 바뀜(FF 51)도 따른다. 타악기 채널(10)은 뺀다.
 * 음 = (시작 초, 끝 초, 음 번호)
 */
struct midi_note {
    double start, end;
    int key;
};

struct tempo_ev {
    unsigned long tick;
    unsigned long usec_per_quarter;
};

static unsigned long midi_vlq(const unsigned char *b, size_t len, size_t *i)
{
    unsigned long v = 0;
    int n = 0;

    while (*i < len && n < 4) {
        unsigned char c = b[(*i)++];

        v = (v << 7) | (c & 0x7F);
        n++;
        if (!(c & 0x80))
            return v;
    }
    die("midi: bad variable-length number");
    return 0;
}

static unsigned long rd32(const unsigned char *p) { return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3]; }

static double tick_to_sec(unsigned long tick, unsigned div, const struct tempo_ev *t, int nt)
{
    double sec = 0.0;
    unsigned long last = 0, usec = 500000;      /* 기본 120 BPM */
    int k;

    for (k = 0; k < nt && t[k].tick <= tick; k++) {
        sec += (double)(t[k].tick - last) * usec / div / 1e6;
        last = t[k].tick;
        usec = t[k].usec_per_quarter;
    }
    return sec + (double)(tick - last) * usec / div / 1e6;
}

static int cmp_tempo(const void *a, const void *b)
{
    const struct tempo_ev *x = a, *y = b;

    return x->tick < y->tick ? -1 : x->tick > y->tick;
}

static int cmp_note_start(const void *a, const void *b)
{
    const struct midi_note *x = a, *y = b;

    return x->start < y->start ? -1 : x->start > y->start;
}

/* 음 목록 (시작 순). *count 에 개수 */
static struct midi_note *read_midi(const char *path, int *count)
{
    size_t len, i;
    unsigned char *b = read_file(path, &len);
    unsigned fmt, ntrk, div, t;
    /* 날 tick 으로 먼저 모은다 (템포 표는 모든 트랙을 읽은 뒤에 완성된다) */
    struct raw { unsigned long s, e; int key; } *raw = NULL;
    int nraw = 0, capraw = 0;
    struct tempo_ev tempo[256];
    int ntempo = 0;
    struct midi_note *out;
    int k;

    if (len < 14 || memcmp(b, "MThd", 4) != 0)
        die("midi: not a MIDI file");
    fmt = (b[8] << 8) | b[9];
    ntrk = (b[10] << 8) | b[11];
    div = (b[12] << 8) | b[13];
    if (fmt > 1 || div == 0 || (div & 0x8000))
        die("midi: only format 0/1 with ticks-per-quarter timing is supported");
    i = 8 + rd32(b + 4);
    for (t = 0; t < ntrk; t++) {
        size_t end;
        unsigned long tick = 0;
        unsigned char run = 0;
        unsigned long on_tick[16][128];
        int on[16][128];

        if (i + 8 > len || memcmp(b + i, "MTrk", 4) != 0)
            die("midi: bad track header");
        end = i + 8 + rd32(b + i + 4);
        if (end > len)
            die("midi: track past end of file");
        i += 8;
        memset(on, 0, sizeof(on));
        while (i < end) {
            unsigned char st;
            unsigned ev, ch, a = 0, c = 0;

            tick += midi_vlq(b, end, &i);
            if (i >= end)
                break;
            st = b[i];
            if (st == 0xFF) {                       /* 메타 */
                unsigned type;
                unsigned long l;

                if (i + 2 > end)
                    break;
                type = b[i + 1];
                i += 2;
                l = midi_vlq(b, end, &i);
                if (i + l > end)
                    die("midi: meta event past end");
                if (type == 0x51 && l == 3 && ntempo < 256) {
                    tempo[ntempo].tick = tick;
                    tempo[ntempo].usec_per_quarter = ((unsigned long)b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
                    ntempo++;
                }
                i += l;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {         /* sysex */
                unsigned long l;

                i++;
                l = midi_vlq(b, end, &i);
                i += l;
                continue;
            }
            if (st & 0x80) {
                run = st;
                i++;
            } else if (!run) {
                die("midi: running status without a status byte");
            }
            ev = run & 0xF0;
            ch = run & 0x0F;
            if (ev == 0xC0 || ev == 0xD0) {
                if (i + 1 > end)
                    break;
                a = b[i++];
            } else {
                if (i + 2 > end)
                    break;
                a = b[i];
                c = b[i + 1];
                i += 2;
            }
            if (ch == 9 || a > 127)
                continue;                           /* 타악기 */
            if (ev == 0x90 && c > 0) {
                if (on[ch][a])                      /* 같은 음이 다시 눌렸다: 앞 음을 여기서 끝낸다 */
                    goto note_off;
                on[ch][a] = 1;
                on_tick[ch][a] = tick;
            } else if (ev == 0x80 || (ev == 0x90 && c == 0)) {
                if (!on[ch][a])
                    continue;
note_off:
                if (nraw == capraw) {
                    capraw = capraw ? capraw * 2 : 1024;
                    raw = realloc(raw, sizeof(*raw) * (size_t)capraw);
                    if (!raw)
                        die("out of memory");
                }
                raw[nraw].s = on_tick[ch][a];
                raw[nraw].e = tick;
                raw[nraw].key = (int)a;
                nraw++;
                on[ch][a] = 0;
                if (ev == 0x90 && c > 0) {          /* 다시 눌린 음의 새 시작 */
                    on[ch][a] = 1;
                    on_tick[ch][a] = tick;
                }
            }
        }
        i = end;
    }
    qsort(tempo, (size_t)ntempo, sizeof(tempo[0]), cmp_tempo);
    out = xmalloc(sizeof(*out) * (size_t)(nraw ? nraw : 1));
    for (k = 0; k < nraw; k++) {
        out[k].start = tick_to_sec(raw[k].s, div, tempo, ntempo);
        out[k].end = tick_to_sec(raw[k].e, div, tempo, ntempo);
        out[k].key = raw[k].key;
    }
    qsort(out, (size_t)nraw, sizeof(*out), cmp_note_start);
    free(raw);
    free(b);
    *count = nraw;
    return out;
}

/*
 * 악보 -> 프레임마다 음 하나 (선율 목소리, 나머지 목소리는 쉼). 프레임 가운데 시각에 울리는 음 중 가장 늦게 시작한 것
 * (음이 겹쳐도 하나만: 부저처럼). 음의 끝이 다음 프레임 안에 있으면 그 프레임은 쉼으로 둔다: 같은 음이 이어질 때도
 * 음이 끊어져 들리게 (스타카토). offset: 악보의 0 초가 영상의 몇 초인가.
 */
static int *voices_from_midi(const struct midi_note *nt, int count, int fps, int nframes, double offset)
{
    int *voice = xmalloc(sizeof(int) * (size_t)nframes * NVOICES);
    double fl = 1.0 / fps;
    int i, k, first = 0;

    memset(voice, 0, sizeof(int) * (size_t)nframes * NVOICES);
    for (i = 0; i < nframes; i++) {
        double t = (i + 0.5) * fl - offset;
        int best = -1;

        while (first < count && nt[first].end <= t - 10.0)
            first++;                                /* 아주 오래전에 끝난 음은 다시 보지 않는다 */
        for (k = first; k < count && nt[k].start <= t; k++) {
            if (nt[k].end > t && (best < 0 || nt[k].start >= nt[best].start))
                best = k;
        }
        if (best < 0)
            continue;
        if (nt[best].end - t < fl && nt[best].end - nt[best].start > 2.5 * fl)
            continue;                               /* 음의 마지막 프레임: 끊어 준다 */
        voice[(size_t)i * NVOICES + V_MELODY] = nt[best].key;
    }
    return voice;
}

/* ---------------- MP4 ---------------- */

struct track_samples {
    unsigned long *offset;
    unsigned long *size;
};

static void write_stbl_common(struct buf *b, const struct track_samples *t, int n, int const_size)
{
    size_t at;
    int i;

    at = fullbox_open(b, "stts", 0, 0);
    b_u32(b, 1);
    b_u32(b, (unsigned long)n);
    b_u32(b, 1);
    box_close(b, at);

    at = fullbox_open(b, "stsc", 0, 0);
    b_u32(b, 1);
    b_u32(b, 1);                /* first_chunk */
    b_u32(b, 1);                /* samples_per_chunk */
    b_u32(b, 1);                /* sample_description_index */
    box_close(b, at);

    at = fullbox_open(b, "stsz", 0, 0);
    if (const_size) {
        b_u32(b, (unsigned long)const_size);
        b_u32(b, (unsigned long)n);
    } else {
        b_u32(b, 0);
        b_u32(b, (unsigned long)n);
        for (i = 0; i < n; i++)
            b_u32(b, t->size[i]);
    }
    box_close(b, at);

    at = fullbox_open(b, "stco", 0, 0);
    b_u32(b, (unsigned long)n);
    for (i = 0; i < n; i++)
        b_u32(b, t->offset[i]);
    box_close(b, at);
}

static void write_dinf(struct buf *b)
{
    size_t dinf = box_open(b, "dinf");
    size_t dref = fullbox_open(b, "dref", 0, 0);
    size_t url;

    b_u32(b, 1);
    url = fullbox_open(b, "url ", 0, 1);     /* 1 = 같은 파일 안 */
    box_close(b, url);
    box_close(b, dref);
    box_close(b, dinf);
}

static void write_tkhd(struct buf *b, unsigned id, unsigned long dur_ms, int is_audio)
{
    size_t at = fullbox_open(b, "tkhd", 0, 3);   /* enabled | in movie */

    b_u32(b, 0); b_u32(b, 0);          /* creation, modification */
    b_u32(b, id);
    b_u32(b, 0);
    b_u32(b, dur_ms);
    b_zero(b, 8);
    b_u16(b, 0); b_u16(b, 0);          /* layer, alternate_group */
    b_u16(b, is_audio ? 0x0100 : 0);   /* volume */
    b_u16(b, 0);
    b_matrix(b);
    b_u32(b, is_audio ? 0 : (unsigned long)NXBW_W << 16);
    b_u32(b, is_audio ? 0 : (unsigned long)NXBW_H << 16);
    box_close(b, at);
}

static void write_mdhd_hdlr(struct buf *b, int fps, int n, const char *handler, const char *name)
{
    size_t at = fullbox_open(b, "mdhd", 0, 0);

    b_u32(b, 0); b_u32(b, 0);
    b_u32(b, (unsigned long)fps);      /* timescale: 한 샘플 = 1 */
    b_u32(b, (unsigned long)n);
    b_u16(b, 0x55C4);                  /* 'und' */
    b_u16(b, 0);
    box_close(b, at);

    at = fullbox_open(b, "hdlr", 0, 0);
    b_u32(b, 0);
    b_fourcc(b, handler);
    b_zero(b, 12);
    b_put(b, name, strlen(name) + 1);
    box_close(b, at);
}

int main(int argc, char **argv)
{
    const char *vpath = NULL, *apath = NULL, *opath = NULL, *npath = NULL, *wpath = NULL, *mpath = NULL;
    const char *midipath = NULL;
    double midi_offset = 0.0;
    int rate = 11025, fps = 30, keyint = 300;
    unsigned char *frames, *pcm_bytes = NULL;
    size_t flen, alen;
    int nframes, i;
    int *voice;
    unsigned *hz;
    unsigned char prev[NXBW_FRAME_BYTES], cur[NXBW_FRAME_BYTES], tmp[NXBW_FRAME_BYTES];
    struct buf mdat = { 0, 0, 0 }, moov = { 0, 0, 0 }, ftyp = { 0, 0, 0 }, sample = { 0, 0, 0 };
    struct track_samples vt, at_;
    unsigned long mdat_base;
    unsigned long dur_ms;
    unsigned long nkeys = 0, nsame = 0;
    unsigned long *keys;
    FILE *out;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (!v)
            die("usage: nxbwenc -v frames.gray -a audio.s16 [-r rate] [-f fps] [-k keyint] -o out.mp4 [-n notes.txt] [-w preview.wav]");
        if (!strcmp(a, "-v")) vpath = v;
        else if (!strcmp(a, "-a")) apath = v;
        else if (!strcmp(a, "-o")) opath = v;
        else if (!strcmp(a, "-n")) npath = v;
        else if (!strcmp(a, "-w")) wpath = v;
        else if (!strcmp(a, "-W")) mpath = v;
        else if (!strcmp(a, "-M")) midipath = v;
        else if (!strcmp(a, "-t")) midi_offset = atof(v) / 1000.0;
        else if (!strcmp(a, "-r")) rate = atoi(v);
        else if (!strcmp(a, "-f")) fps = atoi(v);
        else if (!strcmp(a, "-k")) keyint = atoi(v);
        else die("unknown option");
        i++;
    }
    if (!vpath || (!apath && !midipath) || !opath || rate < 4000 || fps < 1 || keyint < 1)
        die("usage: nxbwenc -v frames.gray (-a audio.s16 | -M score.mid [-t offset_ms]) [-r rate] [-f fps] [-k keyint] -o out.mp4");

    uzlib_init();
    frames = read_file(vpath, &flen);
    nframes = (int)(flen / (NXBW_W * NXBW_H));
    if (nframes < 1)
        die("no frames");

    if (midipath) {
        int count;
        struct midi_note *nt = read_midi(midipath, &count);

        fprintf(stderr, "nxbwenc: %d frames; score %s: %d notes, %.1f..%.1f s (offset %+.3f s)\n", nframes, midipath,
                count, count ? nt[0].start : 0.0, count ? nt[count - 1].end : 0.0, midi_offset);
        voice = voices_from_midi(nt, count, fps, nframes, midi_offset);
        free(nt);
    } else {
        size_t k;
        int16_t *pcm;

        /* PCM: little endian -> 호스트 */
        pcm_bytes = read_file(apath, &alen);
        pcm = xmalloc(alen);
        for (k = 0; k + 1 < alen; k += 2)
            pcm[k / 2] = (int16_t)(pcm_bytes[k] | (pcm_bytes[k + 1] << 8));
        fprintf(stderr, "nxbwenc: %d frames, %zu audio samples; extracting chords...\n", nframes, alen / 2);
        voice = extract_chords(pcm, alen / 2, rate, fps, nframes);
        free(pcm);
        free(pcm_bytes);
    }
    hz = xmalloc(sizeof(unsigned) * (size_t)nframes * NVOICES);
    for (i = 0; i < nframes * NVOICES; i++)
        hz[i] = voice[i] ? (unsigned)(note_hz(voice[i]) + 0.5) : 0;

    if (npath) {
        static const char *names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        FILE *nf = fopen(npath, "w");

        if (!nf) {
            perror(npath);
            return 1;
        }
        fprintf(nf, "# frame time melody harmony bass (- = rest)\n");
        for (i = 0; i < nframes; i++) {
            int k;

            fprintf(nf, "%d %.3f", i, (double)i / fps);
            for (k = 0; k < NVOICES; k++) {
                int m = voice[(size_t)i * NVOICES + k];

                if (m)
                    fprintf(nf, " %s%d", names[m % 12], m / 12 - 1);
                else
                    fprintf(nf, " -");
            }
            fprintf(nf, "\n");
        }
        fclose(nf);
    }
    if (wpath)
        write_wav_preview(wpath, hz, nframes, fps, 0);
    if (mpath)
        write_wav_preview(mpath, hz, nframes, fps, 1);

    /* ftyp */
    {
        size_t a = box_open(&ftyp, "ftyp");

        b_fourcc(&ftyp, "isom");
        b_u32(&ftyp, 0x200);
        b_fourcc(&ftyp, "isom");
        b_fourcc(&ftyp, "iso2");
        b_fourcc(&ftyp, "mp41");
        box_close(&ftyp, a);
    }
    mdat_base = (unsigned long)ftyp.len + 8;      /* mdat 머리 8 바이트 뒤가 첫 샘플 */

    vt.offset = xmalloc(sizeof(unsigned long) * (size_t)nframes);
    vt.size = xmalloc(sizeof(unsigned long) * (size_t)nframes);
    at_.offset = xmalloc(sizeof(unsigned long) * (size_t)nframes);
    at_.size = NULL;
    keys = xmalloc(sizeof(unsigned long) * (size_t)nframes);

    /* 샘플: 비디오 한 프레임, 음 한 샘플을 번갈아 (interleave) */
    memset(prev, 0, sizeof(prev));
    for (i = 0; i < nframes; i++) {
        int k;

        quantize(frames + (size_t)i * NXBW_W * NXBW_H, cur);
        sample.len = 0;
        if (i % keyint == 0) {
            b_u8(&sample, NXBW_KEY);
            zlib_compress_into(&sample, cur, NXBW_FRAME_BYTES);
            zlib_verify(sample.p + 1, sample.len - 1, cur, NXBW_FRAME_BYTES);
            keys[nkeys++] = (unsigned long)i + 1;
        } else if (memcmp(cur, prev, NXBW_FRAME_BYTES) == 0) {
            b_u8(&sample, NXBW_SAME);
            nsame++;
        } else {
            for (k = 0; k < NXBW_FRAME_BYTES; k++)
                tmp[k] = (unsigned char)(cur[k] ^ prev[k]);
            b_u8(&sample, NXBW_DELTA);
            zlib_compress_into(&sample, tmp, NXBW_FRAME_BYTES);
            zlib_verify(sample.p + 1, sample.len - 1, tmp, NXBW_FRAME_BYTES);
        }
        vt.offset[i] = mdat_base + (unsigned long)mdat.len;
        vt.size[i] = (unsigned long)sample.len;
        b_put(&mdat, sample.p, sample.len);

        at_.offset[i] = mdat_base + (unsigned long)mdat.len;
        for (k = 0; k < NVOICES; k++)
            b_u16(&mdat, hz[(size_t)i * NVOICES + k]);

        memcpy(prev, cur, sizeof(prev));
    }

    dur_ms = (unsigned long)((double)nframes * 1000.0 / fps + 0.5);

    /* moov */
    {
        size_t m = box_open(&moov, "moov");
        size_t a, trak, mdia, minf, stbl, stsd, entry;

        a = fullbox_open(&moov, "mvhd", 0, 0);
        b_u32(&moov, 0); b_u32(&moov, 0);
        b_u32(&moov, 1000);
        b_u32(&moov, dur_ms);
        b_u32(&moov, 0x00010000);       /* rate 1.0 */
        b_u16(&moov, 0x0100);           /* volume 1.0 */
        b_zero(&moov, 10);
        b_matrix(&moov);
        b_zero(&moov, 24);
        b_u32(&moov, 3);                /* next_track_ID */
        box_close(&moov, a);

        /* --- 비디오 트랙 --- */
        trak = box_open(&moov, "trak");
        write_tkhd(&moov, 1, dur_ms, 0);
        mdia = box_open(&moov, "mdia");
        write_mdhd_hdlr(&moov, fps, nframes, "vide", "Nyxis nxbw 4bpp video");
        minf = box_open(&moov, "minf");
        a = fullbox_open(&moov, "vmhd", 0, 1);
        b_zero(&moov, 8);
        box_close(&moov, a);
        write_dinf(&moov);
        stbl = box_open(&moov, "stbl");
        stsd = fullbox_open(&moov, "stsd", 0, 0);
        b_u32(&moov, 1);
        entry = box_open(&moov, "nxbw");
        b_zero(&moov, 6);
        b_u16(&moov, 1);                /* data_reference_index */
        b_zero(&moov, 16);              /* pre_defined, reserved */
        b_u16(&moov, NXBW_W);
        b_u16(&moov, NXBW_H);
        b_u32(&moov, 0x00480000);       /* 72 dpi */
        b_u32(&moov, 0x00480000);
        b_u32(&moov, 0);
        b_u16(&moov, 1);                /* frame_count */
        {
            unsigned char name[32];
            const char *s = "Nyxis nxbw";

            memset(name, 0, sizeof(name));
            name[0] = (unsigned char)strlen(s);
            memcpy(name + 1, s, strlen(s));
            b_put(&moov, name, 32);
        }
        b_u16(&moov, NXBW_BPP);         /* depth */
        b_u16(&moov, 0xFFFF);
        a = box_open(&moov, "nxbC");
        b_u8(&moov, 1);
        b_u8(&moov, NXBW_BPP);
        b_u8(&moov, NXBW_LEVELS);
        b_u8(&moov, 'z');
        b_u16(&moov, NXBW_W);
        b_u16(&moov, NXBW_H);
        b_u32(&moov, (unsigned long)keyint);
        box_close(&moov, a);
        box_close(&moov, entry);
        box_close(&moov, stsd);
        write_stbl_common(&moov, &vt, nframes, 0);
        a = fullbox_open(&moov, "stss", 0, 0);
        b_u32(&moov, nkeys);
        for (i = 0; i < (int)nkeys; i++)
            b_u32(&moov, keys[i]);
        box_close(&moov, a);
        box_close(&moov, stbl);
        box_close(&moov, minf);
        box_close(&moov, mdia);
        box_close(&moov, trak);

        /* --- 사각파 음 트랙 --- */
        trak = box_open(&moov, "trak");
        write_tkhd(&moov, 2, dur_ms, 1);
        mdia = box_open(&moov, "mdia");
        write_mdhd_hdlr(&moov, fps, nframes, "soun", "Nyxis nxsq square wave");
        minf = box_open(&moov, "minf");
        a = fullbox_open(&moov, "smhd", 0, 0);
        b_u32(&moov, 0);
        box_close(&moov, a);
        write_dinf(&moov);
        stbl = box_open(&moov, "stbl");
        stsd = fullbox_open(&moov, "stsd", 0, 0);
        b_u32(&moov, 1);
        entry = box_open(&moov, "nxsq");
        b_zero(&moov, 6);
        b_u16(&moov, 1);
        b_zero(&moov, 8);
        b_u16(&moov, 1);                /* channelcount */
        b_u16(&moov, 16);               /* samplesize */
        b_u32(&moov, 0);
        b_u32(&moov, (unsigned long)fps << 16);   /* samplerate 16.16 */
        a = box_open(&moov, "nxsC");
        b_u8(&moov, 2);                 /* version 2: 샘플 = 목소리 NVOICES 개 */
        b_u8(&moov, 's');
        b_u8(&moov, NVOICES);
        b_u8(&moov, 0);
        box_close(&moov, a);
        box_close(&moov, entry);
        box_close(&moov, stsd);
        write_stbl_common(&moov, &at_, nframes, 2 * NVOICES);
        box_close(&moov, stbl);
        box_close(&moov, minf);
        box_close(&moov, mdia);
        box_close(&moov, trak);

        box_close(&moov, m);
    }

    out = fopen(opath, "wb");
    if (!out) {
        perror(opath);
        return 1;
    }
    fwrite(ftyp.p, 1, ftyp.len, out);
    {
        unsigned char h[8];
        unsigned long sz = (unsigned long)mdat.len + 8;

        h[0] = (unsigned char)(sz >> 24); h[1] = (unsigned char)(sz >> 16);
        h[2] = (unsigned char)(sz >> 8); h[3] = (unsigned char)sz;
        memcpy(h + 4, "mdat", 4);
        fwrite(h, 1, 8, out);
    }
    fwrite(mdat.p, 1, mdat.len, out);
    fwrite(moov.p, 1, moov.len, out);
    if (fclose(out) != 0)
        die("write failed");

    fprintf(stderr, "nxbwenc: %s: %d frames (%lu key, %lu same), %.1f s, %zu bytes (raw %lu)\n",
            opath, nframes, nkeys, nsame, (double)nframes / fps,
            ftyp.len + 8 + mdat.len + moov.len, (unsigned long)nframes * NXBW_FRAME_BYTES);
    return 0;
}
