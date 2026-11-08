/*
 * ttf.c - 최소 TrueType 파서 + 래스터라이저. 범위와 제약은 ttf.h 참고.
 *
 * 렌더링 파이프라인:
 *   1) cmap(format 4) 로 코드포인트 -> 글리프 번호
 *   2) loca 로 glyf 안의 글리프 위치를 찾고 윤곽선(점 + on/off-curve 플래그)을 디코드
 *   3) 점을 픽셀 좌표(1/256 px 고정소수점, y 아래 방향)로 변환
 *   4) 2차 베지어를 직선으로 평탄화 -> 간선 목록
 *   5) 픽셀당 RASTER_SS 개의 가로 샘플 줄마다 간선 교차점을 구해 non-zero 와인딩 규칙으로 구간을 채우고,
 *      구간이 픽셀을 가로로 덮는 정확한 길이를 누적 -> 알파 (가로는 정확, 세로는 RASTER_SS 배 슈퍼샘플)
 */

#include "nyxis.h"
#include "memory.h"
#include "console/fonts/ttf.h"

#define FX_ONE         256          /* 1 픽셀 = 256 고정소수점 단위 */
#define RASTER_SS      8            /* 픽셀당 세로 샘플 줄 수 */

#define TTF_MAX_POINTS    1024U     /* 글리프(컴포넌트) 하나의 점 개수 상한 */
#define TTF_MAX_CONTOURS  128U
#define TTF_MAX_EDGES     2048U
#define TTF_MAX_CROSSINGS 128U      /* 샘플 줄 하나에서의 교차점 상한 */
#define TTF_MAX_DEPTH     4U        /* compound glyph 중첩 상한 */

#define TAG(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

/* ===================== */
/* 경계 검사 읽기          */
/* ===================== */

static bool in_range(usize size, u32 off, u32 len)
{
    return (usize)off <= size && (usize)len <= size - (usize)off;
}

static u16 be16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* 순차 읽기 커서: 범위를 벗어나면 bad 를 세우고 0 을 돌려준다 (이후 읽기도 모두 0) */
typedef struct {
    const u8 *p;
    u32 len;
    u32 pos;
    bool bad;
} reader;

static void rd_init(reader *r, const u8 *p, u32 len)
{
    r->p = p;
    r->len = len;
    r->pos = 0;
    r->bad = false;
}

static bool rd_need(reader *r, u32 n)
{
    if (r->bad || n > r->len - r->pos) {
        r->bad = true;
        return false;
    }
    return true;
}

static u8 rd_u8(reader *r)
{
    if (!rd_need(r, 1))
        return 0;
    return r->p[r->pos++];
}

static u16 rd_u16(reader *r)
{
    u16 v;

    if (!rd_need(r, 2))
        return 0;
    v = be16(r->p + r->pos);
    r->pos += 2;
    return v;
}

static void rd_skip(reader *r, u32 n)
{
    if (rd_need(r, n))
        r->pos += n;
}

/* ===================== */
/* 초기화                  */
/* ===================== */

static bool find_table(const u8 *data, usize size, u16 num_tables, u32 tag, u32 *off, u32 *len)
{
    u16 i;

    for (i = 0; i < num_tables; i++) {
        const u8 *rec = data + 12 + (usize)i * 16;

        if (be32(rec) == tag) {
            *off = be32(rec + 8);
            *len = be32(rec + 12);
            return in_range(size, *off, *len);
        }
    }
    return false;
}

/* cmap 에서 유니코드 BMP format 4 서브테이블을 찾는다: (3,1) 우선, 다음 (0,*) */
static bool find_cmap4(ttf_font *f, u32 cmap_off, u32 cmap_len)
{
    const u8 *cmap = f->data + cmap_off;
    u16 n;
    u16 i;
    int pass;

    if (cmap_len < 4)
        return false;
    n = be16(cmap + 2);
    if ((u32)n > (cmap_len - 4) / 8)
        return false;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < n; i++) {
            const u8 *rec = cmap + 4 + (usize)i * 8;
            u16 plat = be16(rec);
            u16 enc = be16(rec + 2);
            u32 sub = be32(rec + 4);
            u32 sub_len;

            if (pass == 0 && !(plat == 3 && enc == 1))
                continue;
            if (pass == 1 && plat != 0)
                continue;
            if (sub > cmap_len || cmap_len - sub < 4)
                continue;
            if (be16(cmap + sub) != 4)
                continue;
            sub_len = be16(cmap + sub + 2);
            if (sub_len < 16 || sub_len > cmap_len - sub)
                continue;

            f->cmap4_off = cmap_off + sub;
            f->cmap4_len = sub_len;
            return true;
        }
    }
    return false;
}

