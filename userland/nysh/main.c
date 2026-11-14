/*
 * nysh - Nyxis 셸 (nlibc)
 *
 *   ~* cd tmp:/
 *   (프롬프트가 tmp:/ 와 별표로 바뀐다)  echo "hello $USER" > hi.txt && cat hi.txt | wc -c
 *   mkdir work; cd work
 *   work* ls *.txt
 *
 * 프롬프트: 현재 디렉터리의 마지막 이름 + "* ". 홈(face:/)은 "~", 이름공간의 뿌리는 "tmp:/" 처럼 전체
 * 색: stderr 가 터미널이면 프롬프트(디렉터리 굵은 파랑, "*" 는 직전 명령이 성공이면 초록/실패면 빨강)와
 *     nysh 오류 메시지(빨강)에 ANSI 색을 쓴다. NO_COLOR(비어 있지 않음) 나 TERM=dumb 이면 끈다.
 * (예: "~* ", "work* ", "dev:/" 뒤에 "* ").
 *
 * 문법:
 *   명령 목록   a ; b      a && b      a || b      a &  (백그라운드)
 *   파이프      a | b | c
 *   리디렉션    < 파일   > 파일   >> 파일   2> 파일   2>> 파일   2>&1   >&2
 *   따옴표      '그대로'   "변수 $X 는 펼침"   \ 다음 글자 그대로
 *   변수        $NAME ${NAME} $? (마지막 종료 코드) $$ (셸 pid). NAME=값 (셸 변수 = 환경 변수),
 *               NAME=값 명령 (그 명령에만)
 *   ~           단어 맨 앞의 ~ 는 $HOME (기본 face:/)
 *   글롭        * ? : 마지막 경로 구성요소에서만 펼친다 (맞는 것이 없으면 그대로). '.' 로 시작하는 이름은
 *               패턴도 '.' 로 시작할 때만
 *   주석        # 부터 줄 끝까지
 * 내장 명령: cd [dir|-], pwd, exit [n], export [NAME=값...], unset NAME..., env, source 파일 (또는 .),
 *            whoami, help
 * 그 밖의 명령은 이름만이면 app:/<이름>.run, 경로면 그 파일을 실행한다 (nlibc execvp).
 *
 * Ctrl-C: 대화형이면 포그라운드 작업(파이프라인 하나)마다 새 프로세스 그룹을 만들고 콘솔의 포그라운드 그룹으로
 *         정한다 (tcsetpgrp). 콘솔이 Ctrl-C 를 받으면 커널이 그 그룹에 SIGINT 를 보낸다 (Ctrl-\ 는 SIGQUIT):
 *         처리기가 없는 프로그램은 끝나고 $? 는 130 (SIGQUIT 은 131). 프롬프트에서는 포그라운드 그룹이 없으므로(0)
 *         쓰던 줄만 버려진다. 백그라운드 작업(&)과 셸 자신은 신호를 받지 않는다.
 *
 * 설정 (nx64api nx64_setting_*: 사용자 값 roi:/<uid>/app/nysh/<키> 가 기본값 app:/nysh.run/<키> 보다 먼저):
 *   mark   프롬프트 기호 (기본 "*", 출력 가능한 ASCII 1~8 자)
 *   rc     대화형 셸이 시작할 때 실행하는 명령들. 기본(app:/nysh.run/rc)을 먼저, 그다음 사용자 것
 *          (roi:/<uid>/app/nysh/rc) 을 실행한다 (둘 다 있으면 둘 다: 시스템 설정 위에 사용자 설정)
 *
 * 실행: nysh            대화형 (표준 입력이 콘솔이면 프롬프트를 보인다)
 *       nysh -c 명령    한 줄 실행
 *       nysh 파일       파일의 줄들을 실행
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <nx64.h>

#define LINE_MAX_LEN 1024
#define MAX_TOKENS   256
#define POOL_SIZE    16384
#define MAX_ARGS     128
#define MAX_CMDS     16
#define MAX_REDIRS   8
#define MAX_ASSIGNS  16
#define MAX_BG       16

extern char **environ;

/* ------------------------------------------------------------------ */
/* 토큰                                                                */
/* ------------------------------------------------------------------ */

enum {
    T_WORD, T_PIPE, T_OR, T_AND, T_SEMI, T_BG,
    T_IN, T_OUT, T_APPEND, T_ERR, T_ERRAPP, T_ERR2OUT, T_OUT2ERR,
    T_END
};

struct token {
    int type;
    char *text;     /* T_WORD: 따옴표/변수를 처리한 글자 */
    char *glob;     /* T_WORD: 글자마다 '1' 이면 따옴표 밖의 * ? (글롭 문자). 없으면 NULL */
};

/* 한 줄의 토큰과 그 글자들. source 가 실행 도중 다른 줄들을 실행하므로 줄마다 따로 만든다 */
struct lexer {
    struct token tok[MAX_TOKENS];
    int ntok;
    char pool[POOL_SIZE];       /* 토큰 글자와 펼친 인자 */
    size_t used;
};

