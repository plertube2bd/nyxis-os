/*
 * perm.h - 보안 계층: 파일 권한 비트와 프로세스의 umask
 *
 * 권한 "판정" 자체는 파일시스템 드라이버의 한 곳(nyfs_check_access)에서만 한다 (nyfs.c 머리말).
 * 이 계층은 시스템 콜과 그 판정 사이의 정책을 맡는다:
 *   - 새 노드의 mode 에 호출자의 umask 를 적용한다 (NxCreateNode)
 *   - chmod 의 입력 검사 (0777 이하만: setuid/setgid/sticky 없음) 와 이름공간 보호 (커널 전용 경로 금지)
 *   - stat 결과에 권한/소유자를 채운다
 *   - 실행 권한: NxProcessCreate/NxProcessExec 는 VFS_O_EXEC 로 열어 x 비트를 요구한다 (spawn_build_image)
 * chmod 를 누가 할 수 있는지(소유자 또는 root)는 드라이버가 판정한다 (nyfs_chmod).
 */
#ifndef KERNEL_SECURITY_PERM_H
#define KERNEL_SECURITY_PERM_H

#include "nyxis.h"
#include "nyx_abi.h"
#include "drivers/filesystem/vfs.h"

/* 현재 프로세스의 umask 를 적용한 mode */
u32 perm_apply_umask(u32 mode);

/* 현재 프로세스의 umask 를 바꾸고 이전 값을 돌려준다 (0777 밖의 비트는 버린다) */
u32 perm_set_umask(u32 mask);

/* chmod. kpath 는 이미 이름공간 해석과 보호 검사를 거친 VFS 경로 */
Nstatus perm_chmod(const char *kpath, u32 mode);

/* VFS 정보를 nx_stat 의 type/size/mode/uid/gid 로 옮긴다 (rights 는 건드리지 않는다) */
void perm_fill_stat(const vfs_stat_t *vs, struct nx_stat *st);

#endif /* KERNEL_SECURITY_PERM_H */
