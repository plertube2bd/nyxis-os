/*
 * exec.h - 실행 계층: fork 와 exec
 *
 * [fork] 부모의 사용자 주소 공간을 새 주소 공간에 통째로 복사하고(즉시 복사, COW 아님), 부모가 시스템 콜에
 *   들어온 순간의 사용자 레지스터를 자식의 첫 복귀 프레임으로 쓴다 (rax 만 0). 핸들 테이블은 슬롯 번호와
 *   세대까지 같게 물려준다 (handles.h 의 nx_handles_inherit) — 그래서 자식 메모리에 남은 핸들 값이 그대로 유효하다.
 *   자식은 READY 상태로 만들어지고, 부모는 시스템 콜에서 돌아온 뒤 스케줄러가 자식을 실행한다.
 *
 * [exec] 새 이미지(주소 공간 + 스택)를 spawn_build_image 로 끝까지 만든 다음에야 옛 주소 공간을 버린다.
 *   그래서 실패(파일 없음, 실행 권한 없음, ELF 아님, 메모리 부족)는 옛 프로그램에 오류로 돌아간다.
 *   성공하면 시스템 콜 프레임을 새 프로그램의 시작 상태(rip = 진입점, rsp = 새 스택, 나머지 0)로 바꿔서
 *   시스템 콜 복귀가 곧 새 프로그램의 시작이 된다. pid/신원/umask/부모 관계/핸들은 그대로이고,
 *   NX_HFLAG_CLOEXEC 핸들은 닫히며 남은 핸들의 세대는 0 이 된다 (nx_handles_exec).
 */
#ifndef KERNEL_EXEC_EXEC_H
#define KERNEL_EXEC_EXEC_H

#include "nyxis.h"
#include "interrupt.h"
#include "kernel/process/process.h"
#include "kernel/process/spawn.h"

/* NxProcessFork 의 본체. frame = 부모의 시스템 콜 프레임. 성공하면 *out_child 에 참조 1 개 */
Nstatus exec_fork(struct trap_frame *frame, process_t **out_child);

/* NxProcessExec 의 본체. 성공하면 frame 이 새 프로그램의 시작 상태로 바뀌어 있다 */
Nstatus exec_replace(struct trap_frame *frame, const char *path, const spawn_args_t *args);

#endif /* KERNEL_EXEC_EXEC_H */
