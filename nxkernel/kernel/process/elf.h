/*
 * elf.h - ELF64 사용자 프로그램 로더
 *
 * 정적(static, no-PIE) ELF64 실행 파일(ET_EXEC)의 PT_LOAD 세그먼트를 새 주소 공간에
 * 매핑한다. 커널은 이 파일을 통째로 메모리에 올리지 않고, VFS 핸들에서 필요한 바이트만
 * 그때그때 읽어 목적지 페이지에 직접 써 넣는다 (커널에 아직 범용 힙 할당자가 없으므로
 * 파일 크기만큼의 임시 버퍼를 만들 수 없다 — 그리고 설령 있어도 불필요한 복사다).
 *
 * 제약 (지금 버전, 문서화된 한계):
 *  - ET_EXEC, x86_64, 리틀 엔디언만 지원한다 (동적 링크/PIE 는 지원하지 않는다).
 *  - 모든 PT_LOAD 세그먼트의 p_vaddr 는 4KiB 정렬이어야 한다 (표준 링커가 만드는
 *    파일은 대부분 이 조건을 만족한다). 아니면 NinvalidFormat.
 *  - 세그먼트가 쓰기 가능하면서 실행 가능하면 거부한다 (W^X, paging_map_page 가 강제).
 */
#ifndef KERNEL_PROCESS_ELF_H
#define KERNEL_PROCESS_ELF_H

#include "nyxis.h"
#include "kernel/paging/paging.h"
#include "drivers/filesystem/vfs.h"

/*
 * file 이 가리키는 ELF64 실행 파일을 검증하며 as 의 사용자 영역에 매핑한다.
 * file->offset 은 이 함수가 자유롭게 사용한다 (돌려줄 때 값을 보장하지 않는다).
 *
 *   out_entry      : 성공하면 ELF 진입점의 가상 주소 (e_entry)
 *   out_image_end  : 성공하면 로드된 이미지의 끝(가장 높은 가상 주소, 4KiB 정렬) —
 *                     이 값을 힙(brk)의 시작 주소로 쓰면 된다.
 *
 * 실패하면 as 안에 부분적으로 매핑이 남아 있을 수 있다 — 이 함수는 스스로 되돌리지 않는다.
 * 호출자는 실패 시 반드시 as 전체를 paging_addrspace_destroy() 로 파기해야 한다 (부분적으로만
 * 되돌리면 "매핑은 없는데 pfa_free 는 안 된" 프레임이 생겨 물리 메모리가 샐 수 있으므로,
 * 정리는 항상 주소 공간을 통째로 파기하는 한 곳에서만 일어나도록 설계했다).
 */
Nstatus elf_load(handle_t *file, addr_space_t *as, u64 *out_entry, u64 *out_image_end);

#endif /* KERNEL_PROCESS_ELF_H */