static struct lexer *g_lex;     /* 지금 실행 중인 줄 */
#define g_tok       (g_lex->tok)
#define g_ntok      (g_lex->ntok)
#define g_pool      (g_lex->pool)
#define g_pool_used (g_lex->used)

static int g_last = 0;              /* $? */
static int g_interactive = 0;
static int g_color = 0;             /* 오류/프롬프트에 ANSI 색 (stderr 가 터미널이고 NO_COLOR 가 없을 때) */
static pid_t g_bg[MAX_BG];

#define C_ERR    "\033[31m"
#define C_RESET  "\033[0m"

/* 오류 줄의 앞/뒤: 색을 켜고 끈다 (색이 꺼져 있으면 아무것도 찍지 않는다) */
static void err_begin(void)
{
    if (g_color)
        fputs(C_ERR, stderr);
}

static void err_end(void)
{
    if (g_color)
        fputs(C_RESET, stderr);
    fputc('\n', stderr);
}

static void shell_error(const char *what, const char *msg)
{
    err_begin();
    fprintf(stderr, "nysh: %s: %s", what, msg);
    err_end();
}

/* POSIX 관례: NO_COLOR 가 비어 있지 않으면, 또는 TERM=dumb 이면 색을 쓰지 않는다 */
static int want_color(void)
{
    const char *nc = getenv("NO_COLOR");
    const char *term = getenv("TERM");

    if (nc && nc[0])
        return 0;
    if (term && strcmp(term, "dumb") == 0)
        return 0;
    return isatty(2);
}

static char *pool_alloc(size_t n)
{
    char *p;

    if (g_pool_used + n > POOL_SIZE)
        return NULL;
    p = g_pool + g_pool_used;
    g_pool_used += n;
    return p;
}

/* 지금 만드는 단어 (풀의 끝에 이어 붙인다) */
struct wbuf {
    char text[LINE_MAX_LEN * 2];
    char glob[LINE_MAX_LEN * 2];
    size_t len;
    int any_glob;
    int overflow;
};

static void wput(struct wbuf *w, char c, int is_glob)
{
    if (w->len + 1 >= sizeof(w->text)) {
        w->overflow = 1;
        return;
    }
    w->text[w->len] = c;
    w->glob[w->len] = is_glob ? '1' : '0';
    w->len++;
    if (is_glob)
        w->any_glob = 1;
}

static void wputs(struct wbuf *w, const char *s)
{
    while (s && *s)
        wput(w, *s++, 0);
}

static int is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

/* s 가 '$' 를 가리킬 때 변수를 펼쳐 w 에 넣고, 소비한 글자 수를 돌려준다 */
static size_t expand_var(const char *s, struct wbuf *w)
{
    char name[128];
    char num[24];
    size_t i = 1;
    size_t n = 0;
    int braced = 0;

    if (s[1] == '?') {
        sprintf(num, "%d", g_last);
        wputs(w, num);
        return 2;
    }
    if (s[1] == '$') {
        sprintf(num, "%ld", (long)getpid());
        wputs(w, num);
        return 2;
    }
    if (s[1] == '{') {
        braced = 1;
        i = 2;
    }
    if (!is_name_start(s[i])) {
        wput(w, '$', 0);                /* 변수 이름이 아니면 '$' 그대로 */
        return 1;
    }
    while (is_name_char(s[i]) && n + 1 < sizeof(name))
        name[n++] = s[i++];
    name[n] = '\0';
    if (braced) {
        if (s[i] != '}') {
            wput(w, '$', 0);
            return 1;
        }
        i++;
    }
    wputs(w, getenv(name));
    return i;
}

