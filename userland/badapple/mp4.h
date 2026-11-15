/*
 * mp4.h - 아주 작은 MP4 (ISO BMFF) 읽기: 트랙의 샘플 표 (위치/크기) 만 뽑는다
 *
 * 파일 전체를 메모리에 둔 상태에서 moov/trak/mdia/minf/stbl 을 따라가 stsd(첫 엔트리의 종류와 자식 상자),
 * stts, stsc, stsz, stco/co64 로 샘플마다 (오프셋, 크기) 를 만든다. 편집 목록(elst)이나 조각난 MP4(moof)는
 * 다루지 않는다 (nxbwenc 이 만드는 파일, 그리고 보통의 "한 덩어리" MP4 는 된다).
 */
#ifndef BADAPPLE_MP4_H
#define BADAPPLE_MP4_H

#include <stddef.h>

struct mp4_track {
    unsigned long handler;          /* 'vide', 'soun' ... */
    unsigned long codec;            /* stsd 첫 엔트리의 종류 ('nxbw' ...) */
    const unsigned char *entry;     /* 그 엔트리의 내용 (크기/종류 다음부터) */
    size_t entry_len;
    unsigned long timescale;        /* mdhd */
    unsigned long sample_delta;     /* stts 첫 항목 (고정 프레임 속도) */
    unsigned long nsamples;
    unsigned long *offset;          /* 샘플마다 파일 안 위치 */
    unsigned long *size;            /* 샘플마다 크기 */
};

#define MP4_MAX_TRACKS 8

struct mp4_file {
    const unsigned char *data;
    size_t len;
    int ntracks;
    struct mp4_track track[MP4_MAX_TRACKS];
};

/* 성공 0. 실패 -1 이고 *why 에 이유 */
int mp4_parse(struct mp4_file *f, const unsigned char *data, size_t len, const char **why);
void mp4_free(struct mp4_file *f);

/* 그 핸들러의 첫 트랙 (없으면 NULL) */
struct mp4_track *mp4_find(struct mp4_file *f, unsigned long handler);

/* 엔트리 안에서 자식 상자 찾기: skip = 엔트리 앞의 고정 길이 (VisualSampleEntry 78, AudioSampleEntry 28).
 * 찾으면 내용의 시작, *len 에 길이 */
const unsigned char *mp4_entry_child(const struct mp4_track *t, size_t skip, unsigned long type, size_t *len);

unsigned long mp4_be32(const unsigned char *p);
unsigned mp4_be16(const unsigned char *p);

#endif /* BADAPPLE_MP4_H */
