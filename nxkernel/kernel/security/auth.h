/*
 * auth.h - 사용자 인증 (roi:/users) 과 신원 토큰
 *
 * [사용자 DB] fs:/roi/users (= roi:/users). 빌드 때 호스트 도구(tools/mkusers.py, `make users`)가 만든다.
 *   한 줄에 한 사용자:  이름:uid:gid:반복횟수:솔트(hex):해시(hex)
 *     해시 = PBKDF2-HMAC-SHA256(비밀번호, 솔트, 반복횟수, 32바이트)
 *   '#' 로 시작하는 줄과 빈 줄은 무시한다. 형식이 틀린 줄은 경고를 찍고 건너뛴다.
 *   커널은 부팅 때 한 번만 읽어 메모리에 둔다. 이 파일은 커널 전용 경로라 실행 중에는 아무도
 *   (uid 0 포함) 읽거나 바꿀 수 없다 (kernel/syscall/nspath.h). 바꾸려면 이미지를 다시 만든다.
 *
 * [토큰] 인증에 성공하면 그 사용자의 신원(uid/gid/이름)을 담은 토큰이 핸들로 주어진다.
 *   토큰은 일회용이다: NxProcessCreate 에 쓰면 새 프로세스가 그 신원으로 시작하고 토큰은 사라진다.
 *   토큰을 얻는 길은 인증뿐이다 — uid 0 이라도 비밀번호 없이 다른 사용자의 토큰을 만들 수 없다.
 */
#ifndef KERNEL_SECURITY_AUTH_H
#define KERNEL_SECURITY_AUTH_H

#include "nyxis.h"
#include "kernel/process/process.h"

#define AUTH_MAX_USERS      32U
#define AUTH_DB_MAX         4095U          /* roi:/users 최대 크기 (바이트) */
#define AUTH_MIN_ITERATIONS 1000U
#define AUTH_MAX_ITERATIONS 1000000U
#define AUTH_PASSWORD_MAX   128U

typedef struct auth_identity {
    u32  uid;
    u32  gid;
    char name[NX_USER_NAME_MAX];
} auth_identity_t;

/* path(커널 VFS 경로)의 사용자 DB 를 읽는다. 부팅 때 한 번. 성공하면 읽은 사용자 수를 *out_count 에 */
Nstatus auth_load_users(const char *path, u32 *out_count);

/*
 * name/password 를 검증한다. 맞으면 *out 에 신원을 채우고 NSTATUS_OK, 아니면 Nauthentication.
 * 사용자가 없을 때도 같은 양의 계산을 해서 "그런 사용자가 있는지" 가 시간으로 드러나지 않게 한다.
 * (실패 시의 지연은 시스템 콜 계층이 넣는다 — 이 함수는 잠들지 않는다)
 */
Nstatus auth_verify(const char *name, const char *password, auth_identity_t *out);

/* 이름으로 사용자를 찾는다 (자체 점검/정보용). 없으면 NnotFound */
Nstatus auth_lookup(const char *name, auth_identity_t *out);

#endif /* KERNEL_SECURITY_AUTH_H */
