/*
 * nyxapp.h - 앱 설정 (Nyxis 전용)
 *
 * nyfs 의 노드는 파일이든 디렉터리든 하나의 오브젝트다 (내용 = 데이터 스트림, 자식 = 목록, 둘은 독립).
 * 그래서 앱은 "ELF 를 내용으로, 기본 설정을 자식으로" 가진 오브젝트다:
 *
 *   app:/<앱>.run              실행 파일 (오브젝트의 내용)
 *   app:/<앱>.run/<키>         기본값 (앱과 함께 배포, root 소유: 누구나 읽기만. 소스는 rootfs/app/<앱>/<키>)
 *   roi:/<uid>/app/<앱>/<키>   사용자 값 (그 사용자만 읽고 씀). 있으면 기본값보다 먼저
 *
 * 값 하나 = 파일 하나. 한 줄짜리 값은 끝의 개행 하나를 뗀다 (roi:/hostname 과 같은 규칙).
 * 앱/키 이름: 구성요소마다 [A-Za-z0-9._-]+ ("." 과 ".." 은 안 됨). 키는 '/' 로 하위 경로를 쓸 수 있다
 * (예: "keys/quit"). 이름이 틀리면 EINVAL.
 */
#ifndef NYXAPP_H
#define NYXAPP_H

#include <stddef.h>

#define NYX_SETTING_DEFAULT 0   /* app:/<앱>.run/<키> */
#define NYX_SETTING_USER    1   /* roi:/<uid>/app/<앱>/<키> */

/*
 * 설정 값을 buf 에 읽는다 (NUL 종결, 끝의 개행 하나는 뗀다). 사용자 값이 있으면 그것, 없으면 기본값.
 * 반환: 값의 길이. 둘 다 없으면 -1 (errno = ENOENT), buf 가 모자라면 -1 (ERANGE), 이름이 틀리면 -1 (EINVAL).
 * where 를 받는 _from 판은 한쪽만 본다.
 */
long nyx_setting_get(const char *app, const char *key, char *buf, size_t cap);
long nyx_setting_get_from(int where, const char *app, const char *key, char *buf, size_t cap);

/* 사용자 값으로 저장한다 (value + '\n'). roi:/<uid>/app/<앱>/ 과 키의 상위 디렉터리가 없으면 0700 으로 만든다.
 * 성공 0, 실패 -1 (errno) */
int nyx_setting_set(const char *app, const char *key, const char *value);

/* 사용자 값을 지운다 (이후 기본값이 보인다). 없던 값이면 0 */
int nyx_setting_reset(const char *app, const char *key);

/* 설정 파일의 경로 (여러 줄짜리 설정을 직접 읽을 때. 예: nysh 의 rc). 성공 0, 실패 -1 (EINVAL/ERANGE) */
int nyx_setting_path(int where, const char *app, const char *key, char *buf, size_t cap);

#endif /* NYXAPP_H */
