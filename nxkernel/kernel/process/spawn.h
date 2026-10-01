/*
 * spawn.h - 실행 파일 경로로부터 새 사용자 프로세스를 만든다 (NxProcessCreate 의 실제 구현)
 */
#ifndef KERNEL_PROCESS_SPAWN_H
#define KERNEL_PROCESS_SPAWN_H

#include "nyxis.h"

/*
 * path 의 ELF64 실행 파일을 열어 새 주소 공간에 적재하고, 64KiB 스택(+가드 페이지)을 만들고,
 * READY 상태의 새 프로세스를 등록한다. 성공하면 *out_pid 에 pid.
 *
 * 어느 단계에서 실패하든 이 함수가 만든 모든 것(열린 파일, 주소 공간 전체 = 그 안의 모든
 * 프레임과 페이지 테이블)을 되돌리고 오류를 반환한다. 성공하면 자원의 소유권은 새 프로세스로
 * 넘어가고, 프로세스가 끝날 때 process_reap() 이 회수한다.
 */
Nstatus process_spawn(const char *path, u32 *out_pid);

#endif /* KERNEL_PROCESS_SPAWN_H */
