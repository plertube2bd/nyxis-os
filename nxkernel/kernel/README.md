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
- `paging_map_page(phys, virt, flags)` / `paging_unmap_page()`: Map/unmap one 4 KiB user-space page (W^X is enforced).
- `paging_is_user_range()`: Validates that a user pointer range is user-accessible (used by system calls).

There is no `paging_disable()`: paging cannot be turned off in long mode.

### Process (`process/`)

Cooperative kernel threads with round-robin scheduling and an assembly context switch (`switch.s`).

- `process_init()`: Registers the boot thread as the idle process (pid 0).
- `process_create(entry, arg)`: Creates a kernel thread with its own kernel stack.
- `process_switch()` / `schedule()`: Switch context.
- `process_terminate()` / `process_exit()`: Terminate a process.

### System calls (`syscall/`)

`int 0x80` and `syscall`/`sysret` (`syscall_entry.s`). `rax` = number, `rdi, rsi, rdx, r10, r8, r9` = arguments,
result in `rax` (negative = `Nstatus` error). `syscall` clobbers `rcx`/`r11`. Numbers, struct layouts and
constants are in `include/nyx_abi.h`, a public header shared with userland (kernel-internal types don't leak into it).

Implemented so far (see `syscalls.txt` for the full ABI table): `NxGetVersion`, `NxGetTime`, `NxSleep`, `NxYield`,
`NxSysInfo`, `NxOpen`/`NxClose`/`NxRead`/`NxWrite`/`NxSeek`/`NxStat`/`NxDuplicateHandle`, `NxProcessExit`,
`NxProcessInfo`, `NxKernelPrint`, `NxDebugNop`. Everything else returns `NsyscallFailed`.

Two supporting layers:
- `uaccess.c`/`uaccess_asm.s`: the only code allowed to dereference a user pointer. Every access is checked against
  the page tables (`paging_is_user_range`) *and* done through a copy routine that recovers from a page fault instead
  of crashing the kernel, closing the check-then-use race where a page could be unmapped between the check and the
  actual access. `#PF` in the kernel is only ever treated as "bad user access" when it happened at that exact
  instruction; any other kernel-mode `#PF` is still a real bug and panics.
- `handles.c`: a per-process, capability-style handle table (files + stdin/stdout/stderr). Every handle carries a
  set of rights; `NxDuplicateHandle` can only narrow rights, never widen them. Handle values embed a generation
  counter so a closed slot's old value can't be reused to reach whatever reuses that slot next.
User pointers are validated with `paging_is_user_range()` before use.

### Testing

- `make check` : strict C89 (`-std=c89 -pedantic -Wall -Wextra -Werror`) build at `-O0` and `-O2`.
- `make test-host` : FAT16/VFS/ramdisk regression tests and mutation fuzzing under ASan/UBSan.
- `make SELFTEST=1 os` : in-kernel self-test (system calls, threads, timer, paging, ring 3 with both syscall paths). `SELFTEST=2..7` trigger exceptions.

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
- `paging_map_page(phys, virt, flags)` / `paging_unmap_page()`: 사용자 공간 4KiB 페이지 하나를 매핑/해제합니다 (W^X 강제).
- `paging_is_user_range()`: 유저 포인터 범위가 유저 접근 가능한지 검사합니다 (시스템 콜에서 사용).

롱 모드에서는 페이징을 끌 수 없으므로 `paging_disable()` 은 없습니다.

### 프로세스 (`process/`)

어셈블리 컨텍스트 스위치(`switch.s`)를 쓰는 협력형 커널 스레드 + 라운드 로빈 스케줄러입니다.

- `process_init()`: 부트 스레드를 idle 프로세스(pid 0)로 등록합니다.
- `process_create(entry, arg)`: 자기 커널 스택을 가진 커널 스레드를 만듭니다.
- `process_switch()` / `schedule()`: 문맥 전환.
- `process_terminate()` / `process_exit()`: 프로세스 종료.

### 시스템 콜 (`syscall/`)

`int 0x80` 과 `syscall`/`sysret`(`syscall_entry.s`) 둘 다 지원. `rax` = 번호, `rdi, rsi, rdx, r10, r8, r9` = 인자,
결과는 `rax` (음수 = `Nstatus` 오류). `syscall` 은 `rcx`/`r11` 을 파괴합니다. 번호/구조체/상수는 유저랜드와 공유하는
공개 헤더 `include/nyx_abi.h` 에 있습니다 (커널 내부 타입은 여기 노출되지 않습니다).

지금까지 구현: `NxGetVersion`, `NxGetTime`, `NxSleep`, `NxYield`, `NxSysInfo`,
`NxOpen`/`NxClose`/`NxRead`/`NxWrite`/`NxSeek`/`NxStat`/`NxDuplicateHandle`, `NxProcessExit`, `NxProcessInfo`,
`NxKernelPrint`, `NxDebugNop` (전체 ABI 표는 `syscalls.txt` 참고). 나머지 번호는 `NsyscallFailed` 를 반환합니다.

보조 계층 둘:
- `uaccess.c`/`uaccess_asm.s`: 사용자 포인터를 역참조할 수 있는 유일한 코드입니다. 모든 접근은 페이지 테이블
  검사(`paging_is_user_range`)와, 폴트가 나면 커널을 죽이는 대신 복구하는 복사 루틴 둘 다를 거칩니다. 그래서
  "검사 -> 실제 접근" 사이에 매핑이 바뀌는 경쟁(TOCTOU)도 안전합니다. 커널 모드 `#PF` 는 그 복사 명령에서
  정확히 발생했을 때만 "잘못된 사용자 접근" 으로 처리하고, 그 외의 커널 모드 `#PF` 는 여전히 진짜 버그로
  간주해 패닉합니다.
- `handles.c`: 프로세스별 capability 방식 핸들 테이블(파일 + stdin/stdout/stderr)입니다. 모든 핸들은 권한을
  가지고, `NxDuplicateHandle` 은 권한을 줄이기만 할 수 있고 늘릴 수 없습니다. 핸들 값에는 세대(generation)가
  들어 있어서, 닫힌 슬롯의 옛 핸들 값으로 그 슬롯을 재사용한 다른 객체에 접근할 수 없습니다.
유저 포인터는 사용 전에 `paging_is_user_range()` 로 검증합니다.

### 테스트

- `make check` : 엄격한 C89 (`-std=c89 -pedantic -Wall -Wextra -Werror`) 빌드를 `-O0`, `-O2` 로 수행.
- `make test-host` : FAT16/VFS/램디스크 회귀 테스트 + ASan/UBSan 변이 퍼징.
- `make SELFTEST=1 os` : 커널 내부 자체 점검(시스템 콜, 스레드, 타이머, 페이징, 두 시스템 콜 경로를 쓰는 ring 3 프로그램). `SELFTEST=2..7` 은 예외를 일부러 발생시킵니다.

모든 함수는 오류 처리를 위해 `Nstatus`를 반환합니다.
