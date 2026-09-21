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
result in `rax` (negative = `Nstatus` error). `syscall` clobbers `rcx`/`r11`.
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
결과는 `rax` (음수 = `Nstatus` 오류). `syscall` 은 `rcx`/`r11` 을 파괴합니다.
유저 포인터는 사용 전에 `paging_is_user_range()` 로 검증합니다.

### 테스트

- `make check` : 엄격한 C89 (`-std=c89 -pedantic -Wall -Wextra -Werror`) 빌드를 `-O0`, `-O2` 로 수행.
- `make test-host` : FAT16/VFS/램디스크 회귀 테스트 + ASan/UBSan 변이 퍼징.
- `make SELFTEST=1 os` : 커널 내부 자체 점검(시스템 콜, 스레드, 타이머, 페이징, 두 시스템 콜 경로를 쓰는 ring 3 프로그램). `SELFTEST=2..7` 은 예외를 일부러 발생시킵니다.

모든 함수는 오류 처리를 위해 `Nstatus`를 반환합니다.
