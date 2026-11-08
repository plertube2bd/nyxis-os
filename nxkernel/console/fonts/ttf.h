/*
 * ttf.h - 최소 TrueType(.ttf) 파서 + 정수 전용 안티앨리어싱 래스터라이저
 *
 * 1단계 범위 (일부러 작게):
 *   - sfnt 테이블 디렉터리, head / maxp / hhea / hmtx / loca / glyf / cmap(format 4)
 *   - simple glyph (2차 베지어 윤곽선) + 오프셋만 있는 compound glyph
 *   - 힌팅(바이트코드 인스트럭션) 무시, 커닝 없음, 고정폭 셀 렌더링 전용
 *   - OpenType/CFF(.otf, 'OTTO') 는 감지만 하고 Nunsupported 로 거부한다
 *
 * 커널은 -mgeneral-regs-only 로 빌드되므로 부동소수점을 쓰지 않는다. 좌표는 1/256 픽셀 고정소수점이다.
 * 폰트 데이터는 신뢰하지 않는다: 모든 읽기는 경계 검사를 거친다 (망가진 폰트 → 오류, 커널 메모리 밖 접근 없음).
 * 래스터라이저는 정적 작업 버퍼를 쓰므로 재진입 불가 (콘솔 초기화 시 한 스레드에서만 호출한다).
 */
#ifndef CONSOLE_FONTS_TTF_H
#define CONSOLE_FONTS_TTF_H

#include "nyxis.h"

/* 래스터 출력 비트맵의 최대 크기 (셀 크기 상한) */
#define TTF_MAX_BITMAP_W 32U
#define TTF_MAX_BITMAP_H 64U

typedef struct {
    const u8 *data;
    usize     size;

    u32 glyf_off, glyf_len;
    u32 loca_off, loca_len;
    u32 hmtx_off, hmtx_len;
    u32 cmap4_off, cmap4_len;    /* cmap format 4 서브테이블 (파일 기준 오프셋) */

    u16 units_per_em;
    u16 num_glyphs;
    u16 num_hmetrics;
    i16 index_to_loc_format;     /* 0 = u16*2, 1 = u32 */
    i16 ascender;                /* hhea */
    i16 descender;               /* hhea (음수) */
} ttf_font;

/* 렌더링 변환: 픽셀 = 폰트 단위 * scale_num / scale_den, 원점(펜 위치/기준선)은 1/256 픽셀 */
typedef struct {
    i32 scale_num;
    i32 scale_den;
    i32 origin_x;                /* 1/256 px: 셀 왼쪽에서 글리프 원점까지 */
    i32 baseline_y;              /* 1/256 px: 셀 위쪽에서 기준선까지 */
} ttf_transform;

/* 폰트 파일 검증 + 테이블 위치 파악. OTF(CFF) 는 Nunsupported, 형식 오류는 Ncorrupted */
Nstatus ttf_init(ttf_font *font, const u8 *data, usize size);

/* 유니코드 코드포인트 -> 글리프 번호 (없으면 0 = .notdef) */
u16 ttf_glyph_index(const ttf_font *font, u32 codepoint);

/* 글리프의 가로 진행 폭 (폰트 단위) */
u16 ttf_advance_width(const ttf_font *font, u16 glyph);

/*
 * 글리프 하나를 w x h 알파 비트맵(0 = 투명, 255 = 완전 덮음, 행 우선)으로 그린다.
 * 셀 밖으로 나가는 부분은 잘린다. 빈 글리프(공백)는 0 으로 채우고 성공한다.
 */
Nstatus ttf_render_glyph(const ttf_font *font, u16 glyph, const ttf_transform *xf,
                         u8 *out, u32 w, u32 h);

#endif /* CONSOLE_FONTS_TTF_H */
