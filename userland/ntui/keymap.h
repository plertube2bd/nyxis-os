/*
 * keymap.h - 단축키 설정 (설정 keys): 파서, 형식, 찾기. 컴포지터(ntui)와 단축키 설정 창(nkeys)이 함께 쓴다.
 * 규칙과 파일 형식은 keymap.c 머리말과 rootfs/app/ntui/keys.
 */
#ifndef NTUI_KEYMAP_H
#define NTUI_KEYMAP_H

#include <stddef.h>
#include <nwp.h>

/* 키 값 (컴포지터가 창에 보내는 값과 같다: nwp.h) */
#define KEY_ESC        NWP_KEY_ESC
#define KEY_UP         NWP_KEY_UP
#define KEY_DOWN       NWP_KEY_DOWN
#define KEY_LEFT       NWP_KEY_LEFT
#define KEY_RIGHT      NWP_KEY_RIGHT
#define KEY_HOME       NWP_KEY_HOME
#define KEY_END        NWP_KEY_END
#define KEY_DEL        NWP_KEY_DEL
#define KEY_PGUP       NWP_KEY_PGUP
#define KEY_PGDN       NWP_KEY_PGDN
#define KEY_INS        NWP_KEY_INS
#define KEY_F1         NWP_KEY_F1
#define KEY_F2         NWP_KEY_F2
#define KEY_F3         NWP_KEY_F3
#define KEY_F4         NWP_KEY_F4
#define KEY_F5         NWP_KEY_F5
#define KEY_F6         NWP_KEY_F6
#define KEY_F7         NWP_KEY_F7
#define KEY_F8         NWP_KEY_F8
#define KEY_F9         NWP_KEY_F9
#define KEY_F10        NWP_KEY_F10
#define KEY_F11        NWP_KEY_F11
#define KEY_F12        NWP_KEY_F12
#define KEY_SUPER_TAP  NWP_KEY_SUPER_TAP
#define KEY_PRINT      NWP_KEY_PRINT
#define KEY_MENUKEY    NWP_KEY_MENUKEY
#define KEY_ALT_UP     NWP_KEY_ALT_UP
#define KEY_MOD_SHIFT  NWP_MOD_SHIFT
#define KEY_MOD_CTRL   NWP_MOD_CTRL
#define KEY_MOD_ALT    NWP_MOD_ALT
#define KEY_MOD_SUPER  NWP_MOD_SUPER
#define KEY_MODS       NWP_MODS
#define KEY_BASE(k)    NWP_KEY_BASE(k)
#define KEY_CTRL(c)    NWP_KEY_CTRL(c)

/* 컴포지터 동작 (단축키의 action, 메뉴) */
enum action {
    ACT_NONE, ACT_MENU, ACT_RUN, ACT_TERMINAL, ACT_FILES, ACT_CLOSE, ACT_MAXIMIZE, ACT_MINIMIZE,
    ACT_MOVE, ACT_NEXT, ACT_PREV, ACT_TASKBAR, ACT_THEME, ACT_HELP,
    ACT_SWITCH_NEXT, ACT_SWITCH_PREV, ACT_TASKVIEW, ACT_SNAP_LEFT, ACT_SNAP_RIGHT, ACT_RESTORE_MIN,
    ACT_FULLSCREEN, ACT_DESKTOP, ACT_MINIMIZE_ALL, ACT_RESTORE_ALL, ACT_WINMENU, ACT_RESIZE, ACT_LOCK,
    ACT_TASKMGR, ACT_SCREENSHOT, ACT_WINDOW, ACT_KEYS, ACT_RELOAD_KEYS,
    ACT_COUNT
};

#define KM_WINDOW_MAX 12                /* action window <n> 의 상한 */

#define KM_MAX   160                    /* 바인딩 수 (키 하나에 하나) */
#define KM_STMTS 4                      /* 바인딩 하나의 문장 수 */
#define KM_TEXT  96                     /* exec 명령 길이 */

/* 문장: 그 키를 무엇으로 보낼지 */
enum {
    KM_ACTION,                          /* ntui 동작 (act, arg) */
    KM_EXEC,                            /* 명령 실행 (text). nrunner/nterminal/nfiles/ntop 은 ntui 안의 것 */
    KM_PASS,                            /* 그 키를 그대로 활성 창의 프로그램에게 */
    KM_SEND,                            /* 다른 키(arg = 정규 키)를 활성 창의 프로그램에게 */
    KM_SIGNAL,                          /* 활성 터미널 창의 포그라운드 작업에 신호 (arg) */
    KM_NOTHING                          /* 아무것도 하지 않는다 (키를 먹는다) */
};

#define KM_MODE_ALL     0
#define KM_MODE_DISPLAY 1               /* 화면 모드에서만 */
#define KM_MODE_TTY     2               /* 터미널 모드에서만 */

struct km_stmt {
    int op;
    int act;
    int arg;
    char text[KM_TEXT];
};

struct km_binding {
    int key;                            /* 정규 키 (kmCanon) */
    int mode;                           /* KM_MODE_* */
    char app[16];                       /* 비어 있지 않으면 활성 창의 앱 이름이 이것일 때만 (예: nterminal) */
    int nstmt;
    struct km_stmt s[KM_STMTS];
};

int kmLoad(char *err, size_t cap);      /* 사용자 값 -> 기본값 -> 내장. 0 성공, -1 이면 err 에 이유 */
int kmFromUser(void);                   /* 지금 목록이 사용자 값에서 왔다 */
const struct km_binding *kmList(int *n);
/* app(활성 창의 앱 이름, 없으면 "")과 모드(display)에 맞는 바인딩 (없으면 NULL) */
const struct km_binding *kmLookup(int key, const char *app, int display);
int kmCanon(int key);                   /* 들어온 키 -> 정규 키 (Ctrl+글자 = 'a'..'z' + Ctrl, 대문자 = 소문자 + Shift) */
int kmRaw(int canon);                   /* 정규 키 -> 창에 넘길 키 */
void kmFormatKey(int key, int pretty, char *buf, size_t cap);   /* pretty: "Super+R", 아니면 "super+r" */
int kmParseKey(const char *s, int *out);
void kmFormatStmts(const struct km_binding *b, char *buf, size_t cap);
int kmParseStmts(const char *s, struct km_binding *b, char *err, size_t cap);
void kmDescribe(const struct km_binding *b, char *buf, size_t cap);
int kmStmtAction(const struct km_stmt *st);    /* 그 문장이 하는 ntui 동작 (exec nrunner = ACT_RUN 등), 아니면 ACT_NONE */
int kmModeApplies(int mode, int display);  /* display: 컴포지터가 화면을 직접 그린다 */
int kmSave(const struct km_binding *list, int n);  /* 사용자 값으로 저장. 0 / -1 (errno) */
int kmReset(void);                      /* 사용자 값을 지운다 (기본값으로) */

#endif /* NTUI_KEYMAP_H */
