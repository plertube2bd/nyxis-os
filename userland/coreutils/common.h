/*
 * common.h - coreutils 공용 도우미 (각 명령에 함께 링크된다)
 */
#ifndef CU_COMMON_H
#define CU_COMMON_H

#include <stddef.h>

#define CU_PATH_MAX 512

extern const char *cu_prog;     /* 오류 메시지 앞에 붙는 명령 이름 (main 에서 정한다) */
extern int cu_optind;           /* 다음에 볼 argv 번호 */
extern char *cu_optarg;         /* 인자를 받는 옵션의 값 */

/*
 * 짧은 옵션 해석 (POSIX getopt 와 같은 규칙): spec 의 글자 뒤 ':' 는 값을 받는다 ("-n 5" 또는 "-n5").
 * 묶을 수 있다 ("-rf"). 옵션이 아닌 첫 인자 또는 "--" 에서 멈춘다 ("-" 하나는 피연산자).
 * 반환: 옵션 글자, 끝이면 -1, 모르는 옵션/값 없음이면 메시지를 찍고 '?'.
 */
int cu_getopt(int argc, char **argv, const char *spec);

/* "<prog>: <what>: <strerror(errno)>" 를 stderr 로 */
void cu_warn(const char *what);

/* "<prog>: <서식>" + 줄바꿈을 stderr 로 */
void cu_msg(const char *fmt, ...);

/* 마지막 경로 구성요소 (끝의 '/' 는 무시). "tmp:/a/b/" -> "b", 이름공간 뿌리("tmp:/")는 "" */
void cu_basename(const char *path, char *out, size_t cap);

/* dir + '/' + name. 길이가 넘치면 -1 (errno = ENAMETOOLONG) */
int cu_join(char *out, size_t cap, const char *dir, const char *name);

/* fd 가 터미널이고 NO_COLOR(비어 있지 않음)/TERM=dumb 가 아니면 1: ANSI 색을 써도 된다 */
int cu_color_ok(int fd);

/* 디렉터리면 1, 아니면(없음 포함) 0 */
/* 디렉터리로 다룰지: 자식이 있으면(S_ISDIR), 또는 경로가 '/' 로 끝나고 자식을 가질 수 있는 노드면.
 * Nyxis 의 노드는 파일/디렉터리 구분 없이 내용과 자식을 함께 가지고, 자식이 있으면 디렉터리로 보인다 */
int cu_is_dir(const char *path);
/* path 가 자식을 담을 수 있는 노드인지 (opendir 이 된다: Nyxis 는 모든 노드, 다른 OS 는 디렉터리) */
int cu_can_hold(const char *path);

/* n 바이트를 모두 쓴다. 실패하면 -1 */
int cu_write_all(int fd, const void *buf, size_t n);

/* in 을 EOF 까지 out 으로. 실패하면 -1 (errno 유지) */
int cu_copy_fd(int in, int out);

/*
 * 일반 파일 src 를 dst 로 복사한다 (dst 는 만들거나 덮어쓴다, 모드는 src 의 권한 비트).
 * 실패하면 문제가 된 경로로 메시지를 찍고 -1.
 */
int cu_copy_file(const char *src, const char *dst);

#endif /* CU_COMMON_H */
