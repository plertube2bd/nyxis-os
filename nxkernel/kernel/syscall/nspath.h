/*
 * nspath.h - 사용자 경로 -> 커널 경로 해석 (이름공간 규칙은 include/nyx_abi.h 의 "이름공간" 참고)
 *
 * 사용자 프로세스가 넘긴 경로는 시스템 콜 계층에서 항상 이 모듈을 거친다:
 *   app:/x   -> fs:/usr/x
 *   face:/x  -> fs:/home/<호출자의 user_name>/x      (프로세스마다 다르다)
 *   roi:/x   -> fs:/roi/x      (uid 0 만)
 *   fs:/x    -> fs:/x          (uid 0 만)
 *   tmp:/x   -> tmp:/x
 *   dev:/x   -> 장치 이름 "x"  (uid 0 만)
 *   svc:/x   -> 서비스 이름 "x"
 * 경로는 정규화된다: 빈 구성요소와 "." 는 버리고, ".." 는 한 단계 올라가되 별칭의 뿌리 위로는
 * 못 올라간다 (Npermission). 그래서 face:/../other 같은 경로로 남의 홈에 닿을 수 없다.
 * VFS 는 이 결과(fs:/..., tmp:/...)만 본다 — 별칭은 VFS 의 전역 네임스페이스가 아니다.
 *
 * 커널 전용 경로: fs:/roi/users (사용자 비밀번호 해시). uid 0 을 포함해 어떤 프로세스도 열거나
 * 지우거나 옮길 수 없고, 그 조상 디렉터리(fs:/roi)를 지우거나 옮기는 것도 막는다 (옮겨서 우회 방지).
 * 커널은 부팅 때 VFS 를 직접 불러 한 번만 읽는다 (kernel/security/auth.c).
 */
#ifndef KERNEL_SYSCALL_NSPATH_H
#define KERNEL_SYSCALL_NSPATH_H

#include "nyxis.h"

#define NSP_PATH_MAX 320U

#define NSP_KIND_VFS 1U
#define NSP_KIND_DEV 2U
#define NSP_KIND_SVC 3U

#define NSP_OP_OPEN   1U    /* 열기, 목록 */
#define NSP_OP_MODIFY 2U    /* 만들기, 지우기, 이름 바꾸기 */

/* 현재 프로세스의 신원으로 user_path 를 해석한다. *out_kind 에 NSP_KIND_*. */
Nstatus nsp_translate(const char *user_path, char *out, usize out_max, u32 *out_kind);

/* 해석된 VFS 경로가 커널 전용 경로 규칙에 걸리면 Npermission */
Nstatus nsp_check_protected(const char *vfs_path, u32 op);

#endif /* KERNEL_SYSCALL_NSPATH_H */
