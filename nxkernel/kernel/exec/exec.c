/*
 * exec.c - 실행 계층: fork 와 exec (설계는 exec.h)
 */
#include "kernel/exec/exec.h"
#include "kernel/process/signal.h"
#include "kernel/paging/paging.h"
#include "kernel/syscall/handles.h"
#include "kernel/dev/device.h"
#include "phys.h"
#include "lowlevel.h"
#include "memory.h"

#define USER_RFLAGS_INIT 0x202UL    /* IF + 예약 비트 1 */

Nstatus exec_fork(struct trap_frame *frame, process_t **out_child)
{
    process_t *parent = current_process;
    addr_space_t as;
    process_t *child = nNULL;
    Nstatus status;

    if (!parent || !parent->addrspace.pml4 || !frame || !out_child)
        return NinvalidArg;

    status = paging_addrspace_create(&as);
    if (NSTATUS_IS_ERR(status))
        return status;
    status = paging_addrspace_clone_user(&parent->addrspace, &as);
    if (NSTATUS_IS_ERR(status)) {
        paging_addrspace_destroy(&as);
        return status;
    }
    status = process_create_forked(as, frame, &child);
    if (NSTATUS_IS_ERR(status)) {
        paging_addrspace_destroy(&as);
        return status;
    }
    /* 자식은 아직 실행되지 않았다 (시스템 콜 중에는 선점되지 않는다): 지금 핸들을 채운다 */
    status = nx_handles_inherit(&child->handles, &parent->handles, true, false);
    if (NSTATUS_IS_ERR(status)) {
        /* 열린 파일 사본을 만들 메모리가 없었다: 자식을 실행하지 않고 버린다 */
        (void)process_kill(child, NX_EXIT_KILLED);
        process_release(child);
        return status;
    }
    *out_child = child;
    return NSTATUS_OK;
}

Nstatus exec_replace(struct trap_frame *frame, const char *path, const spawn_args_t *args)
{
    process_t *self = current_process;
    spawn_image_t img;
    addr_space_t old;
    Nstatus status;
    u32 mxcsr = 0x1F80U;

    if (!self || !self->addrspace.pml4 || !frame || !path)
        return NinvalidArg;

    /* 1. 새 이미지를 끝까지 만든다 (실패하면 옛 프로그램은 그대로) */
    status = spawn_build_image(path, args, &img);
    if (NSTATUS_IS_ERR(status))
        return status;

    /* 2. 여기부터는 되돌릴 수 없다: 주소 공간을 바꾸고 옛 것을 버린다.
     *    장치 MMIO 매핑(PAGE_NOFREE)은 옛 주소 공간과 함께 사라지므로, 장치가 "이미 매핑됨" 으로 기억하지
     *    않게 지운다 (장치 소유권 자체는 핸들과 함께 남는다: 새 프로그램이 다시 매핑할 수 있다) */
    {
        u32 i;

        for (i = 0; i < NX_MAX_HANDLES; i++) {
            nx_handle_t *slot = &self->handles.slots[i];

            if (slot->type == NX_HANDLE_DEVICE && slot->dev)
                slot->dev->mapped_va = 0;
        }
    }
    old = self->addrspace;
    self->addrspace = img.as;
    write_cr3(virt_to_phys(self->addrspace.pml4));
    paging_addrspace_destroy(&old);

    self->user_entry = img.entry;
    self->user_stack_top = img.stack;
    self->heap_start = img.image_end;
    self->heap_brk = img.image_end;

    /* 3. 핸들: CLOEXEC 를 닫고 세대를 0 으로 (새 프로그램에서 핸들 값 = 슬롯 번호) */
    nx_handles_exec(&self->handles);
    signal_exec_reset(self);                /* 옛 이미지의 처리기 주소는 새 이미지에서 뜻이 없다 */

    /* 4. FPU/SSE 를 새 프로그램의 시작 상태로 */
    __asm__ volatile ("fninit\n\tldmxcsr %0" : : "m"(mxcsr));

    /* 5. 시스템 콜 복귀가 새 프로그램의 시작이 되도록 프레임을 바꾼다 (cs/ss 는 이미 사용자 셀렉터) */
    frame->r15 = frame->r14 = frame->r13 = frame->r12 = 0;
    frame->r11 = frame->r10 = frame->r9 = frame->r8 = 0;
    frame->rbp = frame->rdi = frame->rsi = frame->rdx = 0;
    frame->rcx = frame->rbx = frame->rax = 0;
    frame->rip = img.entry;
    frame->rsp = img.stack;
    frame->rflags = USER_RFLAGS_INIT;
    return NSTATUS_OK;
}