Nstatus ttf_init(ttf_font *f, const u8 *data, usize size)
{
    u32 version;
    u16 num_tables;
    u32 off;
    u32 len;
    u32 cmap_off;
    u32 cmap_len;

    if (!f || !data || size < 12 || size > 0x7FFFFFFFUL)
        return NinvalidArg;

    memset(f, 0, sizeof(*f));
    f->data = data;
    f->size = size;

    version = be32(data);
    if (version == TAG('O', 'T', 'T', 'O'))
        return Nunsupported;                 /* CFF 윤곽선(.otf): 아직 미지원 */
    if (version != 0x00010000U && version != TAG('t', 'r', 'u', 'e'))
        return Ncorrupted;

    num_tables = be16(data + 4);
    if (num_tables == 0 || (usize)num_tables > (size - 12) / 16)
        return Ncorrupted;

    /* head: unitsPerEm, indexToLocFormat */
    if (!find_table(data, size, num_tables, TAG('h', 'e', 'a', 'd'), &off, &len) || len < 54)
        return Ncorrupted;
    if (be32(data + off + 12) != 0x5F0F3CF5U)
        return Ncorrupted;
    f->units_per_em = be16(data + off + 18);
    f->index_to_loc_format = (i16)be16(data + off + 50);
    if (f->units_per_em < 16 || f->units_per_em > 16384)
        return Ncorrupted;
    if (f->index_to_loc_format != 0 && f->index_to_loc_format != 1)
        return Ncorrupted;

    /* maxp: numGlyphs */
    if (!find_table(data, size, num_tables, TAG('m', 'a', 'x', 'p'), &off, &len) || len < 6)
        return Ncorrupted;
    f->num_glyphs = be16(data + off + 4);
    if (f->num_glyphs == 0)
        return Ncorrupted;

    /* hhea: ascender, descender, numberOfHMetrics */
    if (!find_table(data, size, num_tables, TAG('h', 'h', 'e', 'a'), &off, &len) || len < 36)
        return Ncorrupted;
    f->ascender = (i16)be16(data + off + 4);
    f->descender = (i16)be16(data + off + 6);
    f->num_hmetrics = be16(data + off + 34);
    if (f->ascender <= f->descender || f->num_hmetrics == 0 || f->num_hmetrics > f->num_glyphs)
        return Ncorrupted;

    if (!find_table(data, size, num_tables, TAG('h', 'm', 't', 'x'), &f->hmtx_off, &f->hmtx_len))
        return Ncorrupted;
    if (f->hmtx_len / 4 < f->num_hmetrics)
        return Ncorrupted;

    if (!find_table(data, size, num_tables, TAG('l', 'o', 'c', 'a'), &f->loca_off, &f->loca_len))
        return Ncorrupted;
    if (f->loca_len / (f->index_to_loc_format ? 4U : 2U) < (u32)f->num_glyphs + 1U)
        return Ncorrupted;

    if (!find_table(data, size, num_tables, TAG('g', 'l', 'y', 'f'), &f->glyf_off, &f->glyf_len)) {
        /* glyf 가 없으면 TrueType 윤곽선이 아니다 (예: 'true' 버전의 CFF/비트맵 전용 폰트) */
        return Nunsupported;
    }

    if (!find_table(data, size, num_tables, TAG('c', 'm', 'a', 'p'), &cmap_off, &cmap_len))
        return Ncorrupted;
    if (!find_cmap4(f, cmap_off, cmap_len))
        return Nunsupported;

    return NSTATUS_OK;
}

/* ===================== */
/* cmap / hmtx            */
/* ===================== */

