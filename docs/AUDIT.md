# Nyxis OS 커널 소스 감사(Audit) 및 개선 보고서

대상: `main` @ d85c6d2 (nxkernel/, drivers/, include/, bootloader/, userland/ 전체)
작업 브랜치: `fix/kernel-audit`
검증 환경: Ubuntu 24.04, gcc 13.3, gnu-efi, QEMU 8.2 + OVMF(4M, Secure Boot 없음)

## 0. 요약

수정 전 상태는 **QEMU/OVMF 에서 부팅하면 화면에 아무것도 출력하지 못하고 조용히 패닉**하는 상태였다
(RIP 가 `cli; hlt` 패닉 루프, RFLAGS.IF=0 으로 확인). 원인은 하나가 아니라 여러 개가 겹쳐 있었다.

| 순번 | 문제 (1·2번은 실제로 확인, 3~5번은 그 다음 단계에서 막았을 잠복 문제) | 결과 |
|---|---|---|
| 1 | `NTBLI` 구조체 레이아웃이 부트로더(width 오프셋 56)와 커널(오프셋 52)에서 서로 달랐음 | 커널이 해상도 0 으로 읽음 -> `printk_init` 실패 -> 패닉(출력 수단 없음) |
| 2 | 부트로더의 `InitrdAddr` 그림자 변수 | initrd 주소가 항상 NULL -> "No initrd" |
| 3 | IDT 게이트 셀렉터 0x08 (UEFI GDT 의 64비트 코드 세그먼트는 0x38) + IDT 대부분이 비어 있음 | 모든 예외/IRQ/`int 0x80` 이 트리플 폴트 |
| 4 | 커널이 `sti` 하지만 PIC 리맵/타이머/핸들러가 없음 | IRQ 가 예외 벡터(#DF 등)로 들어옴 |
| 5 | `AHCI` 미발견 시 패닉, PCI 스캔 `for (u8 bus...; bus < 256)` 무한 루프 | AHCI 없는 구성에서 정지 |

수정 후: q35(AHCI) / pc(IDE) / VGA 없음 / BltOnly(virtio-gpu) / `-cpu max` / RAM 2~3GB(4GB 초과 영역 포함) /
`-O0`,`-O2`,`-O3` 빌드 모두 부팅하고 내장 자체 점검(`SELFTEST=1`)이 통과한다.

## 1. 발견 사항과 조치

심각도: **[C]** 치명(부팅 불가/임의 메모리 손상), **[H]** 높음(보안/데이터 손상), **[M]** 중간, **[L]** 낮음

### 1.1 공통 헤더 (include/)

- **[C] `usize` 크기가 include 순서에 의존**: `types.h` 가 `NYXIS_64BITS` 를 `nyxis.h` 에서 받았기 때문에
  `types.h` 를 먼저 포함한 파일은 `usize == u32` 였다. NTBLI 의 크기/오프셋이 TU 마다 달랐다 (52 vs 56, 72 vs 80 바이트).
  -> `types.h` 가 스스로 x86_64 를 판별. NTBLI 를 고정폭 필드로 재정의하고 `magic/version/size` 를 추가.
  `NX_STATIC_ASSERT` 로 크기(112바이트) 고정. 커널은 magic/version/size 를 검증한 뒤 **사본**을 사용.
- **[H] `spin_unlock` 이 무조건 `sti()`**: 인터럽트가 꺼져 있어야 하는 구간에서 인터럽트를 켬 (VFS 초기화 중 IDT 없이 IRQ 발생 가능).
  락 획득 순서도 "락 -> cli" 라 교착 위험. -> `irq_save/irq_restore` 로 RFLAGS 저장/복원, 순서 수정 (`sync.h` 신설).
- **[M] atomic 이 `multicore_enabled == false` 이면 비원자적 `v->value++`**: 인터럽트 핸들러와 경쟁 가능. -> 항상 `lock` 접두사.
- **[M] `cli/sti/hlt` 에 "memory" clobber 없음**, `cpuid` 서브리프 미지정, `pack` 매크로의 끝 `;`, 식별자 오염(`interrupt`).
- **[M] `NSTATUS_ERR_FLAG = (i32)1 << 31`**: 부호 있는 정수 오버플로(UB). -> `INT_MIN` 직접 표기.
- C89 위반: `_Bool`, `inline`, `//`, enum 끝 쉼표, `long long`, 혼합 선언, 비트필드 형식, `#pragma once` 등 전부 정리.
- `string.h`: `utf8*`(unsigned char) 인터페이스 -> 표준 `char*` (`-Wpointer-sign`), `strnlen` 추가.
- `interrupt.h`: "32비트용" 이라는 주석과 실제 64비트 구조체 혼재, 실제 스택과 맞지 않는 프레임 구조체, 깨진 `zero_div/find_current_status` 선언 -> 재작성.

### 1.2 부트로더 (bootloader/nytb/nytb3.c)

- **[C] `InitrdAddr` 그림자 변수** (위 표 2번). **[C]** NTBLI 불일치 (위 표 1번).
- **[H] ELF 로더가 파일 값을 무검증 신뢰**: `e_phoff`, `e_phnum`, `p_offset`, `p_filesz`, `p_paddr`, `p_memsz` 로 임의 메모리 읽기/쓰기 가능.
  -> 모든 범위를 오버플로 없는 뺄셈 비교로 검증, 적재 주소 1MiB~1GiB 제한, 진입점이 실행 가능 세그먼트 안인지 확인, 전체 범위 1회 할당.
- **[H] ELF32 경로**: 64비트 UEFI 앱이 32비트 커널로 점프 -> 반드시 크래시. 삭제.
- **[H] `PixelBltOnly` GOP 의 `FrameBufferBase` 를 그대로 전달**: 커널이 유효하지 않은 주소에 그림. -> 프레임버퍼 없음으로 전달하고 커널은 시리얼만 사용.
- **[M]** GOP 없으면 부팅 중단 -> 계속 진행. `ExitBootServices` 1회 실패 시 종료 -> 재시도. 메모리 맵 미전달 -> NTBLI 로 전달.
  파일 읽기가 실제 읽은 크기를 확인하지 않음, 크기 상한 없음, `FreePool(NULL)`, 워치독 미해제, RSDP 미전달, `%p`/`%u` 서식 오류.
- 테스트 패턴 그리기가 항상 켜져 있던 것을 `-DNYTB_DEBUG`(make DEBUG=1) 로 제한.

### 1.3 커널 진입/CPU 구조 (start.s, kernel.c, interrupt, paging, process, syscall, timer)

- **[C] IDT/GDT** (위 표 3, 4번). -> 커널 전용 GDT/TSS(IST 3개: #DF/NMI/#MC), 256 벡터 스텁(`isr_stubs.s`), 예외 덤프, PIC 리맵.
- **[C] 페이징이 32비트 2단계 방식**이고 포인터를 `(u32)` 로 잘랐으며, 페이지 테이블을 고정 주소 0x200000 에 "그냥 증가" 할당(커널 .bss 와 겹칠 수 있음). `paging_disable` 은 롱 모드에서 #GP.
  -> x86_64 4단계 페이징 전면 재작성: 항등 매핑(2MiB) + 커널 이미지 4KiB W^X, NX/WP/SMEP, NULL 페이지/스택 가드 페이지 매핑 해제.
- **[C] `process_create` 가 `process_list + sizeof * pid` 라는 미할당 주소에 구조체를 씀**(메모리 손상). `process_switch` 는 인라인 asm 을 문장별로 쪼개
  레지스터를 저장/복원(컴파일러가 asm 사이에 레지스터를 재사용하므로 무의미, rsp 교체 후 C 코드 계속 실행). -> 정적 테이블 + 어셈블리 컨텍스트 스위치.
- **[H] 시스템 콜**: 유저 포인터를 검증 없이 `printk(message)` -> 임의 커널 메모리 읽기 + 서식 문자열 취약점 + 무한 읽기.
  naked asm 의 인자 재배치가 SysV ABI 와 불일치(4번째 인자가 rcx 인데 r10 사용), 스택 정렬 미보장.
  -> trap_frame 기반 처리, `paging_is_user_range` 검증, 256바이트 복사 후 `printk("%s", buf)`, 반환은 Nstatus 부호 확장.
- **[H] `start.s`**: 고정 물리 주소(0x1000000)를 스택으로 사용, 동작 불가능한 Multiboot2 헤더(32비트로 진입시키는 로더에서 64비트 코드 실행), `cli/cld` 없음.
  -> 커널 .bss 안 32KiB 스택 + 가드 페이지. Multiboot 변환 코드(`boot_info_bridge`) 삭제(NTBLI 오인식 위험도 있었음).
- **[H] 부팅 순서**: 콘솔 초기화 전에 VFS 등을 초기화해서 그 사이 오류는 어디에도 표시되지 않았음. AHCI 실패/프레임버퍼 없음 = 패닉이었음.
  -> 시리얼(COM1) + 프레임버퍼 동시 출력, 실패는 경고 후 계속.
- **[M] PIT**: `outb/inb` 재정의로 컴파일 불가, `timer_tick` static/extern 충돌, 일반 함수를 ISR 로 사용(`ret` vs `iretq`), `idt_set_gate` 시그니처 불일치, 100Hz 하드코딩. (빌드에 포함되지 않아 가려져 있었음)
- **[M] 커널 컴파일 옵션**: `-mno-red-zone` 없음(인터럽트가 레드존을 덮어씀), SSE 사용 가능(ISR 이 XMM 미저장), `-O2` 시 `memset/memcpy` 자기 재귀 가능.

### 1.4 콘솔 (console/, graphics/)

- **[H] `printk(신뢰할 수 없는 문자열)` 가능한 구조** (시스템 콜에서 실제로 악용 가능했음). 512바이트 버퍼가 넘치면 **출력 자체를 버림**. 64비트 값 출력 불가(`%x` 는 32비트).
  -> 스트리밍 포매터(`%c %s %d %i %u %x %X %p %r`, `l/ll/z`, 폭/0채움), 화면 하단 스크롤, 경계 검사(`draw_char_bounded`).
- **[H] `graphics_base`**: `draw_hline_fast` 의 `u32 buffer[1024]` 스택 오버플로(length>1024), 좌표 검사 없음, `.c` 는 정의되지 않은 `memcopy` 호출(컴파일 불가). -> 재작성.
- **[M] `inputs_base`**: 컴파일 불가(선언/정의 충돌), `get_scancode` 에서 `sti()` 가 `return` 뒤에 있어 도달 불가 -> 읽을 때마다 인터럽트가 꺼진 채 반환.
- 폰트 주석 오류('.' 가 ',' 로, '`' 가 ''' 로 표기).

### 1.5 드라이버 (drivers/)

- **[C] FAT16 `memset(&dev->fs, 0, sizeof(filesystem_t))`**: 실제 타입은 `vfs_filesystem_t`(40B)인데 64B 를 지워서 **바로 뒤의 read/write 함수 포인터를 NULL 로 만들고**
  배열의 다음 원소까지 덮어씀 (마운트 직후 모든 파일 읽기 실패).
- **[H] FAT16 파서(신뢰할 수 없는 디스크 이미지)**: `BytesPerSector` 미검증 + 512바이트 스택 버퍼(스택 오버플로), 클러스터 번호 미검증(`cluster-2` 언더플로 -> 임의 오프셋 읽기),
  FAT 체인 순환 시 무한 루프, 8.3 이름을 8글자로 잘라 비교("HELLOWORLD.RUN" 이 "HELLOWOR.RUN" 과 일치), BPB/클러스터 수 검증 부족.
  -> 전면 검증(512B 섹터만, 클러스터 4085~65524, FAT 크기, 체인 길이 상한) + u64 산술.
- **[H] `fat16_file_read` 가 `handle->offset` 을 올리고 `vfs_read` 도 올려서** 오프셋이 이중 증가(여러 번 나눠 읽으면 데이터 누락).
- **[H] VFS 경로 해석이 "찾았는데도 NfileNotFound" 반환**(`create_missing == false` 일 때). `app` 네임스페이스 등록이 항상 실패하고 잘못된 대체 경로로 등록됨.
- **[H] ramdisk 범위 검사가 `start + size > limit` 덧셈 형태**(정수 오버플로로 우회 -> 임의 메모리 읽기/쓰기). -> 뺄셈 형태.
- **[H] AHCI**: `for (u8 bus = 0; bus < 256; bus++)` 무한 루프, 모든 하드웨어 대기 루프에 타임아웃 없음, PRDT 크기(4MiB)/48비트 LBA 미검증, PCI 명령 레지스터(BusMaster/MemSpace) 미설정,
  **UEFI 가 멈춘 포트의 시그니처(0xFFFFFFFF)를 보고 장치를 못 찾음** -> COMRESET 후 시그니처 재수신. (q35 에서 실제 SATA 디스크 섹터 0 읽기 확인)
- **[M] ATA PIO**: 타임아웃 없음/드라이브 부재(0xFF) 미검출/LBA28 초과 미검출. **[M] PIC**: 헤더의 static 정의, 리맵 오프셋 미기록, 스퓨리어스 IRQ 미처리.
- 리소스 누수(vnode, 장치 슬롯), `private` 필드명(C++ 예약어), 중복 typedef 등.

### 1.6 유저랜드 헤더

- **[H] `syscall_wrapper` 인라인 asm**: 모든 입력을 "r" 로 받은 뒤 asm 내부에서 mov -> 최적화 수준에 따라 인자가 뒤섞임. -> 레지스터 직접 바인딩.
- `helloworld` 가 유저 모드에서 `hlt`(특권 명령 -> #GP) 를 실행. 32비트 분기 삭제.

### 1.7 저장소/빌드

- `nxkernel/build.sh` 가 initrd 를 이미지에 넣지 않아(Makefile 과 결과가 다름) 항상 "No initrd loaded".
- Makefile: 20여 개 복붙 규칙, 경고 옵션 없음, 헤더 의존성 없음, 일부 소스는 빌드 대상에서 빠져 있어 컴파일 오류가 숨어 있었음. `make os` 가 추적 중인 `userland/initrd/initrd.img` 를 덮어씀.
- 기본 QEMU 구성(virtio-gpu)의 OVMF GOP 는 BltOnly 라 프레임버퍼가 없다. (아래 질문 참고)

## 2. API 변경 (호출자 영향)

- `NTBLI`: `version(usize)`, `id`, `usize` 필드 -> 고정폭 + `magic/size/pixel_format/memmap_*`. `id` 삭제.
- `paging_init()` -> `paging_init(const NTBLI *)`, `paging_map_page(..., u32 flags)` -> `u64 flags`(NX 비트), `paging_disable()` 삭제.
- `process_init(void *)` -> `process_init(void)`, `process_create(void *entry, ...)` -> `process_entry_t`. `process_t` 의 레지스터 필드 삭제.
- `syscall_dispatch` 반환형 `Nstatus` -> `i64`(부호 확장), `syscall_interrupt_handler` 삭제.
- `string.h` 의 `utf8*` -> `char*`. `vfs_filesystem_t.private` -> `priv`. `get_kernel_info()` 는 `const NTBLI *`.
- 삭제: `boot_info_bridge`(Multiboot2), `zero_div`, `find_current_status`, `interrupt` 매크로, `kernel/interrupt/interrupt.h`.

## 3. 검증

- `make check`: 커널 전체 `-std=c89 -pedantic -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wdeclaration-after-statement -Wvla` 를 `-O0`/`-O2` 로 컴파일. 부트로더는 `-std=c89 -Wall -Wextra -Werror ...`
  (gnu-efi 의 `uefi_call_wrapper` 가 함수 포인터를 `void*` 로 넘기므로 `-pedantic` 은 쓸 수 없음).
- `make test-host`: FAT16/VFS/ramdisk 를 ASan+UBSan 으로 호스트에서 실행. 회귀 12건 + 메타데이터 변이 퍼징(4000회, 크래시/행 0).
- `make SELFTEST=1 os` 후 QEMU: 시스템 콜 검증(커널/NULL/비정규 포인터 거부, 서식 문자열 비확장), 페이징 API(W^X 거부 포함), 두 커널 스레드 교대 실행, PIT 100Hz, 문자열 함수. 전부 통과.
- 예외/보호 검증(`SELFTEST=2..7`): #DE, NULL 역참조(#PF), 스택 오버플로(가드 페이지 -> IST 스택의 #DF 덤프), `.text` 쓰기(WP), 데이터 영역 실행(NX, 에러코드 0x11), #UD.
- 부팅 구성: q35+AHCI(섹터 0 DMA 읽기 확인), pc+IDE(AHCI 없음), `-vga none`(시리얼만), virtio-gpu(BltOnly), `-cpu max`, RAM 2GB / 3GB(4GB 이상 영역), -O0/-O2/-O3.

### 검증하지 못한 것 (정직하게)

- **실제 하드웨어**: 전부 QEMU 에서만 실행했다. AHCI COMRESET 지연, PIC/PIT 타이밍, 실제 UEFI 의 메모리 맵/프레임버퍼 특성은 미검증.
- SMEP 는 `-cpu max` 에서 부팅만 확인했고 SMEP 위반을 일으키는 테스트는 없다 (유저 모드 코드가 아직 없음).
- ring 3 진입(`enter_ring3`)과 `int 0x80` 의 유저 모드 호출 경로는 실행해 보지 못했다 (ring0 에서 `int 0x80` 은 검증).
- 4GB 초과 RAM 은 3GB 게스트(고메모리 약 1GB)까지만 확인.
- 이 저장소에는 Rust 코드가 없어서 Rust 부분은 다루지 않았다.

## 4. 남은 문제 / 알려진 한계

- VFS: mount/unmount 외의 경로에 락 없음, dentry/vnode 참조 카운트를 실제로 쓰지 않음 (멀티코어/선점 도입 전 필수 보완).
- 스케줄러는 협력형. 타이머 선점, ring3 프로세스 로더, 프로세스별 주소 공간 없음.
- 커널 스택 카나리(`-fstack-protector`)는 꺼져 있다 (TLS/GS 기반 canary 설정이 필요). KASLR 없음. SMAP 미사용(유저 접근 함수에 stac/clac 필요).
- 항등 매핑은 PML4[0] 전체를 차지하므로 유저 공간 설계와 충돌한다. 유저 프로세스 도입 전에 higher-half 커널 여부를 결정해야 한다.
- 프레임버퍼는 UC/WB 구분 없이 WB 로 매핑(MTRR 에 의존). PAT 로 WC 지정하면 스크롤이 빨라진다.
- `userland/initrd/initrd.c` 는 컴파일 불가능한 자리 표시자(`bimbimbambam;`)라 손대지 않았다. `nxkernel/kernel/linker32.lds`, 빈 `코드분석.rtf` 도 그대로 둠.
- `esp.img`, `esp_test.img`(각 64MiB), `initrd.img`(16MiB), `serial.log` 가 git 에 추적되고 있다.

## 5. 결정 사항 (질문에 대한 답변 반영)

| 항목 | 결정 | 상태 |
|---|---|---|
| 기본 QEMU 구성 | virtio-gpu(BltOnly) 대신 **std VGA(Bochs VBE) 선형 프레임버퍼**. 그래픽 장치가 없어도 시리얼로 동작 | 완료 (`make run`, `make run-usb`) |
| UEFI 비종속 | 커널은 부트 방식이 아니라 NTBLI 만 본다. 부트 방식마다 "정보 -> NTBLI" 어댑터만 추가 | 완료 (UEFI 직접 + GRUB Multiboot2 BIOS/UEFI) |
| 32/64비트 | **방안 C 선택**: 64비트 커널 하나 + 32비트는 부트(GRUB 32비트 진입)와 32비트 호환 모드 유저(GDT 에 32비트 유저 코드 세그먼트 준비) 지원을 먼저 만들고, 32비트 전용 커널(B)은 나중에 | A 부분 완료 / B 는 이후 |
| 시스템 콜 | `int 0x80` + `syscall/sysret` 둘 다, 반환은 Nstatus 부호 확장 | 완료 (ring 3 에서 두 경로 모두 실행 검증) |
| 유저 공간 | higher-half 커널 (HHDM + 커널 이미지 상위 매핑, 하위 절반은 사용자 공간) | 완료 |
| Multiboot2 / ELF32 / `linker32.lds` | 삭제하지 않는다. Multiboot2 는 제대로 동작하도록 다시 구현. ELF32 로더/`linker32.lds` 는 방안 B(32비트 커널)에서 의미가 생기므로 그때 구현 (이전 ELF32 경로는 롱 모드에서 32비트 커널로 점프하는 동작 불가 코드였음) | Multiboot2 완료 / ELF32 는 B 와 함께 |
| git 추적 바이너리 | 추적 해제 + `.gitignore` | 완료 |
| `helloworld` 링크 / `initrd.c` | 가능한 빨리 제거 예정. 그때까지 유지 | 유지 |
| 스케줄러 | 외부 진입점은 `schedule()` 하나, 정책은 인라인 헤더(`sched_rr.h`) | 완료 |
| Rust | 소유권/메모리 이동을 확신할 수 없는 부분에만 사용. 기존 C 는 질문 없이 바꾸지 않는다 | 해당 없음 |
| 라이선스 | `docs/LICENSING.md` | 완료 |

## 6. higher-half / Multiboot2 / syscall 구현 내용 (2차 작업)

- **메모리 레이아웃** (`include/phys.h`, `linker64.lds`): 커널은 물리 1MiB 에 적재되고 가상 `0xFFFFFFFF80000000 + 물리` 에 링크된다(`-mcmodel=kernel`).
  낮은 주소의 `.boot` 섹션에 초기 진입 코드와 임시 페이지 테이블이 있다. 물리 주소를 다루는 곳(AHCI DMA, ABAR, 페이지 테이블, initrd, 프레임버퍼)은
  `virt_to_phys/phys_to_virt` 로 물리/가상 주소를 구분한다. DMA 는 커널 이미지/HHDM 의 (물리적으로 연속인) 버퍼만 허용한다.
- **페이징**: PML4[256] = HHDM(2MiB, NX), PML4[511] = 커널 이미지(4KiB, W^X, 스택 가드 페이지 없음). 커널 이미지의 HHDM 별칭은 읽기 전용(W^X 우회 방지).
  하위 절반은 사용자 공간이며 `paging_map_page` 로 매핑한다. 부팅 후 낮은 주소 항등 매핑은 없다 (NULL 역참조는 자동으로 #PF).
- **부트** (`kernel/boot/boot.s`): UEFI 용 64비트 `_start`, GRUB 용 32비트 `_start_mb`(Multiboot2 entry address 태그). 둘 다 부트 정보와 메모리 맵을
  커널 안의 버퍼로 복사(UEFI 페이지 테이블을 버리기 전)한 뒤 임시 테이블로 higher-half 에 진입한다. 64비트 CPU 가 아니면 VGA 텍스트로 오류를 표시하고 정지.
  `multiboot2.c` 는 태그(메모리 맵, 프레임버퍼, 모듈=initrd, ACPI RSDP)를 검증하며 NTBLI 로 변환한다.
- **syscall/sysret** (`syscall_entry.s`, `syscall.c`): EFER.SCE/STAR/LSTAR/SFMASK, `swapgs` + 커널 스택 전환(`g_cpu_local`), `int 0x80` 과 같은 `syscall_handle` 로 처리.
  sysret 전 복귀 RIP 이 사용자 영역인지 검사(비정규 RIP sysret 취약점 방지). GDT 를 SYSRET 규칙(유저 코드32 -> 유저 데이터 -> 유저 코드64)으로 재배치.
  시스템 콜 `NxYield(5)`, `NxProcessExit(65)` 추가.
- **테스트**: ring 3 프로그램이 `syscall` 과 `int 0x80` 으로 시스템 콜을 호출하고(성공/NULL/커널 주소 거부/yield/exit), 사용자 모드 #GP 는 해당 프로세스만 종료하고 커널은 계속 동작한다.
  `tests/host/mb2_test.c`: Multiboot2 어댑터의 정상 입력 + 변이 퍼징 2만 회(ASan/UBSan).

### 2차 작업 검증 결과
- `make check` (`-O0`/`-O2`) 통과. `make test-host`: FAT16 회귀 12건 + 퍼징, Multiboot2 어댑터 테스트 통과.
- 부팅 + `SELFTEST=1`(33개 항목, ring 3 포함) 통과: UEFI 직접(q35/AHCI, pc/IDE, VGA 없음, `-cpu max` + 3GB(4GB 초과 영역 포함), `-O0`/`-O2`),
  **GRUB BIOS(SeaBIOS)**, **GRUB UEFI(OVMF)**. GRUB 경로의 프레임버퍼 콘솔(1024x768)도 화면으로 확인.
- 예외/보호(`SELFTEST=2..7`): #DE, NULL 역참조, 스택 오버플로(#DF), `.text` 쓰기, 데이터 페이지 실행(NX), #UD 모두 higher-half 에서도 의도대로 검출.

### 검증하지 못한 것
- 실제 하드웨어(모두 QEMU). AHCI COMRESET 지연, 실제 GRUB/UEFI 환경의 프레임버퍼 태그 다양성(현재 32bpp 직접 색상만 지원).
- SMEP 위반 동작(QEMU `qemu64` 에는 SMEP 없음, `-cpu max` 에서 부팅만 확인). sysret 의 비정규 RIP 방어 경로(`syscall_bad_return`)는 실행해 보지 못했다.
- 32비트 유저(호환 모드) 프로그램 실행, 멀티코어(AP 는 깨우지 않음), 4GB 초과 RAM 은 3GB 게스트까지.

## 7. 남은 문제 / 알려진 한계

- VFS: mount/unmount 외의 경로에 락 없음, dentry/vnode 참조 카운트를 실제로 쓰지 않음 (멀티코어/선점 도입 전 필수).
- 스케줄러는 협력형. 타이머 선점, 실제 유저 프로세스 로더(ELF), 프로세스별 주소 공간(CR3) 없음. syscall/int 0x80 처리 중에는 인터럽트가 꺼져 있다.
- syscall 복귀 직전 사용자 스택 위에서 NMI 가 오면 스택 전환 문제가 생길 수 있다 (NMI 는 IST 를 쓰므로 스택은 안전하나 GS 상태 주의 필요).
- 스택 카나리, KASLR, SMAP 미사용. 프레임버퍼는 WB 매핑(MTRR 에 의존; PAT 로 WC 지정하면 스크롤이 빨라짐).
- Multiboot2 프레임버퍼 태그가 없거나 지원하지 않는 형식이면 시리얼만 사용한다.
- 방안 B(32비트 커널 별도 빌드)와 ELF32 로더/`linker32.lds` 는 아직 없다.
- `userland/initrd/initrd.c` 는 컴파일 불가능한 자리 표시자, 빈 `코드분석.rtf` 는 그대로 둠.
