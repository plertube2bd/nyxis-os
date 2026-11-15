/*
 * mp4.c - 아주 작은 MP4 (ISO BMFF) 읽기. 설명은 mp4.h
 *
 * 모든 길이와 위치는 파일 크기 안인지 확인한다 (깨진 파일이 범위 밖을 읽게 하지 않는다).
 */
#include <stdlib.h>
#include <string.h>

#include "mp4.h"

#define FOURCC(a, b, c, d) \
    (((unsigned long)(a) << 24) | ((unsigned long)(b) << 16) | ((unsigned long)(c) << 8) | (unsigned long)(d))

unsigned long mp4_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

unsigned mp4_be16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

/* [p, end) 안의 상자 하나. 성공하면 1 이고 종류/내용/다음 위치 */
static int next_box(const unsigned char *p, const unsigned char *end, unsigned long *type,
                    const unsigned char **body, const unsigned char **next)
{
    unsigned long size;
    size_t hdr = 8;
    size_t avail;

    if (p >= end || (size_t)(end - p) < 8)
        return 0;
    avail = (size_t)(end - p);
    size = mp4_be32(p);
    *type = mp4_be32(p + 4);
    if (size == 1) {                    /* 64비트 크기: 위 32비트가 0 이어야 (4GiB 넘는 상자는 안 다룬다) */
        if (avail < 16 || mp4_be32(p + 8) != 0)
            return 0;
        size = mp4_be32(p + 12);
        hdr = 16;
    } else if (size == 0) {
        size = (unsigned long)avail;    /* 파일 끝까지 */
    }
    if (size < hdr || size > avail)
        return 0;
    *body = p + hdr;
    *next = p + size;
    return 1;
}

/* [p, end) 에서 그 종류의 첫 상자 */
static const unsigned char *find_box(const unsigned char *p, const unsigned char *end, unsigned long want,
                                     const unsigned char **body_end)
{
    unsigned long type;
    const unsigned char *body;
    const unsigned char *next;

    while (next_box(p, end, &type, &body, &next)) {
        if (type == want) {
            *body_end = next;
            return body;
        }
        p = next;
    }
    return NULL;
}

/* 경로 (상자 종류들) 를 따라 내려간다 */
static const unsigned char *find_path(const unsigned char *p, const unsigned char *end, const unsigned long *path,
                                      int n, const unsigned char **body_end)
{
    int i;

    for (i = 0; i < n && p; i++) {
        p = find_box(p, end, path[i], body_end);
        end = *body_end;
    }
    return p;
}