u16 ttf_glyph_index(const ttf_font *f, u32 cp)
{
    const u8 *t;
    u16 seg_count;
    u32 i;

    if (!f || !f->data || cp > 0xFFFFU)
        return 0;

    t = f->data + f->cmap4_off;
    seg_count = (u16)(be16(t + 6) / 2);
    /* endCode[seg] + pad(2) + startCode[seg] + idDelta[seg] + idRangeOffset[seg] */
    if (seg_count == 0 || 14U + 8U * (u32)seg_count + 2U > f->cmap4_len)
        return 0;

    for (i = 0; i < seg_count; i++) {
        u32 end_code = be16(t + 14 + i * 2);
        u32 start_off = 14 + (u32)seg_count * 2 + 2 + i * 2;
        u32 start_code = be16(t + start_off);
        u16 delta = be16(t + start_off + (u32)seg_count * 2);
        u32 range_pos = start_off + (u32)seg_count * 4;
        u16 range = be16(t + range_pos);
        u32 g;

        if (cp > end_code)
            continue;
        if (cp < start_code)
            return 0;

        if (range == 0)
            return (u16)(cp + delta);

        /* idRangeOffset 는 자기 자신의 위치 기준 바이트 오프셋 */
        g = range_pos + range + (cp - start_code) * 2;
        if (g + 2 > f->cmap4_len)
            return 0;
        g = be16(t + g);
        return g ? (u16)(g + delta) : 0;
    }
    return 0;
}

u16 ttf_advance_width(const ttf_font *f, u16 glyph)
{
    u32 idx;

    if (!f || !f->data)
        return 0;
    idx = glyph < f->num_hmetrics ? glyph : (u32)f->num_hmetrics - 1U;
    return be16(f->data + f->hmtx_off + idx * 4);
}

/* glyf 테이블 안에서 글리프 데이터 범위. 빈 글리프는 *len = 0 */
static bool glyph_range(const ttf_font *f, u16 glyph, u32 *off, u32 *len)
{
    const u8 *loca = f->data + f->loca_off;
    u32 a;
    u32 b;

    if (glyph >= f->num_glyphs)
        return false;

    if (f->index_to_loc_format == 0) {
        a = (u32)be16(loca + (u32)glyph * 2) * 2U;
        b = (u32)be16(loca + (u32)glyph * 2 + 2) * 2U;
    } else {
        a = be32(loca + (u32)glyph * 4);
        b = be32(loca + (u32)glyph * 4 + 4);
    }

    if (b < a || b > f->glyf_len)
        return false;
    *off = a;
    *len = b - a;
    return true;
}

/* ===================== */
/* 윤곽선 -> 간선          */
/* ===================== */

typedef struct {
    i32 x0, y0, x1, y1;   /* y0 < y1 */
    i32 dir;              /* 원래 방향: +1 아래로, -1 위로 */
} edge;

static edge g_edges[TTF_MAX_EDGES];
static u32  g_nedges;
static bool g_edge_overflow;

static i32  g_px[TTF_MAX_POINTS];
static i32  g_py[TTF_MAX_POINTS];
static u8   g_on[TTF_MAX_POINTS];
static u16  g_end_pts[TTF_MAX_CONTOURS];

static void add_line(i32 x0, i32 y0, i32 x1, i32 y1)
{
    edge *e;

    if (y0 == y1)
        return;                       /* 수평선은 교차에 기여하지 않는다 */
    if (g_nedges >= TTF_MAX_EDGES) {
        g_edge_overflow = true;
        return;
    }
    e = &g_edges[g_nedges++];
    if (y0 < y1) {
        e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1; e->dir = 1;
    } else {
        e->x0 = x1; e->y0 = y1; e->x1 = x0; e->y1 = y0; e->dir = -1;
    }
}

static i32 iabs32(i32 v)
{
    return v < 0 ? -v : v;
}

