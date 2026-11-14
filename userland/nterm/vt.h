/*
 * vt.h - nterm 의 터미널 에뮬레이터 (VT100 / xterm 의 부분집합, 셀 격자 + 스크롤백)
 */
#ifndef NTERM_VT_H
#define NTERM_VT_H

#include <nwp.h>

/* 키 값 (컴포지터가 보내는 값: nwp.h) */
#define KEY_ESC       NWP_KEY_ESC
#define KEY_UP        NWP_KEY_UP
#define KEY_DOWN      NWP_KEY_DOWN
#define KEY_LEFT      NWP_KEY_LEFT
#define KEY_RIGHT     NWP_KEY_RIGHT
#define KEY_HOME      NWP_KEY_HOME
#define KEY_END       NWP_KEY_END
#define KEY_DEL       NWP_KEY_DEL
#define KEY_PGUP      NWP_KEY_PGUP
#define KEY_PGDN      NWP_KEY_PGDN
#define KEY_INS       NWP_KEY_INS
#define KEY_F1        NWP_KEY_F1
#define KEY_F2        NWP_KEY_F2
#define KEY_F3        NWP_KEY_F3
#define KEY_F4        NWP_KEY_F4
#define KEY_F5        NWP_KEY_F5
#define KEY_F6        NWP_KEY_F6
#define KEY_F7        NWP_KEY_F7
#define KEY_F8        NWP_KEY_F8
#define KEY_F9        NWP_KEY_F9
#define KEY_F10       NWP_KEY_F10
#define KEY_F11       NWP_KEY_F11
#define KEY_F12       NWP_KEY_F12
#define KEY_MOD_SHIFT NWP_MOD_SHIFT
#define KEY_MOD_CTRL  NWP_MOD_CTRL
#define KEY_MOD_ALT   NWP_MOD_ALT
#define KEY_MOD_SUPER NWP_MOD_SUPER
#define KEY_MODS      NWP_MODS
#define KEY_BASE(k)   NWP_KEY_BASE(k)
#define KEY_CTRL(c)   NWP_KEY_CTRL(c)

#define ATTR_BOLD      NWP_ATTR_BOLD
#define ATTR_REVERSE   NWP_ATTR_REVERSE
#define ATTR_UNDERLINE NWP_ATTR_UNDERLINE

#define VT_ATTR_BOLD      ATTR_BOLD
#define VT_ATTR_REVERSE   ATTR_REVERSE
#define VT_ATTR_UNDERLINE ATTR_UNDERLINE
#define VT_ATTR_FGDEF     0x10U         /* 기본 글자색 (그릴 때 창 색으로) */
#define VT_ATTR_BGDEF     0x20U         /* 기본 배경색 */

struct vtcell {
    unsigned short ch;
    unsigned char fg, bg;               /* 256색 번호 (VT_ATTR_*DEF 면 무시) */
    unsigned char attr;
};

struct vt;

/* 응답(커서 위치 보고 등)을 쓸 곳. ctx 는 vtNew 에 준 값 */
typedef void (*vt_reply_fn)(void *ctx, const char *s, int n);

/* 창 조작 (CSI Ps ; Ps ; Ps t: xterm 의 XTWINOPS). params 는 CSI 의 숫자들 (n >= 1) */
typedef void (*vt_winop_fn)(void *ctx, const int *params, int n);
/* 창 제목 (OSC 0 / OSC 2). 출력할 수 없는 글자는 '?' 로 바꿔 둔다 */
typedef void (*vt_title_fn)(void *ctx, const char *title);

struct vt *vtNew(int cols, int rows, vt_reply_fn reply, void *ctx);
void vtSetWindowOps(struct vt *v, vt_winop_fn winop, vt_title_fn title);   /* ctx 는 vtNew 의 것 */
void vtFree(struct vt *v);
void vtResize(struct vt *v, int cols, int rows);
void vtWrite(struct vt *v, const char *buf, int n);    /* 프로그램 출력을 해석한다 */
int vtCols(const struct vt *v);
int vtRows(const struct vt *v);
/* 보이는 줄 y (0..rows-1) 의 셀. scroll 만큼 스크롤백으로 올려 본다 (0 = 지금 화면) */
const struct vtcell *vtLine(const struct vt *v, int y, int scroll);
int vtScrollback(const struct vt *v);  /* 올려 볼 수 있는 줄 수 (보조 화면이면 0) */
int vtCursor(const struct vt *v, int *x, int *y);  /* 커서가 보이면 1 */
int vtAppCursorKeys(const struct vt *v);           /* DECCKM: 방향키를 ESC O A 로 */
/* 키를 프로그램에 보낼 바이트로 바꾼다. 바이트 수 (보낼 것이 없으면 0) */
int vtEncodeKey(const struct vt *v, int key, char *out, int cap);

#endif /* NTERM_VT_H */
