# Nyxis Kernel Components

## English

This directory contains the core kernel components for the Nyxis operating system.

### Boot (`boot/`)

`boot.s` has two entry points: `_start` (64-bit, UEFI/NYTB, `rdi = NTBLI`) and `_start_mb` (32-bit, Multiboot2/GRUB,
BIOS or UEFI). Both copy the boot info, build temporary page tables (low 4 GiB identity + kernel at the higher half),
and jump to `kernel_main(boot_kind, info_phys)`. `multiboot2.c` converts a Multiboot2 info structure into `NTBLI`,
so the kernel never depends on the firmware/boot method. `make iso` builds a GRUB ISO (BIOS + UEFI).

### Interrupts (`interrupt/`)

- `gdt_init()`: Builds the kernel's own GDT/TSS (kernel/user segments, IST stacks) and loads it.
- `interrupt_init()`: Fills all 256 IDT vectors from the assembly stubs in `isr_stubs.s` and loads the IDT.
- `interrupt_register_handler()` / `irq_register_handler()`: Register C handlers taking a `struct trap_frame *`.
- CPU exceptions print a register dump; kernel-mode exceptions panic, user-mode exceptions terminate the process.

### Paging (`paging/`)

x86_64 4-level paging, higher-half layout (see `include/phys.h`): user space in the lower half (empty at boot),
HHDM (all physical memory, 2 MiB pages, NX) at `0xFFFF800000000000`, kernel image at `0xFFFFFFFF80000000` with
4 KiB pages and W^X permissions (the boot-stack guard page is unmapped; the image is read-only in the HHDM).

- `paging_init(info)`: Builds the page tables (does not switch to them yet).
- `paging_enable()`: Enables EFER.NXE / CR0.WP / CR4.SMEP (if supported) and loads CR3.
- `addr_space_t`: a process's own address space (its own PML4). The upper half (HHDM + kernel image) is shared by
  every address space; only the lower half (user space) is private. `paging_addrspace_create()` makes a new, empty
  one; `paging_addrspace_destroy()` walks its user region and returns every frame — page tables and data pages
  alike — to the physical frame allocator. Never call it while that address space is the one currently loaded.
- `paging_map_page(as, phys, virt, flags)` / `paging_unmap_page()` / `paging_unmap_free_page()`: map/unmap one
  4 KiB page in a given address space's user region (W^X is enforced); the `_free` variant also reclaims the
  frame, used when shrinking the heap.