static u32 isqrt32(u32 v)
{
    u32 r = 0;

    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

/* 2차 베지어 (p0, c, p1) 를 직선 n 개로 평탄화 */
static void add_quad(i32 x0, i32 y0, i32 cx, i32 cy, i32 x1, i32 y1)
{
    /* 평탄화 오차 ~ |p0 - 2c + p1| / (8 n^2). 오차 < ~0.1 px 가 되도록 n 을 고른다 */
    u32 dd = (u32)iabs32(x0 - 2 * cx + x1) + (u32)iabs32(y0 - 2 * cy + y1);
    u32 n = isqrt32(dd / 200U) + 1U;
    u32 i;
    i32 px = x0;
    i32 py = y0;

    if (n > 16U)
        n = 16U;

    for (i = 1; i <= n; i++) {
        i64 t = (i64)i;
        i64 u = (i64)(n - i);
        i64 nn = (i64)n * (i64)n;
        i32 qx = (i32)((u * u * x0 + 2 * u * t * cx + t * t * x1) / nn);
        i32 qy = (i32)((u * u * y0 + 2 * u * t * cy + t * t * y1) / nn);

        add_line(px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

/* 윤곽선 하나 (점 s..e, 포함) 를 간선으로. 연속된 off-curve 점 사이에는 암묵적인 on-curve 중점이 있다 */
static void emit_contour(u32 s, u32 e)
{
    i32 sx, sy;          /* 시작(=끝) on-curve 점 */
    i32 cur_x, cur_y;
    i32 ctl_x = 0, ctl_y = 0;
    bool has_ctl = false;
    u32 i;
    u32 first;
    u32 last;

    if (e <= s)
        return;          /* 점 1개짜리 윤곽선은 면적이 없다 */

    if (g_on[s]) {
        sx = g_px[s]; sy = g_py[s];
        first = s + 1; last = e;
    } else if (g_on[e]) {
        sx = g_px[e]; sy = g_py[e];
        first = s; last = e - 1;
    } else {
        sx = (g_px[s] + g_px[e]) / 2; sy = (g_py[s] + g_py[e]) / 2;
        first = s; last = e;
    }

    cur_x = sx;
    cur_y = sy;

    for (i = first; i <= last; i++) {
        i32 x = g_px[i];
        i32 y = g_py[i];

        if (g_on[i]) {
            if (has_ctl)
                add_quad(cur_x, cur_y, ctl_x, ctl_y, x, y);
            else
                add_line(cur_x, cur_y, x, y);
            cur_x = x; cur_y = y;
            has_ctl = false;
        } else {
            if (has_ctl) {
                i32 mx = (ctl_x + x) / 2;
                i32 my = (ctl_y + y) / 2;

                add_quad(cur_x, cur_y, ctl_x, ctl_y, mx, my);
                cur_x = mx; cur_y = my;
            }
            ctl_x = x; ctl_y = y;
            has_ctl = true;
        }
    }

    if (has_ctl)
        add_quad(cur_x, cur_y, ctl_x, ctl_y, sx, sy);
    else
        add_line(cur_x, cur_y, sx, sy);
}

/* 폰트 단위 (ux, uy) -> 1/256 px (y 아래 방향) */
static i32 map_x(const ttf_transform *xf, i32 ux)
{
    return xf->origin_x + (i32)((i64)ux * xf->scale_num * FX_ONE / xf->scale_den);
}

static i32 map_y(const ttf_transform *xf, i32 uy)
{
    return xf->baseline_y - (i32)((i64)uy * xf->scale_num * FX_ONE / xf->scale_den);
}

/* simple glyph: 윤곽선 디코드 후 (dx, dy) 폰트 단위만큼 옮겨 간선 추가 */
static Nstatus decode_simple(const u8 *g, u32 len, i16 ncont, i32 dx, i32 dy, const ttf_transform *xf)
{
    reader r;
    u32 npts;
    u32 i;
    u16 ninstr;
    i32 acc;
    u32 s;

    if ((u32)ncont > TTF_MAX_CONTOURS)
        return Noverflow;

    rd_init(&r, g, len);
    rd_skip(&r, 10);                              /* numberOfContours + bbox */

    for (i = 0; i < (u32)ncont; i++) {
        g_end_pts[i] = rd_u16(&r);
        if (i > 0 && g_end_pts[i] <= g_end_pts[i - 1])
            return Ncorrupted;
    }
    if (r.bad)
        return Ncorrupted;
    if (ncont == 0)
        return NSTATUS_OK;

    npts = (u32)g_end_pts[ncont - 1] + 1U;
    if (npts > TTF_MAX_POINTS)
        return Noverflow;

    ninstr = rd_u16(&r);
    rd_skip(&r, ninstr);                          /* 힌팅 인스트럭션: 무시 */

    /* 플래그 (REPEAT 압축) — 일단 g_on[] 에 원본 플래그를 담아 둔다 */
    for (i = 0; i < npts; ) {
        u8 fl = rd_u8(&r);

        g_on[i++] = fl;
        if (fl & 0x08) {
            u8 rep = rd_u8(&r);

            while (rep-- > 0 && i < npts)
                g_on[i++] = fl;
        }
        if (r.bad)
            return Ncorrupted;
    }

    /* x 좌표 (델타) */
    acc = 0;
    for (i = 0; i < npts; i++) {
        u8 fl = g_on[i];

        if (fl & 0x02) {
            i32 d = rd_u8(&r);
            acc += (fl & 0x10) ? d : -d;
        } else if (!(fl & 0x10)) {
            acc += (i16)rd_u16(&r);
        }
        g_px[i] = acc;
    }

    /* y 좌표 (델타) */
    acc = 0;
    for (i = 0; i < npts; i++) {
        u8 fl = g_on[i];

        if (fl & 0x04) {
            i32 d = rd_u8(&r);
            acc += (fl & 0x20) ? d : -d;
        } else if (!(fl & 0x20)) {
            acc += (i16)rd_u16(&r);
        }
        g_py[i] = acc;
    }
    if (r.bad)
        return Ncorrupted;

    for (i = 0; i < npts; i++) {
        g_px[i] = map_x(xf, g_px[i] + dx);
        g_py[i] = map_y(xf, g_py[i] + dy);
        g_on[i] = (u8)(g_on[i] & 0x01);
    }

    s = 0;
    for (i = 0; i < (u32)ncont; i++) {
        emit_contour(s, g_end_pts[i]);
        s = (u32)g_end_pts[i] + 1U;
    }

    return g_edge_overflow ? Noverflow : NSTATUS_OK;
}

static Nstatus decode_glyph(const ttf_font *f, u16 glyph, i32 dx, i32 dy, const ttf_transform *xf, u32 depth);

/* compound glyph: 1단계에서는 x/y 오프셋만 지원. 스케일/회전 행렬이 있는 컴포넌트는 Nunsupported */
static Nstatus decode_compound(const ttf_font *f, const u8 *g, u32 len, i32 dx, i32 dy,
                               const ttf_transform *xf, u32 depth)
{
    reader r;
    u16 flags;

    rd_init(&r, g, len);
    rd_skip(&r, 10);

    do {
        u16 comp;
        i32 ax;
        i32 ay;
        Nstatus st;

        flags = rd_u16(&r);
        comp = rd_u16(&r);
        if (flags & 0x0001) {                     /* ARG_1_AND_2_ARE_WORDS */
            ax = (i16)rd_u16(&r);
            ay = (i16)rd_u16(&r);
        } else {
            ax = (i8)rd_u8(&r);
            ay = (i8)rd_u8(&r);
        }
        if (r.bad)
            return Ncorrupted;
        if (!(flags & 0x0002))                    /* ARGS_ARE_XY_VALUES 아님 = 점 맞추기 */
            return Nunsupported;
        if (flags & (0x0008 | 0x0040 | 0x0080))   /* SCALE / X_AND_Y_SCALE / TWO_BY_TWO */
            return Nunsupported;

        st = decode_glyph(f, comp, dx + ax, dy + ay, xf, depth + 1U);
        if (NSTATUS_IS_ERR(st))
            return st;
    } while (flags & 0x0020);                     /* MORE_COMPONENTS */

    return NSTATUS_OK;
}

static Nstatus decode_glyph(const ttf_font *f, u16 glyph, i32 dx, i32 dy, const ttf_transform *xf, u32 depth)
{
    u32 off;
    u32 len;
    const u8 *g;
    i16 ncont;

    if (depth > TTF_MAX_DEPTH)
        return Noverflow;
    if (!glyph_range(f, glyph, &off, &len))
        return Ncorrupted;
    if (len == 0)
        return NSTATUS_OK;                        /* 빈 글리프 (공백 등) */
    if (len < 10)
        return Ncorrupted;

    g = f->data + f->glyf_off + off;
    ncont = (i16)be16(g);

    if (ncont >= 0)
        return decode_simple(g, len, ncont, dx, dy, xf);
    return decode_compound(f, g, len, dx, dy, xf, depth);
}

/* ===================== */
/* 스캔라인 채우기         */
/* ===================== */

typedef struct {
    i32 x;
    i32 dir;
} crossing;

static u16 g_acc[TTF_MAX_BITMAP_W];
static crossing g_cross[TTF_MAX_CROSSINGS];

/* 고정소수점 구간 [xa, xb) 가 각 픽셀을 가로로 덮는 길이를 누적 */
static void fill_span(u32 w, i32 xa, i32 xb)
{
    i32 maxx = (i32)w * FX_ONE;
    i32 ia;
    i32 ib;
    i32 i;

    if (xa < 0) xa = 0;
    if (xb > maxx) xb = maxx;
    if (xa >= xb)
        return;

    ia = xa / FX_ONE;
    ib = (xb - 1) / FX_ONE;
    if (ia == ib) {
        g_acc[ia] = (u16)(g_acc[ia] + (xb - xa));
        return;
    }
    g_acc[ia] = (u16)(g_acc[ia] + (FX_ONE - (xa - ia * FX_ONE)));
    for (i = ia + 1; i < ib; i++)
        g_acc[i] = (u16)(g_acc[i] + FX_ONE);
    g_acc[ib] = (u16)(g_acc[ib] + (xb - ib * FX_ONE));
}

static void rasterize(u8 *out, u32 w, u32 h)
{
    u32 row;

    for (row = 0; row < h; row++) {
        u32 sub;
        u32 col;

        memset(g_acc, 0, sizeof(g_acc));

        for (sub = 0; sub < RASTER_SS; sub++) {
            /* 샘플 줄의 y (각 서브 줄의 중앙) */
            i32 sy = (i32)(row * FX_ONE) + (i32)((sub * FX_ONE + FX_ONE / 2) / RASTER_SS);
            u32 nc = 0;
            u32 i;
            i32 wind;
            i32 span_start;

            for (i = 0; i < g_nedges; i++) {
                const edge *e = &g_edges[i];
                u32 j;
                i32 x;

                if (sy < e->y0 || sy >= e->y1)
                    continue;
                if (nc >= TTF_MAX_CROSSINGS)
                    break;

                x = e->x0 + (i32)((i64)(sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0));

                /* 삽입 정렬 (교차점은 보통 몇 개뿐) */
                j = nc;
                while (j > 0 && g_cross[j - 1].x > x) {
                    g_cross[j] = g_cross[j - 1];
                    j--;
                }
                g_cross[j].x = x;
                g_cross[j].dir = e->dir;
                nc++;
            }

            /* non-zero 와인딩 */
            wind = 0;
            span_start = 0;
            for (i = 0; i < nc; i++) {
                i32 prev = wind;

                wind += g_cross[i].dir;
                if (prev == 0 && wind != 0)
                    span_start = g_cross[i].x;
                else if (prev != 0 && wind == 0)
                    fill_span(w, span_start, g_cross[i].x);
            }
        }

        /* 누적값 최대 = FX_ONE * RASTER_SS -> 0..255 */
        for (col = 0; col < w; col++) {
            u32 a = ((u32)g_acc[col] * 255U + (FX_ONE * RASTER_SS) / 2U) / (FX_ONE * RASTER_SS);

            out[row * w + col] = (u8)(a > 255U ? 255U : a);
        }
    }
}

Nstatus ttf_render_glyph(const ttf_font *f, u16 glyph, const ttf_transform *xf,
                         u8 *out, u32 w, u32 h)
{
    Nstatus st;

    if (!f || !f->data || !xf || !out || !w || !h)
        return NinvalidArg;
    if (w > TTF_MAX_BITMAP_W || h > TTF_MAX_BITMAP_H)
        return NinvalidArg;
    if (xf->scale_num <= 0 || xf->scale_den <= 0)
        return NinvalidArg;

    memset(out, 0, (usize)w * h);

    g_nedges = 0;
    g_edge_overflow = false;

    st = decode_glyph(f, glyph, 0, 0, xf, 0);
    if (NSTATUS_IS_ERR(st))
        return st;

    rasterize(out, w, h);
    return NSTATUS_OK;
}
