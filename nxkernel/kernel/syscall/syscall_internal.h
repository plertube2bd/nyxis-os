/*
 * syscall_internal.h - 시스템 콜 구현 파일들(syscall.c, syscall_ext.c) 사이에서만 쓰는 도우미
 */
#ifndef KERNEL_SYSCALL_INTERNAL_H
#define KERNEL_SYSCALL_INTERNAL_H

#include "nyxis.h"
#include "kernel/syscall/handles.h"

/* 현재 프로세스의 핸들 테이블 (없으면 NULL) */
nx_handle_table_t *cur_handles(void);

/* 프로세스 핸들 handle (권한 rights 필요) 의 프로세스 */
struct process;
Nstatus lookup_process(u64 handle, u32 rights, struct process **out);

/* 사용자 경로 -> VFS 경로 (장치/서비스/info 경로는 거부) + 커널 전용 경로 검사 (op = NSP_OP_*) */
Nstatus user_vfs_path(u64 uptr, char *kpath, usize kmax, u32 op);

/* 사용자의 NULL 로 끝나는 문자열 포인터 배열을 blob 뒤에 이어 붙인다 (spawn_args_t 형식) */
Nstatus collect_user_strings(u64 user_vec, char *blob, usize *used, u32 *count);

/* syscall_ext.c: 실행/보안/IPC 계층과 핸들 확장의 시스템 콜 */
i64 sys_duplicate_handle_ex(const u64 *a);
i64 sys_handle_flags(const u64 *a);
i64 sys_process_fork(const u64 *a);
i64 sys_process_exec(const u64 *a);
i64 sys_pipe_create(const u64 *a);
i64 sys_pty_create(const u64 *a);
i64 sys_stat_path(const u64 *a);
i64 sys_set_mode(const u64 *a);
i64 sys_set_umask(const u64 *a);

#endif /* KERNEL_SYSCALL_INTERNAL_H */