- `paging_is_user_range(as, ...)`: validates that a user pointer range is user-accessible in `as` (used by system
  calls, always called with the calling process's own address space).
- `paging_kernel_cr3()` / `paging_kernel_addrspace()`: the shared address space kernel threads run under when they
  have none of their own (empty user region). The latter exists only for exercising the mapping API outside of a
  real process (self-tests); production code always goes through `paging_addrspace_create()`.

There is no `paging_disable()`: paging cannot be turned off in long mode.

### Memory (`mm/`)

`pfa.c`: a physical frame allocator — a free-list threaded through the free pages themselves (each free page's
first 8 bytes hold the physical address of the next one), so it needs no memory proportional to RAM size. Every
frame it hands out is zeroed first, so a process can never read what a previous owner (kernel bootstrap data, or
another process's freed memory) left behind. `pfa_init()` runs right after `paging_enable()` and excludes low
memory, the kernel image, and initrd by physical address regardless of what the boot memory map claims —
Multiboot2/GRUB's E820-derived map can report those regions as "available" even though they're in use.

### Process (`process/`)

Cooperative kernel threads with round-robin scheduling and an assembly context switch (`switch.s`). User
processes are kernel threads with their own `addr_space_t` that immediately drop into ring 3.

- `process_init()`: Registers the boot thread as the idle process (pid 0).
- `process_create(entry, arg)`: Creates a kernel thread with its own kernel stack.
- `process_create_user(as, entry, stack_top, heap_start, out_pid)`: registers a user process. All of its fields
  (address space, ring-3 entry point, stack, heap bounds) are set *before* it's linked into the scheduler's list,
  so a timer tick can never pick it up half-initialized.
- `process_switch()`: always reloads CR3 — a process's own address space if it has one, otherwise the shared
  kernel one — so the address space `uaccess` validates against and the one the CPU actually uses are always the
  same. `schedule()` wraps it with the round-robin policy (`sched_rr.h`).
- `process_terminate()` / `process_exit()` / `process_exit_with_code()`: terminate a process, recording an exit
  code. Handles close immediately; a terminated process's address space (and therefore its ELF image, heap, and
  stack) is only torn down once nothing could still be running under its CR3 — during `process_reap()`, deferred
  until the next process lifecycle event, and forceable with `process_reap_terminated()`.
- `process_get_exit_code()` / `process_count()`: query a terminated-but-not-yet-reaped process's exit code, or how
  many processes are alive.

`elf.c` (`elf_load()`): validates and maps an ET_EXEC ELF64 binary's `PT_LOAD` segments into a fresh address
space, streaming file bytes straight from the VFS handle into freshly allocated pages (no bulk buffer, since the
kernel still has no general-purpose heap of its own). `p_vaddr` must be page-aligned; the entry point must land
inside an executable segment. On any failure the caller discards the whole address space, so `elf_load()` doesn't
try to partially unwind — a page it already mapped stays mapped until then rather than risking a frame that's
unmapped without being freed.

`umem.c` (`umem_brk()`): brk-style heap growth/shrink for a process (`NxVirtualAlloc`). Growing maps freshly
zeroed pages; shrinking unmaps and frees whole pages beyond the new break. Both a per-call and a per-process total
cap apply, so one process can't exhaust the frame allocator in a single call.

`spawn.c` (`process_spawn()`): what `NxProcessCreate` actually calls — opens the path, creates the address space,
loads the ELF, maps a 64 KiB stack with an unmapped guard page below it (so overflow faults immediately instead
of corrupting something), and registers the process. Any failure at any step tears down everything already built.

### System calls (`syscall/`)

`int 0x80` and `syscall`/`sysret` (`syscall_entry.s`). `rax` = number, `rdi, rsi, rdx, r10, r8, r9` = arguments,
result in `rax` (negative = `Nstatus` error). `syscall` clobbers `rcx`/`r11`. Numbers, struct layouts and
constants are in `include/nyx_abi.h`, a public header shared with userland (kernel-internal types don't leak into it).

Implemented so far (see `syscalls.txt` for the full ABI table): `NxGetVersion`, `NxGetTime`, `NxSleep`, `NxYield`,
`NxSysInfo`, `NxOpen`/`NxClose`/`NxRead`/`NxWrite`/`NxSeek`/`NxStat`/`NxDuplicateHandle`, `NxProcessCreate`,
`NxProcessExit`, `NxProcessInfo`, `NxVirtualAlloc` (brk-style; `NxVirtualFree`'s number is reserved for a future
mmap-style API — for now a negative `NxVirtualAlloc` increment *is* "free"), `NxKernelPrint`, `NxDebugNop`.
Everything else returns `NsyscallFailed`. The kernel returns `Nstatus`, never POSIX `errno` — that translation is
nlibc's job (`userland/nlibc/library/errno.h`), kept out of the kernel ABI on purpose.

Two supporting layers:
- `uaccess.c`/`uaccess_asm.s`: the only code allowed to dereference a user pointer. Every access is checked against
  the page tables (`paging_is_user_range`, against the calling process's own `addr_space_t`) *and* done through a
  copy routine that recovers from a page fault instead of crashing the kernel, closing the check-then-use race
  where a page could be unmapped between the check and the actual access. `#PF` in the kernel is only ever treated
  as "bad user access" when it happened at that exact instruction; any other kernel-mode `#PF` is still a real bug
  and panics.
- `handles.c`: a per-process, capability-style handle table (files + stdin/stdout/stderr — every new process gets
  its own, freshly bound to the console; it does not inherit its parent's). Every handle carries a set of rights;
  `NxDuplicateHandle` can only narrow rights, never widen them. Handle values embed a generation counter so a
  closed slot's old value can't be reused to reach whatever reuses that slot next.

### Testing

- `make check` : strict C89 (`-std=c89 -pedantic -Wall -Wextra -Werror`) build at `-O0` and `-O2`.
- `make test-host` : FAT16/VFS/ramdisk regression tests and mutation fuzzing under ASan/UBSan.
- `make SELFTEST=1 os` : in-kernel self-test (system calls, threads, timer, paging, ring 3 with both syscall paths, and `NxProcessCreate` loading and running `userland/elftest/` as a real ELF64 process — including that its address space is fully reclaimed on exit). `SELFTEST=2..7` trigger exceptions.

All functions return `Nstatus` for error handling.

## 한국어

이 디렉토리에는 Nyxis 운영체제의 핵심 커널 컴포넌트가 포함되어 있습니다.

### 부트 (`boot/`)

`boot.s` 에는 진입점이 둘 있습니다: `_start`(64비트, UEFI/NYTB, `rdi = NTBLI`)와 `_start_mb`(32비트, Multiboot2/GRUB, BIOS 또는 UEFI).
둘 다 부트 정보를 복사하고 임시 페이지 테이블(낮은 4GiB 항등 + higher-half 커널)을 만든 뒤 `kernel_main(boot_kind, info_phys)` 로 점프합니다.
`multiboot2.c` 가 Multiboot2 정보를 `NTBLI` 로 변환하므로 커널은 펌웨어/부트 방식에 종속되지 않습니다. `make iso` 로 GRUB ISO(BIOS+UEFI 겸용)를 만듭니다.

### 인터럽트 (`interrupt/`)

- `gdt_init()`: 커널 전용 GDT/TSS(커널/유저 세그먼트, IST 스택)를 만들어 로드합니다.
- `interrupt_init()`: `isr_stubs.s` 의 스텁으로 256개 IDT 벡터를 모두 채우고 IDT 를 로드합니다.
- `interrupt_register_handler()` / `irq_register_handler()`: `struct trap_frame *` 를 받는 C 핸들러를 등록합니다.
- CPU 예외는 레지스터 덤프를 출력합니다. 커널 모드 예외는 패닉, 유저 모드 예외는 해당 프로세스만 종료합니다.

### 페이징 (`paging/`)

x86_64 4단계 페이징, higher-half 레이아웃입니다 (`include/phys.h` 참고). 하위 절반은 사용자 공간(부팅 직후 비어 있음),
`0xFFFF800000000000` 에 HHDM(물리 메모리 전체, 2MiB 페이지, NX), `0xFFFFFFFF80000000` 에 커널 이미지(4KiB 페이지, W^X)가 있습니다.
(부트 스택 가드 페이지는 매핑하지 않고, 커널 이미지의 HHDM 별칭은 읽기 전용입니다)

- `paging_init(info)`: 페이지 테이블을 구성합니다 (아직 전환하지 않음).
- `paging_enable()`: EFER.NXE / CR0.WP / CR4.SMEP(지원 시) 를 켜고 CR3 를 로드합니다.
- `addr_space_t`: 프로세스 하나의 독립된 주소 공간(자기만의 PML4)입니다. 상위 절반(HHDM +
  커널 이미지)은 모든 주소 공간이 공유하고, 하위 절반(사용자 공간)만 각자 따로 갖습니다.
  `paging_addrspace_create()` 가 빈 주소 공간을 새로 만들고, `paging_addrspace_destroy()` 가
  그 사용자 영역을 순회하며 페이지 테이블과 데이터 페이지를 전부 물리 프레임 할당자로
  되돌립니다. 그 주소 공간이 현재 실려 있는 CR3 일 때는 절대 호출하면 안 됩니다.
- `paging_map_page(as, phys, virt, flags)` / `paging_unmap_page()` / `paging_unmap_free_page()`:
  특정 주소 공간의 사용자 영역에 4KiB 페이지 하나를 매핑/해제합니다 (W^X 강제). `_free`
  버전은 프레임까지 회수하며, 힙을 줄일 때 씁니다.
- `paging_is_user_range(as, ...)`: `as` 안에서 유저 포인터 범위가 유저 접근 가능한지
  검사합니다 (시스템 콜에서, 항상 "호출한 프로세스 자신의" 주소 공간으로 사용).
- `paging_kernel_cr3()` / `paging_kernel_addrspace()`: 전용 주소 공간이 없는 커널 스레드가
  공유하는 주소 공간(사용자 영역이 비어 있음)입니다. 뒤의 것은 실제 프로세스 없이 매핑
  API 자체를 검사하는 자체 점검용이며, 실제 코드는 항상 `paging_addrspace_create()` 를
  거쳐야 합니다.

롱 모드에서는 페이징을 끌 수 없으므로 `paging_disable()` 은 없습니다.

### 메모리 (`mm/`)

`pfa.c`: 물리 프레임 할당자입니다. 자유 페이지 자신의 첫 8바이트에 "다음 자유 페이지의
물리 주소"를 저장하는 연결 리스트 방식이라, RAM 크기에 비례하는 메모리가 필요 없습니다.
내주는 모든 프레임은 항상 먼저 0 으로 지웁니다 — 그러지 않으면 이전 소유자(부팅 초기
데이터, 또는 다른 프로세스가 반납한 메모리)의 내용을 새 소유자가 그대로 읽을 수 있습니다.
`pfa_init()` 은 `paging_enable()` 직후 실행되며, 부트 메모리 맵이 뭐라고 보고하든 상관없이
저 1MiB · 커널 이미지 · initrd 를 물리 주소로 직접 제외합니다 — Multiboot2/GRUB 의
E820 기반 맵은 이미 쓰이고 있는 그 영역들도 "사용 가능"으로 보고할 수 있기 때문입니다.

### 프로세스 (`process/`)

어셈블리 컨텍스트 스위치(`switch.s`)를 쓰는 협력형 커널 스레드 + 라운드 로빈 스케줄러입니다.
사용자 프로세스는 자기만의 `addr_space_t` 를 가지고 바로 ring3 로 내려가는 커널 스레드입니다.

- `process_init()`: 부트 스레드를 idle 프로세스(pid 0)로 등록합니다.
- `process_create(entry, arg)`: 자기 커널 스택을 가진 커널 스레드를 만듭니다.
- `process_create_user(as, entry, stack_top, heap_start, out_pid)`: 사용자 프로세스를
  등록합니다. 모든 필드(주소 공간, ring3 진입점, 스택, 힙 범위)를 스케줄러 목록에 연결하기
  *전에* 채우므로, 타이머 인터럽트가 절반만 초기화된 프로세스를 실행할 일이 없습니다.
- `process_switch()`: 항상 CR3 를 다시 로드합니다 — 프로세스가 전용 주소 공간을 가지면 그것,
  없으면 공유 커널 주소 공간을. 그래야 `uaccess` 가 검사하는 주소 공간과 CPU 가 실제로 쓰는
  주소 공간이 항상 같습니다. `schedule()` 이 라운드 로빈 정책(`sched_rr.h`)으로 이를 감쌉니다.
- `process_terminate()` / `process_exit()` / `process_exit_with_code()`: 종료 코드를 남기고
  프로세스를 종료합니다. 핸들은 즉시 닫히지만, 종료된 프로세스의 주소 공간(따라서 ELF
  이미지·힙·스택)은 그 CR3 로 아무것도 더 실행되고 있지 않다고 보장되는 시점 —
  `process_reap()`, 다음 프로세스 생애주기 이벤트까지 미뤄지며 `process_reap_terminated()`
  로 즉시 강제할 수 있습니다 — 에만 회수됩니다.
- `process_get_exit_code()` / `process_count()`: 종료됐지만 아직 회수 전인 프로세스의 종료
  코드를 조회하거나, 살아 있는 프로세스 수를 셉니다.

`elf.c` (`elf_load()`): ET_EXEC ELF64 실행 파일의 `PT_LOAD` 세그먼트를 검증하며 새 주소
공간에 매핑합니다. 파일 바이트를 VFS 핸들에서 새로 할당한 페이지로 곧바로 스트리밍해
넣습니다(커널에 아직 범용 힙이 없으므로 큰 버퍼를 만들지 않습니다). `p_vaddr` 는 페이지
정렬이어야 하고, 진입점은 실행 가능한 세그먼트 안에 있어야 합니다. 실패하면 호출자가
주소 공간 전체를 버리므로, `elf_load()` 자신은 부분적으로 되돌리려 하지 않습니다 — 이미
매핑한 페이지를 어설프게 해제하다가 "매핑은 없는데 회수도 안 된" 프레임을 만드는 쪽이
더 위험하기 때문입니다.

`umem.c` (`umem_brk()`): 프로세스의 brk 방식 힙 늘리기/줄이기(`NxVirtualAlloc`)입니다.
늘릴 때는 방금 0 으로 지운 페이지를 매핑하고, 줄일 때는 새 break 를 넘어선 페이지를
통째로 해제합니다. 한 번의 호출당 상한과 프로세스 전체 누적 상한을 함께 적용해서,
프로세스 하나가 한 번에 물리 프레임 할당자를 고갈시키지 못하게 합니다.

`spawn.c` (`process_spawn()`): `NxProcessCreate` 가 실제로 호출하는 곳입니다. 경로를 열고,
주소 공간을 만들고, ELF 를 적재하고, 아래에 매핑하지 않은 가드 페이지가 있는 64KiB 스택을
매핑하고(오버플로가 다른 것을 덮어쓰지 않고 즉시 폴트로 드러나게), 프로세스를 등록합니다.
어느 단계에서든 실패하면 그때까지 만든 것을 전부 정리합니다.

### 시스템 콜 (`syscall/`)

`int 0x80` 과 `syscall`/`sysret`(`syscall_entry.s`) 둘 다 지원. `rax` = 번호, `rdi, rsi, rdx, r10, r8, r9` = 인자,
결과는 `rax` (음수 = `Nstatus` 오류). `syscall` 은 `rcx`/`r11` 을 파괴합니다. 번호/구조체/상수는 유저랜드와 공유하는
공개 헤더 `include/nyx_abi.h` 에 있습니다 (커널 내부 타입은 여기 노출되지 않습니다).

지금까지 구현: `NxGetVersion`, `NxGetTime`, `NxSleep`, `NxYield`, `NxSysInfo`,
`NxOpen`/`NxClose`/`NxRead`/`NxWrite`/`NxSeek`/`NxStat`/`NxDuplicateHandle`, `NxProcessCreate`, `NxProcessExit`,
`NxProcessInfo`, `NxVirtualAlloc`(brk 방식 — `NxVirtualFree` 번호는 미래의 mmap 스타일 API 를 위해 예약만 해
두었고, 지금은 `NxVirtualAlloc` 에 음수를 넘기는 것이 "해제"입니다), `NxKernelPrint`, `NxDebugNop`
(전체 ABI 표는 `syscalls.txt` 참고). 나머지 번호는 `NsyscallFailed` 를 반환합니다. 커널은 POSIX `errno` 를
전혀 모르고 항상 `Nstatus` 를 돌려줍니다 — 그 변환은 일부러 커널 ABI 밖, nlibc 쪽
(`userland/nlibc/library/errno.h`) 이 맡습니다.

보조 계층 둘:
- `uaccess.c`/`uaccess_asm.s`: 사용자 포인터를 역참조할 수 있는 유일한 코드입니다. 모든 접근은 페이지 테이블
  검사(`paging_is_user_range`, 호출한 프로세스 자신의 `addr_space_t` 로)와, 폴트가 나면 커널을 죽이는 대신
  복구하는 복사 루틴 둘 다를 거칩니다. 그래서 "검사 -> 실제 접근" 사이에 매핑이 바뀌는 경쟁(TOCTOU)도
  안전합니다. 커널 모드 `#PF` 는 그 복사 명령에서 정확히 발생했을 때만 "잘못된 사용자 접근" 으로 처리하고,
  그 외의 커널 모드 `#PF` 는 여전히 진짜 버그로 간주해 패닉합니다.
- `handles.c`: 프로세스별 capability 방식 핸들 테이블입니다(파일 + stdin/stdout/stderr — 새 프로세스마다
  콘솔에 새로 연결된 자기 것을 받고, 부모의 것을 상속하지 않습니다). 모든 핸들은 권한을 가지고,
  `NxDuplicateHandle` 은 권한을 줄이기만 할 수 있고 늘릴 수 없습니다. 핸들 값에는 세대(generation)가
  들어 있어서, 닫힌 슬롯의 옛 핸들 값으로 그 슬롯을 재사용한 다른 객체에 접근할 수 없습니다.

### 테스트

- `make check` : 엄격한 C89 (`-std=c89 -pedantic -Wall -Wextra -Werror`) 빌드를 `-O0`, `-O2` 로 수행.
- `make test-host` : FAT16/VFS/램디스크 회귀 테스트 + ASan/UBSan 변이 퍼징.
- `make SELFTEST=1 os` : 커널 내부 자체 점검(시스템 콜, 스레드, 타이머, 페이징, 두 시스템 콜 경로를 쓰는 ring 3 프로그램, 그리고 `NxProcessCreate` 로 `userland/elftest/` 를 실제 ELF64 프로세스로 적재·실행 — 종료 시 주소 공간이 완전히 회수되는지까지 확인). `SELFTEST=2..7` 은 예외를 일부러 발생시킵니다.

모든 함수는 오류 처리를 위해 `Nstatus`를 반환합니다.