static int is_op_char(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

static int add_token(int type, const char *raw, size_t n)
{
    struct token *t;

    if (g_ntok >= MAX_TOKENS - 1) {
        shell_error("syntax", "too many words");
        return -1;
    }
    t = &g_tok[g_ntok++];
    t->type = type;
    t->text = NULL;
    t->glob = NULL;
    if (raw) {
        t->text = pool_alloc(n + 1);
        if (!t->text) {
            shell_error("syntax", "line too long");
            return -1;
        }
        memcpy(t->text, raw, n);
        t->text[n] = '\0';
    }
    return 0;
}

/*
 * 한 줄을 토큰으로 나눈다. 단어는 원문 그대로(따옴표 포함) 둔다: 변수와 ~ 는 그 명령을 실행하기 직전에
 * 펼친다 (expand_raw). 그래야 "X=1; echo $X" 의 $X 가 앞 명령의 결과를 본다.
 * 성공 0, 문법 오류 -1 (메시지 출력)
 */
static int tokenize(const char *s)
{
    g_ntok = 0;
    g_pool_used = 0;

    for (;;) {
        const char *start;

        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        if (*s == '\0' || *s == '#')
            break;

        /* 연산자 */
        if (s[0] == '2' && s[1] == '>' && s[2] == '&' && s[3] == '1') {
            if (add_token(T_ERR2OUT, NULL, 0) < 0) return -1;
            s += 4;
            continue;
        }
        if (s[0] == '2' && s[1] == '>' && s[2] == '>') {
            if (add_token(T_ERRAPP, NULL, 0) < 0) return -1;
            s += 3;
            continue;
        }
        if (s[0] == '2' && s[1] == '>') {
            if (add_token(T_ERR, NULL, 0) < 0) return -1;
            s += 2;
            continue;
        }
        if ((s[0] == '>' && s[1] == '&' && s[2] == '2') || (s[0] == '1' && s[1] == '>' && s[2] == '&' && s[3] == '2')) {
            if (add_token(T_OUT2ERR, NULL, 0) < 0) return -1;
            s += (s[0] == '1') ? 4 : 3;
            continue;
        }
        if (s[0] == '1' && s[1] == '>') {
            s++;                        /* "1>" 는 ">" 와 같다 */
            continue;
        }
        if (is_op_char(*s)) {
            int type;

            if (s[0] == '|' && s[1] == '|') { type = T_OR; s += 2; }
            else if (s[0] == '&' && s[1] == '&') { type = T_AND; s += 2; }
            else if (s[0] == '>' && s[1] == '>') { type = T_APPEND; s += 2; }
            else if (s[0] == '|') { type = T_PIPE; s++; }
            else if (s[0] == '&') { type = T_BG; s++; }
            else if (s[0] == ';') { type = T_SEMI; s++; }
            else if (s[0] == '<') { type = T_IN; s++; }
            else { type = T_OUT; s++; }
            if (add_token(type, NULL, 0) < 0)
                return -1;
            continue;
        }

        /* 단어: 따옴표 안의 공백/연산자는 단어의 일부 */
        start = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && !is_op_char(*s)) {
            if (*s == '\'') {
                s++;
                while (*s && *s != '\'')
                    s++;
                if (*s != '\'') {
                    shell_error("syntax", "unterminated '");
                    return -1;
                }
                s++;
            } else if (*s == '"') {
                s++;
                while (*s && *s != '"') {
                    if (*s == '\\' && s[1] != '\0')
                        s++;
                    s++;
                }
                if (*s != '"') {
                    shell_error("syntax", "unterminated \"");
                    return -1;
                }
                s++;
            } else if (*s == '\\') {
                s++;
                if (*s)
                    s++;
            } else {
                s++;
            }
        }
        if (add_token(T_WORD, start, (size_t)(s - start)) < 0)
            return -1;
    }
    g_tok[g_ntok].type = T_END;
    return 0;
}

/*
 * 원문 단어를 펼친다: 따옴표를 벗기고, $변수와 맨 앞의 ~ 를 바꾸고, 따옴표 밖의 * ? 를 글롭 문자로 표시한다.
 * 결과는 out->text (와 글롭이 있으면 out->glob) 에. 공간이 모자라면 -1.
 */