static int parse_track(struct mp4_track *t, const unsigned char *trak, const unsigned char *trak_end,
                       const unsigned char *file, size_t file_len, const char **why)
{
    static const unsigned long p_mdhd[] = { FOURCC('m','d','i','a'), FOURCC('m','d','h','d') };
    static const unsigned long p_hdlr[] = { FOURCC('m','d','i','a'), FOURCC('h','d','l','r') };
    static const unsigned long p_stbl[] = { FOURCC('m','d','i','a'), FOURCC('m','i','n','f'), FOURCC('s','t','b','l') };
    const unsigned char *b, *be, *stbl, *stbl_end;
    const unsigned char *stsz, *stsc, *stco;
    const unsigned char *stsz_end = NULL, *stsc_end = NULL, *stco_end = NULL;
    unsigned long n, i, nchunks, nstsc, const_size;
    int co64 = 0;
    unsigned long chunk, k, s;

    memset(t, 0, sizeof(*t));

    b = find_path(trak, trak_end, p_mdhd, 2, &be);
    if (!b || be - b < 24) {
        *why = "track without mdhd";
        return -1;
    }
    if (b[0] == 1) {
        if (be - b < 36) {
            *why = "short mdhd";
            return -1;
        }
        t->timescale = mp4_be32(b + 20);
    } else {
        t->timescale = mp4_be32(b + 12);
    }

    b = find_path(trak, trak_end, p_hdlr, 2, &be);
    if (!b || be - b < 12) {
        *why = "track without hdlr";
        return -1;
    }
    t->handler = mp4_be32(b + 8);

    stbl = find_path(trak, trak_end, p_stbl, 3, &stbl_end);
    if (!stbl) {
        *why = "track without stbl";
        return -1;
    }

    /* stsd: 첫 엔트리 */
    b = find_box(stbl, stbl_end, FOURCC('s','t','s','d'), &be);
    if (!b || be - b < 16 || mp4_be32(b + 4) < 1) {
        *why = "bad stsd";
        return -1;
    }
    {
        unsigned long type;
        const unsigned char *body;
        const unsigned char *next;

        if (!next_box(b + 8, be, &type, &body, &next)) {
            *why = "bad stsd entry";
            return -1;
        }
        t->codec = type;
        t->entry = body;
        t->entry_len = (size_t)(next - body);
    }

    /* stts: 고정 프레임 속도만 (첫 항목의 delta) */
    b = find_box(stbl, stbl_end, FOURCC('s','t','t','s'), &be);
    if (!b || be - b < 16 || mp4_be32(b + 4) < 1) {
        *why = "bad stts";
        return -1;
    }
    t->sample_delta = mp4_be32(b + 12);

    stsz = find_box(stbl, stbl_end, FOURCC('s','t','s','z'), &stsz_end);
    stsc = find_box(stbl, stbl_end, FOURCC('s','t','s','c'), &stsc_end);
    stco = find_box(stbl, stbl_end, FOURCC('s','t','c','o'), &stco_end);
    if (!stco) {
        stco = find_box(stbl, stbl_end, FOURCC('c','o','6','4'), &stco_end);
        co64 = 1;
    }
    if (!stsz || !stsc || !stco || stsz_end - stsz < 12 || stsc_end - stsc < 8 || stco_end - stco < 8) {
        *why = "missing sample table (stsz/stsc/stco)";
        return -1;
    }

    const_size = mp4_be32(stsz + 4);
    n = mp4_be32(stsz + 8);
    if (n == 0 || n > 10000000UL || (const_size == 0 && (unsigned long)(stsz_end - stsz - 12) / 4 < n)) {
        *why = "bad stsz";
        return -1;
    }
    nstsc = mp4_be32(stsc + 4);
    if (nstsc == 0 || (unsigned long)(stsc_end - stsc - 8) / 12 < nstsc) {
        *why = "bad stsc";
        return -1;
    }
    nchunks = mp4_be32(stco + 4);
    if (nchunks == 0 || (unsigned long)(stco_end - stco - 8) / (co64 ? 8 : 4) < nchunks) {
        *why = "bad stco";
        return -1;
    }

    t->nsamples = n;
    t->offset = malloc(sizeof(unsigned long) * n);
    t->size = malloc(sizeof(unsigned long) * n);
    if (!t->offset || !t->size) {
        *why = "out of memory";
        return -1;
    }
    for (i = 0; i < n; i++)
        t->size[i] = const_size ? const_size : mp4_be32(stsz + 12 + 4 * i);

    /* stsc: (first_chunk, samples_per_chunk) 구간으로 덩어리마다 샘플을 늘어놓는다 */
    s = 0;
    for (k = 0; k < nstsc && s < n; k++) {
        const unsigned char *e = stsc + 8 + 12 * k;
        unsigned long first = mp4_be32(e);
        unsigned long per = mp4_be32(e + 4);
        unsigned long last = k + 1 < nstsc ? mp4_be32(e + 12) - 1 : nchunks;

        if (first < 1 || per == 0 || last > nchunks || last + 1 < first) {
            *why = "bad stsc entry";
            return -1;
        }
        for (chunk = first; chunk <= last && s < n; chunk++) {
            unsigned long off;
            unsigned long j;

            if (co64) {
                const unsigned char *c = stco + 8 + 8 * (chunk - 1);

                if (mp4_be32(c) != 0) {
                    *why = "chunk beyond 4 GiB";
                    return -1;
                }
                off = mp4_be32(c + 4);
            } else {
                off = mp4_be32(stco + 8 + 4 * (chunk - 1));
            }
            for (j = 0; j < per && s < n; j++) {
                t->offset[s] = off;
                off += t->size[s];
                s++;
            }
        }
    }
    if (s != n) {
        *why = "sample table does not cover every sample";
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (t->offset[i] > file_len || t->size[i] > file_len - t->offset[i]) {
            *why = "sample outside the file";
            return -1;
        }
    }
    (void)file;
    return 0;
}

int mp4_parse(struct mp4_file *f, const unsigned char *data, size_t len, const char **why)
{
    const unsigned char *moov, *moov_end;
    const unsigned char *p;
    unsigned long type;
    const unsigned char *body;
    const unsigned char *next;

    memset(f, 0, sizeof(*f));
    f->data = data;
    f->len = len;

    if (len < 8 || mp4_be32(data + 4) != FOURCC('f','t','y','p')) {
        *why = "not an MP4 file (no ftyp)";
        return -1;
    }
    moov = find_box(data, data + len, FOURCC('m','o','o','v'), &moov_end);
    if (!moov) {
        *why = "no moov box";
        return -1;
    }
    p = moov;
    while (next_box(p, moov_end, &type, &body, &next)) {
        if (type == FOURCC('t','r','a','k') && f->ntracks < MP4_MAX_TRACKS) {
            if (parse_track(&f->track[f->ntracks], body, next, data, len, why) < 0) {
                f->ntracks++;           /* mp4_free 가 그 트랙의 표도 놓도록 */
                mp4_free(f);
                return -1;
            }
            f->ntracks++;
        }
        p = next;
    }
    if (f->ntracks == 0) {
        *why = "no tracks";
        return -1;
    }
    return 0;
}

void mp4_free(struct mp4_file *f)
{
    int i;

    for (i = 0; i < f->ntracks; i++) {
        free(f->track[i].offset);
        free(f->track[i].size);
        f->track[i].offset = NULL;
        f->track[i].size = NULL;
    }
    f->ntracks = 0;
}

struct mp4_track *mp4_find(struct mp4_file *f, unsigned long handler)
{
    int i;

    for (i = 0; i < f->ntracks; i++)
        if (f->track[i].handler == handler)
            return &f->track[i];
    return NULL;
}

const unsigned char *mp4_entry_child(const struct mp4_track *t, size_t skip, unsigned long type, size_t *len)
{
    const unsigned char *body_end;
    const unsigned char *b;

    if (t->entry_len < skip)
        return NULL;
    b = find_box(t->entry + skip, t->entry + t->entry_len, type, &body_end);
    if (b)
        *len = (size_t)(body_end - b);
    return b;
}