static int expand_raw(const char *s, struct token *out)
{
    struct wbuf *w = (struct wbuf *)malloc(sizeof(struct wbuf));
    int rc = 0;

    if (!w)
        return -1;
    memset(w, 0, sizeof(*w));
    if (s[0] == '~' && (s[1] == '\0' || s[1] == '/')) {
        const char *home = getenv("HOME");

        wputs(w, home ? home : "face:/");
        s++;
        if (*s == '/' && w->len > 0 && w->text[w->len - 1] == '/')
            s++;                        /* "face:/" + "/x" 의 '/' 겹침을 피한다 */
    }
    while (*s) {
        if (*s == '\'') {
            s++;
            while (*s && *s != '\'')
                wput(w, *s++, 0);
            if (*s)
                s++;
        } else if (*s == '"') {
            s++;
            while (*s && *s != '"') {
                if (*s == '\\' && (s[1] == '"' || s[1] == '\\' || s[1] == '$')) {
                    wput(w, s[1], 0);
                    s += 2;
                } else if (*s == '$') {
                    s += expand_var(s, w);
                } else {
                    wput(w, *s++, 0);
                }
            }
            if (*s)
                s++;
        } else if (*s == '\\') {
            if (s[1] != '\0') {
                wput(w, s[1], 0);
                s += 2;
            } else {
                s++;
            }
        } else if (*s == '$') {
            s += expand_var(s, w);
        } else {
            wput(w, *s, *s == '*' || *s == '?');
            s++;
        }
    }

    out->type = T_WORD;
    out->text = pool_alloc(w->len + 1);
    out->glob = w->any_glob ? pool_alloc(w->len + 1) : NULL;
    if (!out->text || (w->any_glob && !out->glob) || w->overflow) {
        shell_error("syntax", "line too long");
        rc = -1;
    } else {
        memcpy(out->text, w->text, w->len);
        out->text[w->len] = '\0';
        if (out->glob) {
            memcpy(out->glob, w->glob, w->len);
            out->glob[w->len] = '\0';
        }
    }
    free(w);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 글롭                                                                */
/* ------------------------------------------------------------------ */

/* pat 의 글롭 문자(mask '1')만 특별하다 */
static int glob_match(const char *pat, const char *mask, const char *name)
{
    while (*pat) {
        if (*mask == '1' && *pat == '*') {
            while (*pat == '*' && *mask == '1') {
                pat++;
                mask++;
            }
            if (*pat == '\0')
                return 1;
            for (; *name; name++) {
                if (glob_match(pat, mask, name))
                    return 1;
            }
            return 0;
        }
        if (*name == '\0')
            return 0;
        if (!(*mask == '1' && *pat == '?') && *pat != *name)
            return 0;
        pat++;
        mask++;
        name++;
    }
    return *name == '\0';
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/*
 * word 를 펼쳐 argv[*argc..] 에 더한다. 글롭이 아니거나 맞는 것이 없으면 그대로 하나.
 * 반환: 0, 공간이 모자라면 -1.
 */
static int expand_word(const struct token *t, char **argv, int *argc)
{
    const char *text = t->text;
    const char *slash;
    const char *colon;
    char dir[LINE_MAX_LEN];
    size_t base_off;
    size_t k;
    int first = *argc;
    DIR *d;
    struct dirent *e;

    if (!t->glob) {
        if (*argc >= MAX_ARGS)
            return -1;
        argv[(*argc)++] = t->text;
        return 0;
    }

    /* 마지막 구성요소 앞에 글롭 문자가 있으면 펼치지 않는다 */
    slash = strrchr(text, '/');
    colon = strchr(text, ':');
    if (!slash && colon)
        slash = colon;
    base_off = slash ? (size_t)(slash - text) + 1 : 0;
    for (k = 0; k < base_off; k++) {
        if (t->glob[k] == '1')
            goto literal;
    }
    if (base_off >= sizeof(dir))
        goto literal;
    if (base_off == 0) {
        strcpy(dir, ".");
    } else {
        memcpy(dir, text, base_off);
        dir[base_off] = '\0';
    }

    d = opendir(dir);
    if (!d)
        goto literal;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        char *m;

        if (e->d_name[0] == '.' && text[base_off] != '.')
            continue;
        if (!glob_match(text + base_off, t->glob + base_off, e->d_name))
            continue;
        if (*argc >= MAX_ARGS) {
            closedir(d);
            return -1;
        }
        m = pool_alloc(base_off + nl + 1);
        if (!m) {
            closedir(d);
            return -1;
        }
        memcpy(m, text, base_off);
        memcpy(m + base_off, e->d_name, nl + 1);
        argv[(*argc)++] = m;
    }
    closedir(d);
    if (*argc > first) {
        qsort(argv + first, (size_t)(*argc - first), sizeof(*argv), cmp_str);
        return 0;
    }

literal:
    if (*argc >= MAX_ARGS)
        return -1;
    argv[(*argc)++] = t->text;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 명령 구성                                                            */
/* ------------------------------------------------------------------ */

enum { R_IN, R_OUT, R_APPEND, R_DUP };

struct redir {
    int type;
    int fd;         /* 바꿀 fd */
    int src;        /* R_DUP: 복제할 fd */
    char *path;
};

struct cmd {
    int argc;
    char *argv[MAX_ARGS + 1];
    int nredir;
    struct redir redir[MAX_REDIRS];
    int nassign;
    char *assign[MAX_ASSIGNS];
};

static int is_assignment(const struct token *t)
{
    const char *p = t->text;

    if (t->glob || !is_name_start(*p))
        return 0;
    while (is_name_char(*p))
        p++;
    return *p == '=';
}

/*
 * *pos 부터 파이프라인 하나를 cmds 로 만든다. 끝난 자리(연산자)는 *pos 에.
 * 반환: 명령 수 (빈 줄이면 0), 문법 오류면 -1.
 */
static int parse_pipeline(int *pos, struct cmd *cmds)
{
    int n = 0;
    int i = *pos;
    struct cmd *c;

    memset(&cmds[0], 0, sizeof(cmds[0]));
    c = &cmds[0];
    for (;; i++) {
        struct token *t = &g_tok[i];

        if (t->type == T_WORD) {
            struct token x;
            int assign = c->argc == 0 && is_assignment(t) && c->nassign < MAX_ASSIGNS;

            if (expand_raw(t->text, &x) < 0)
                return -1;
            if (assign) {
                c->assign[c->nassign++] = x.text;
                continue;
            }
            if (expand_word(&x, c->argv, &c->argc) < 0) {
                shell_error("syntax", "too many arguments");
                return -1;
            }
            continue;
        }
        if (t->type == T_IN || t->type == T_OUT || t->type == T_APPEND || t->type == T_ERR || t->type == T_ERRAPP) {
            struct redir *r;

            if (g_tok[i + 1].type != T_WORD) {
                shell_error("syntax", "missing file name after redirection");
                return -1;
            }
            if (c->nredir >= MAX_REDIRS) {
                shell_error("syntax", "too many redirections");
                return -1;
            }
            r = &c->redir[c->nredir++];
            r->type = (t->type == T_IN) ? R_IN : (t->type == T_APPEND || t->type == T_ERRAPP) ? R_APPEND : R_OUT;
            r->fd = (t->type == T_IN) ? 0 : (t->type == T_ERR || t->type == T_ERRAPP) ? 2 : 1;
            r->src = -1;
            {
                struct token x;

                if (expand_raw(g_tok[++i].text, &x) < 0)
                    return -1;
                r->path = x.text;
            }
            continue;
        }
        if (t->type == T_ERR2OUT || t->type == T_OUT2ERR) {
            struct redir *r;

            if (c->nredir >= MAX_REDIRS) {
                shell_error("syntax", "too many redirections");
                return -1;
            }
            r = &c->redir[c->nredir++];
            r->type = R_DUP;
            r->fd = (t->type == T_ERR2OUT) ? 2 : 1;
            r->src = (t->type == T_ERR2OUT) ? 1 : 2;
            r->path = NULL;
            continue;
        }

        /* 명령의 끝 */
        c->argv[c->argc] = NULL;
        if (c->argc == 0 && c->nredir == 0 && c->nassign == 0) {
            if (t->type == T_END || t->type == T_SEMI) {
                if (n == 0) {
                    *pos = i;
                    return 0;
                }
            }
            shell_error("syntax", "unexpected operator");
            return -1;
        }
        n++;
        if (t->type != T_PIPE) {
            *pos = i;
            return n;
        }
        if (n >= MAX_CMDS) {
            shell_error("syntax", "pipeline too long");
            return -1;
        }
        c = &cmds[n];
        memset(c, 0, sizeof(*c));
    }
}

/* ------------------------------------------------------------------ */
/* 내장 명령                                                            */
/* ------------------------------------------------------------------ */

static int run_file(const char *path);

static int b_cd(int argc, char **argv)
{
    char old[LINE_MAX_LEN];
    const char *target;

    if (!getcwd(old, sizeof(old)))
        old[0] = '\0';
    if (argc < 2) {
        target = getenv("HOME");
        if (!target)
            target = "face:/";
    } else if (strcmp(argv[1], "-") == 0) {
        target = getenv("OLDPWD");
        if (!target) {
            shell_error("cd", "OLDPWD not set");
            return 1;
        }
        puts(target);
    } else {
        target = argv[1];
    }
    if (chdir(target) != 0) {
        err_begin();
        fprintf(stderr, "nysh: cd: %s: %s", target, strerror(errno));
        err_end();
        return 1;
    }
    if (old[0])
        setenv("OLDPWD", old, 1);
    if (getcwd(old, sizeof(old)))
        setenv("PWD", old, 1);
    return 0;
}

static int b_pwd(int argc, char **argv)
{
    char buf[LINE_MAX_LEN];

    (void)argc;
    (void)argv;
    if (!getcwd(buf, sizeof(buf))) {
        shell_error("pwd", strerror(errno));
        return 1;
    }
    puts(buf);
    return 0;
}

static int b_exit(int argc, char **argv)
{
    int code = argc > 1 ? atoi(argv[1]) : g_last;

    fflush(stdout);
    exit(code & 0xFF);
    return 0;
}

static int set_assignment(const char *a)
{
    char name[128];
    const char *eq = strchr(a, '=');
    size_t n = (size_t)(eq - a);

    if (n >= sizeof(name))
        return -1;
    memcpy(name, a, n);
    name[n] = '\0';
    return setenv(name, eq + 1, 1);
}

static int b_env(int argc, char **argv)
{
    char **e;

    (void)argc;
    (void)argv;
    for (e = environ; e && *e; e++)
        puts(*e);
    return 0;
}

static int b_export(int argc, char **argv)
{
    int i;
    int rc = 0;

    if (argc < 2)
        return b_env(argc, argv);
    for (i = 1; i < argc; i++) {
        if (strchr(argv[i], '=')) {
            struct token t;

            t.text = argv[i];
            t.glob = NULL;
            if (!is_assignment(&t) || set_assignment(argv[i]) != 0) {
                err_begin();
                fprintf(stderr, "nysh: export: '%s': not a valid name", argv[i]);
                err_end();
                rc = 1;
            }
        }
        /* 값 없는 export NAME: 모든 변수가 이미 환경 변수다 */
    }
    return rc;
}

static int b_unset(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
        (void)unsetenv(argv[i]);
    return 0;
}

static int b_source(int argc, char **argv)
{
    if (argc < 2) {
        shell_error("source", "missing file");
        return 2;
    }
    return run_file(argv[1]);
}

/* 커널이 아는 신원 (nx64_whoami): elev 아래에서는 root. 환경 변수 USER/LOGNAME 은 보지 않는다 */
static int b_whoami(int argc, char **argv)
{
    struct nx64_identity me;

    (void)argc;
    (void)argv;
    puts(nx64_whoami(&me) == 0 && me.name[0] != '\0' ? me.name : "?");
    return 0;
}

static int b_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    puts("built-in: cd [dir|-]  pwd  exit [n]  export [NAME=value]  unset NAME  env  source FILE  whoami  help");
    puts("commands: cat echo ls cp mv rm touch mkdir rmdir chmod head tail wc sleep true false (app:/<name>.run)");
    puts("syntax:   a | b   a ; b   a && b   a || b   a &   < f   > f   >> f   2> f   2>&1   >&2");
    puts("          'text'  \"$VAR\"  \\x  $? $$ ${NAME}  NAME=value  ~  *.txt  # comment");
    return 0;
}

struct builtin {
    const char *name;
    int (*fn)(int, char **);
};

static const struct builtin g_builtins[] = {
    { "cd", b_cd }, { "pwd", b_pwd }, { "exit", b_exit }, { "logout", b_exit },
    { "export", b_export }, { "unset", b_unset }, { "env", b_env },
    { "source", b_source }, { ".", b_source }, { "whoami", b_whoami }, { "help", b_help },
    { NULL, NULL }
};

static const struct builtin *find_builtin(const char *name)
{
    const struct builtin *b;

    for (b = g_builtins; b->name; b++) {
        if (strcmp(b->name, name) == 0)
            return b;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 실행                                                                */
/* ------------------------------------------------------------------ */

/* 리디렉션을 적용한다. 실패하면 메시지를 찍고 -1 */
static int apply_redirs(const struct cmd *c)
{
    int i;

    for (i = 0; i < c->nredir; i++) {
        const struct redir *r = &c->redir[i];
        int fd;

        if (r->type == R_DUP) {
            if (dup2(r->src, r->fd) < 0) {
                shell_error("redirect", strerror(errno));
                return -1;
            }
            continue;
        }
        if (r->type == R_IN)
            fd = open(r->path, O_RDONLY);
        else
            fd = open(r->path, O_WRONLY | O_CREAT | (r->type == R_APPEND ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) {
            shell_error(r->path, strerror(errno));
            return -1;
        }
        if (fd != r->fd) {
            if (dup2(fd, r->fd) < 0) {
                shell_error(r->path, strerror(errno));
                close(fd);
                return -1;
            }
            close(fd);
        }
    }
    return 0;
}

/* 자식 프로세스에서: 실행하고 돌아오지 않는다 */
static void exec_child(struct cmd *c)
{
    const struct builtin *b;
    int i;

    for (i = 0; i < c->nassign; i++)
        (void)set_assignment(c->assign[i]);
    if (apply_redirs(c) < 0)
        _exit(1);
    if (c->argc == 0)
        _exit(0);
    b = find_builtin(c->argv[0]);
    if (b) {
        int rc = b->fn(c->argc, c->argv);

        fflush(stdout);
        _exit(rc);
    }
    execvp(c->argv[0], c->argv);
    if (errno == ENOENT)
        shell_error(c->argv[0], "command not found");
    else
        shell_error(c->argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

/* 셸 프로세스 안에서 내장 명령 하나 (cd/export 가 셸에 남아야 한다). 리디렉션은 끝나면 되돌린다 */
static int run_builtin_here(const struct builtin *b, struct cmd *c)
{
    int saved[3];
    int i;
    int rc;

    for (i = 0; i < 3; i++)
        saved[i] = -1;
    for (i = 0; i < c->nredir; i++) {
        int fd = c->redir[i].fd;

        if (fd >= 0 && fd < 3 && saved[fd] < 0)
            saved[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    }
    fflush(stdout);
    if (apply_redirs(c) < 0)
        rc = 1;
    else
        rc = b->fn(c->argc, c->argv);
    fflush(stdout);
    for (i = 0; i < 3; i++) {
        if (saved[i] >= 0) {
            dup2(saved[i], i);
            close(saved[i]);
        }
    }
    return rc;
}

static int wait_status(pid_t pid)
{
    int st = 0;

    if (waitpid(pid, &st, 0) != pid)
        return 127;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) {
        int sig = WTERMSIG(st);

        if (sig == SIGINT) {
            /* Ctrl-C: 콘솔이 이미 "^C" 와 줄바꿈을 보였다 */
        } else if (sig == SIGSEGV)
            fputs("Segmentation fault\n", stderr);
        else if (sig == SIGKILL)
            fputs("Killed\n", stderr);
        else if (sig == SIGTERM)
            fputs("Terminated\n", stderr);
        else if (sig == SIGQUIT)
            fputs("Quit\n", stderr);
        else if (sig == SIGFPE)
            fputs("Floating point exception\n", stderr);
        else
            fprintf(stderr, "Terminated by signal %d\n", sig);
        return 128 + sig;
    }
    return 1;
}

static void remember_bg(pid_t pid)
{
    int i;

    for (i = 0; i < MAX_BG; i++) {
        if (g_bg[i] == 0) {
            g_bg[i] = pid;
            return;
        }
    }
}

/* 끝난 백그라운드 작업을 거둔다 */
static void reap_bg(void)
{
    int i;

    for (i = 0; i < MAX_BG; i++) {
        int st = 0;

        if (g_bg[i] != 0 && waitpid(g_bg[i], &st, WNOHANG) == g_bg[i]) {
            if (g_interactive)
                fprintf(stderr, "[done] %ld (exit %d)\n", (long)g_bg[i],
                        WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
            g_bg[i] = 0;
        }
    }
}

static int run_pipeline(struct cmd *cmds, int n, int background)
{
    pid_t pids[MAX_CMDS];
    pid_t pgid = 0;                         /* 이 작업의 프로세스 그룹 (첫 자식의 pid) */
    int jobctl = g_interactive && !background;
    int in_fd = -1;
    int started = 0;
    int rc = 0;
    int i;

    /* 셸에 남아야 하는 것: 변수 대입만 있는 명령, 파이프/백그라운드가 아닌 내장 명령 */
    if (n == 1 && !background) {
        const struct builtin *b = cmds[0].argc > 0 ? find_builtin(cmds[0].argv[0]) : NULL;

        if (cmds[0].argc == 0 && cmds[0].nredir == 0) {
            for (i = 0; i < cmds[0].nassign; i++)
                (void)set_assignment(cmds[0].assign[i]);
            return 0;
        }
        if (b && cmds[0].nassign == 0)
            return run_builtin_here(b, &cmds[0]);
    }

    fflush(stdout);
    for (i = 0; i < n; i++) {
        int p[2] = { -1, -1 };
        pid_t pid;

        if (i < n - 1 && pipe(p) != 0) {
            shell_error("pipe", strerror(errno));
            rc = 1;
            break;
        }
        pid = fork();
        if (pid < 0) {
            shell_error("fork", strerror(errno));
            if (p[0] >= 0) {
                close(p[0]);
                close(p[1]);
            }
            rc = 1;
            break;
        }
        if (pid == 0) {
            /* 부모도 아래에서 같은 일을 한다: 누가 먼저 실행되든 exec 전에 그룹이 정해지도록 */
            if (jobctl)
                (void)setpgid(0, pgid);     /* 첫 자식은 0 = 자기 pid 로 새 그룹 */
            if (in_fd >= 0) {
                dup2(in_fd, 0);
                close(in_fd);
            }
            if (p[1] >= 0) {
                dup2(p[1], 1);
                close(p[1]);
                close(p[0]);
            }
            exec_child(&cmds[i]);
        }
        pids[started++] = pid;
        if (jobctl) {
            if (pgid == 0) {
                pgid = pid;
                (void)setpgid(pid, pgid);
                (void)tcsetpgrp(0, pgid);   /* 이제 Ctrl-C 는 이 작업을 끝낸다 */
            } else {
                (void)setpgid(pid, pgid);
            }
        }
        if (in_fd >= 0)
            close(in_fd);
        if (p[1] >= 0)
            close(p[1]);
        in_fd = p[0];
    }
    if (in_fd >= 0)
        close(in_fd);

    if (background) {
        for (i = 0; i < started; i++)
            remember_bg(pids[i]);
        if (started > 0 && g_interactive)
            fprintf(stderr, "[%ld]\n", (long)pids[started - 1]);
        return rc;
    }
    for (i = 0; i < started; i++) {
        int st = wait_status(pids[i]);

        if (i == started - 1 && rc == 0)
            rc = st;
    }
    if (jobctl && pgid != 0)
        (void)tcsetpgrp(0, 0);              /* 프롬프트로: Ctrl-C 는 쓰던 줄만 버린다 */
    return rc;
}

/* 한 줄 실행 */
static void run_line(const char *line)
{
    struct lexer *outer = g_lex;
    struct cmd *cmds;
    int pos = 0;
    int skip = 0;

    g_lex = (struct lexer *)malloc(sizeof(struct lexer));
    cmds = (struct cmd *)malloc(sizeof(struct cmd) * MAX_CMDS);
    if (!g_lex || !cmds) {
        shell_error("nysh", "out of memory");
        free(g_lex);
        free(cmds);
        g_lex = outer;
        return;
    }
    if (tokenize(line) < 0) {
        g_last = 2;
        free(g_lex);
        free(cmds);
        g_lex = outer;
        return;
    }
    while (g_tok[pos].type != T_END) {
        int n = parse_pipeline(&pos, cmds);
        int op = g_tok[pos].type;

        if (n < 0) {
            g_last = 2;
            break;
        }
        if (n > 0 && !skip)
            g_last = run_pipeline(cmds, n, op == T_BG);
        if (op == T_END)
            break;
        /* 다음 것을 실행할지: && 는 성공했을 때만, || 는 실패했을 때만 */
        skip = (op == T_AND && g_last != 0) || (op == T_OR && g_last == 0);
        pos++;
    }
    free(cmds);
    free(g_lex);
    g_lex = outer;
}

static int run_stream(FILE *in)
{
    char line[LINE_MAX_LEN];

    while (fgets(line, sizeof(line), in))
        run_line(line);
    return g_last;
}

static int run_file(const char *path)
{
    FILE *f = fopen(path, "r");
    int rc;

    if (!f) {
        shell_error(path, strerror(errno));
        return 1;
    }
    rc = run_stream(f);
    fclose(f);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 프롬프트                                                             */
/* ------------------------------------------------------------------ */

/* 현재 디렉터리의 마지막 이름. 홈은 "~", 이름공간의 뿌리("tmp:/")는 전체 */
static void prompt_name(char *out, size_t cap)
{
    char cwd[LINE_MAX_LEN];
    const char *home = getenv("HOME");
    const char *colon;
    const char *base;
    size_t len;

    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(out, "?");
        return;
    }
    len = strlen(cwd);
    while (len > 1 && cwd[len - 1] == '/' && cwd[len - 2] != ':')
        cwd[--len] = '\0';
    if (home) {
        size_t hl = strlen(home);

        while (hl > 1 && home[hl - 1] == '/' && home[hl - 2] != ':')
            hl--;
        if (strncmp(cwd, home, hl) == 0 && cwd[hl] == '\0') {
            strcpy(out, "~");
            return;
        }
    }
    colon = strchr(cwd, ':');
    if (colon && colon[1] == '/' && colon[2] == '\0') {
        base = cwd;                                 /* 이름공간의 뿌리 */
    } else {
        base = strrchr(cwd, '/');
        base = base ? base + 1 : cwd;
    }
    strncpy(out, base, cap - 1);
    out[cap - 1] = '\0';
}

/* 프롬프트 기호 (설정 mark). 출력 가능한 ASCII 1~8 자가 아니면 "*" */
static char g_mark[9] = "*";

static void load_mark(void)
{
    char v[16];
    long n = nx64_setting_get("nysh", "mark", v, sizeof(v));
    long i;

    if (n < 1 || n > 8)
        return;
    for (i = 0; i < n; i++) {
        if ((unsigned char)v[i] < 0x21 || (unsigned char)v[i] > 0x7E)
            return;
    }
    memcpy(g_mark, v, (size_t)n + 1U);
}

static void show_prompt(void)
{
    char name[256];

    prompt_name(name, sizeof(name));
    if (g_color)        /* 디렉터리: 굵은 파랑, 기호: 직전 명령이 성공하면 초록, 실패하면 빨강 */
        fprintf(stderr, "\033[1;34m%s\033[0m\033[1;%dm%s\033[0m ", name, g_last == 0 ? 32 : 31, g_mark);
    else
        fprintf(stderr, "%s%s ", name, g_mark);
}

/* 시작 스크립트 (설정 rc): 기본, 그다음 사용자 것. 없는 것은 조용히 건너뛴다 */
static void run_rc(void)
{
    static const int where[2] = { NX64_SETTING_DEFAULT, NX64_SETTING_USER };
    char path[256];
    int i;

    for (i = 0; i < 2; i++) {
        FILE *f;

        if (nx64_setting_path(where[i], "nysh", "rc", path, sizeof(path)) != 0)
            continue;
        f = fopen(path, "r");
        if (!f)
            continue;
        (void)run_stream(f);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    char line[LINE_MAX_LEN];
    const char *me;

    /* 로그인 셸은 빈 환경으로 시작한다: 기본 변수를 채운다 */
    if (!getenv("HOME"))
        setenv("HOME", "face:/", 1);
    me = getlogin();
    if (me) {
        if (!getenv("USER"))
            setenv("USER", me, 1);
        if (!getenv("LOGNAME"))
            setenv("LOGNAME", me, 1);
    }
    if (!getenv("SHELL"))
        setenv("SHELL", "app:/nysh.run", 1);
    if (getcwd(line, sizeof(line)))
        setenv("PWD", line, 1);
    g_color = want_color();

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        run_line(argv[2]);
        return g_last;
    }
    if (argc >= 2)
        return run_file(argv[1]);

    g_interactive = isatty(0);
    if (g_interactive) {
        (void)tcsetpgrp(0, 0);              /* 이 셸을 띄운 셸의 작업 그룹에서 벗어난다: 프롬프트의 Ctrl-C 로 죽지 않게 */
        load_mark();
        run_rc();
        g_last = 0;                         /* rc 의 마지막 결과가 첫 프롬프트 색을 정하지 않게 */
    }
    for (;;) {
        reap_bg();
        if (g_interactive)
            show_prompt();
        if (!fgets(line, sizeof(line), stdin)) {
            if (ferror(stdin) && errno == EINTR) {
                clearerr(stdin);                    /* 신호가 읽기를 깨웠다: 로그아웃이 아니다 */
                continue;
            }
            if (g_interactive)
                fputc('\n', stderr);
            return g_last;                          /* EOF (Ctrl-D): 로그아웃 */
        }
        run_line(line);
    }
}
