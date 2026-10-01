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

## 8. 시스템 콜 구현 (3차 작업)

가장 먼저 필요한 것부터 만들었다: 시스템 콜이 사용자 입력을 다루는 코드이므로, "무엇을 만들지" 보다
"안전하게 다루는 통로부터 만드는 것" 을 우선했다. 그 통로(uaccess, 핸들 테이블) 위에 핵심 호출들을 올렸다.

- **공개 ABI 헤더** (`include/nyx_abi.h`): 시스템 콜 번호, 구조체(`nx_stat`/`nx_sysinfo`/`nx_procinfo`), 플래그, 제한값을
  커널 내부 타입과 분리해서 정의한다. 유저랜드는 이 헤더 하나만 포함하면 된다. 구조체 크기는 `NX_STATIC_ASSERT` 로 고정한다.
- **`uaccess.c`/`uaccess_asm.s`**: 사용자 포인터를 역참조하는 유일한 통로. `paging_is_user_range()` 로 먼저 검사하고,
  실제 복사는 `#PF` 를 복구할 수 있는 어셈블리 루틴(`nx_uaccess_copy`) 하나로만 한다. 검사와 실제 접근 사이에 다른
  실행 흐름이 매핑을 바꾸는 경쟁(TOCTOU)이 있어도, 그 순간의 `#PF` 는 커널을 패닉시키지 않고 시스템 콜 오류로 바뀐다.
  `interrupt.c` 의 `#PF` 핸들러가 `uaccess_fixup()` 을 먼저 확인하고, 그 복사 명령에서 난 커널 모드 폴트가 아니면
  (즉 진짜 커널 버그이면) 그대로 패닉한다.
- **`handles.c`**: 프로세스별 capability 방식 핸들 테이블. 핸들 값 = `(세대 << 32) | 슬롯`. 각 핸들은 권한
  (`NX_RIGHT_READ/WRITE/SEEK/STAT/DUP`)을 가지고, `NxDuplicateHandle` 은 `원본 권한 & 요청 마스크` 로만 계산되므로
  ALL 을 요청해도 권한을 넓힐 수 없다. 닫힌 슬롯은 세대가 올라가서 오래된 핸들 값이 재사용을 가리키지 못한다.
  프로세스가 종료되면 `process_terminate()` 가 남은 핸들을 전부 닫는다 (열린 파일이 새지 않는다).
- **구현한 시스템 콜** (표 기반 디스패치, `syscall.c` 의 `g_syscalls`): `NxGetVersion`, `NxGetTime`, `NxSleep`, `NxYield`,
  `NxSysInfo`, `NxOpen`/`NxClose`/`NxRead`/`NxWrite`/`NxSeek`/`NxStat`/`NxDuplicateHandle`, `NxProcessExit`, `NxProcessInfo`,
  `NxKernelPrint`(이번에 유저 포인터 검증 경로로 다시 연결), `NxDebugNop`. 정확한 번호/시그니처는 `syscalls.txt` 참고.
- **VFS/드라이버 보강**: `vfs_fstat()`(핸들 기준 stat, 경로 재해석이 없어 TOCTOU 없음), `vfs_reopen()`(핸들 복제 시
  파일시스템 내부 상태를 공유하지 않도록 새로 연다), FAT16 에 `stat` 연산 추가.
- **프로세스**: `wake_tick` 기반 `process_sleep_ticks()` (스케줄러가 시간이 되면 깨움), 종료 코드(`exit_code`), 핸들 테이블을
  `process_t` 에 내장. 스케줄러 정책 함수(`sched_rr.h`)가 잠든 프로세스를 깨우는 조건도 함께 처리한다 (원칙: `schedule()`
  하나만 외부에 공개, 정책은 인라인).
- **printk_write()**: 사용자 프로그램의 콘솔 출력 전용 경로. 서식 문자열이 아니며, 출력 가능한 ASCII/`\n`/`\r`/`\t` 외의
  바이트(특히 ESC 등 제어 문자)는 `?` 로 바꿔서 시리얼 터미널로의 이스케이프 시퀀스 주입을 막는다.
- **자체 점검 확장** (`selftest_user.s`): ring 3 프로그램이 구현한 시스템 콜 전부를 `syscall` 명령과 `int 0x80` 양쪽으로
  호출한다 — 파일 열기/읽기/탐색(seek)/상태(stat)/닫기, 핸들 복제와 권한-축소만 가능함(ALL 마스크로 복제해도 쓰기 권한이
  생기지 않음을 확인), 이미 닫은 핸들 재사용 거부, stdout 쓰기와 그 복제본 쓰기, 프로세스 정보 조회, 버전/시간/수면/시스템 정보.
  총 67개 점검(functional 33개 + ring3 34개)이 모두 통과한다.

### 3차 작업 검증 결과
- `make check` (`-O0`/`-O2`), `make test-host`(FAT16 + Multiboot2 어댑터) 통과.
- `SELFTEST=1`: q35/AHCI, pc/IDE, VGA 없음, `-O0`, GRUB BIOS, GRUB UEFI 에서 부팅 + 67개 점검 전부 통과 (실패 0건).
- `SELFTEST=2..7`(#DE/#PF/#DF/WP/NX/#UD)과 기본(SELFTEST 없는) 릴리스 빌드 부팅도 이번 변경 이후 다시 확인했다.

### 검증하지 못한 것
- 실제 하드웨어. 동시에 여러 프로세스가 같은 파일을 여는 경쟁(현재 VFS 락이 얕아서 이론상 취약, 7절 참고), `NX_MAX_HANDLES`(32개)
  소진 시의 동작(코드는 `NoutOfMemory` 를 반환하도록 되어 있으나 점검에서 실제로 채워보지는 않았다).
- 콘솔 입력(`NxRead(stdin, ...)`): 키보드 드라이버가 아직 인터럽트 기반이 아니라서 `NxRead` 가 콘솔 핸들에는 `Nunsupported` 를
  반환한다. `sys_read()` 에 그 분기를 표시해 뒀다.

### 다음에 만들 것 (원칙: 우선순위 높은 것부터)
1. `NxProcessCreate`/`NxProcessWait` — 지금은 커널 스레드만 있고 실제 사용자 프로세스를 새로 만들 방법이 없다.
2. ELF 유저 프로세스 로더 (7절의 "실제 유저 프로세스 로더 없음"과 연결). 이게 있어야 `helloworld` 를 커널에 링크해 두는
   임시 구조를 없앨 수 있다.
3. `NxVirtualAlloc`/`NxVirtualFree` — 지금 유저 프로세스는 커널이 미리 매핑해 준 고정 페이지만 쓸 수 있다.
4. 콘솔 입력(키보드 인터럽트 + 대기 중인 프로세스를 깨우는 큐) — 이게 있어야 `NxRead(stdin, ...)` 이 실제로 동작한다.

## 9. ELF 로더 / NxProcessCreate / 프로세스별 주소 공간 / 힙 (4차 작업)

"C89 libc 를 위해 필요한 커널 기능 요청서"(사용자가 제공)의 1순위·2순위 항목을 구현했다. 요청서는
crt0 이 실제로 호출되려면(=진짜 유저 프로세스가 있어야) 필요한 것부터 순서를 매겨 두었으므로 그 순서를 그대로 따랐다.

### 9.1 물리 프레임 할당자 (`kernel/mm/pfa.c`) — 1-2/2-1 의 공통 기반

프로세스별 주소 공간을 만들고 없애려면(1-2), 그리고 힙을 늘리고 줄이려면(2-1) 물리 페이지를 동적으로
할당/회수할 방법이 있어야 하는데, 기존 코드에는 그게 없었다 — `paging.c` 의 페이지 테이블은 부팅 시
한 번 채워지는 정적 배열(`g_pool`, 320페이지)에서 나왔고 **회수(free)가 아예 없었다**. 이 배열을 계속
쓰면 프로세스 생성/종료를 몇 번만 반복해도 고갈된다.

- 자유 페이지 자신의 첫 8바이트에 "다음 자유 페이지의 물리 주소"를 저장하는 연결 리스트 방식이라
  RAM 크기에 비례하는 별도 메타데이터가 필요 없다.
- **보안**: `pfa_alloc()` 이 내주는 모든 프레임은 항상 0 으로 지운 뒤 반환한다. 그러지 않으면 어떤
  프로세스가 반납한(종료된) 힙/스택 내용을 다음에 그 프레임을 받는 **다른** 프로세스가 그대로 읽어낼
  수 있다 — 격리가 있는 척하면서 실제로는 정보가 새는 구멍이 된다.
- **부트로더 신뢰 문제**: Multiboot2(GRUB, E820 기반)는 커널/initrd 가 이미 올라가 있는 물리 영역도
  "사용 가능"으로 보고할 수 있다(UEFI 처럼 로더가 자기 메모리를 별도 타입으로 표시해 주지 않는다).
  그래서 `pfa_init()` 은 메모리 타입과 무관하게 커널 이미지·initrd·저 1MiB 를 **항상** 물리 주소로
  직접 제외한다. (양쪽 부트 경로 모두에서 실기로 확인 — 9.4절)

### 9.2 프로세스별 독립 주소 공간 (`paging.h`/`paging.c`, `process.h`/`process.c`) — 1-2

`process_t.cr3` 필드는 있었지만 아무도 채우지 않았고(주석에 "미지원(추후)"라고 명시되어 있었다), 시스템
전체가 부팅 시 만들어진 **단 하나의** 주소 공간만 썼다.

- `addr_space_t`(자기만의 PML4)를 도입했다. 상위 절반(HHDM + 커널 이미지, PML4 256/511번)은 모든 주소
  공간이 **공유**하고(그래야 인터럽트/시스템 콜이 어떤 프로세스의 CR3 가 실려 있든 커널 코드를 실행할
  수 있다), 하위 절반(사용자 공간)만 주소 공간마다 완전히 독립적이다.
- `paging_addrspace_create()` / `paging_addrspace_destroy()`: 후자는 사용자 영역을 4단계 전부 순회하며
  데이터 프레임과 중간 페이지 테이블을 남김없이 `pfa_free()` 한다 — "1-2. 프로세스 종료 시 페이지테이블
  회수" 요구사항 그 자체다.
- `paging_map_page`/`paging_unmap_page`/`paging_is_user_range` 는 이제 `addr_space_t*` 를 명시적으로
  받는다(기존에는 암묵적으로 전역 하나만 다뤘다). **버그를 하나 여기서 직접 잡았다**: 기존
  `table_of()`(페이지 테이블 항목 -> 커널 가상 주소 변환)는 `KERNEL_VMA_BASE` 오프셋을 썼는데, 이는
  "커널 이미지 안의" 정적 풀에서 할당된 테이블에만 맞는 변환이다. 프로세스별 테이블은 PFA(=HHDM 범위의
  임의 RAM)에서 나오므로 **HHDM 오프셋**으로 변환해야 한다 — 섞어 쓰면 엉뚱한 커널 가상 주소를 페이지
  테이블로 착각해서 읽고 쓰는, 조용한 메모리 손상이 된다. 그래서 정적 풀 경로(`alloc_table`/`table_of`,
  부팅 시 HHDM/커널 이미지 구성 전용)와 동적/PFA 경로(`alloc_table_dyn`/`table_of_dyn`, 그 이후 모든
  프로세스별 매핑 전용)를 명확히 분리했다.
- `process_switch()` 는 이제 **항상** CR3 를 다시 로드한다(전용 주소 공간이 있으면 그것, 없으면 공유
  커널 주소 공간). 예전에는 `if (proc->cr3) write_cr3(...)` 로 "커널 스레드면 그냥 이전 CR3 를 남겨
  둔다"였는데, `uaccess`/`paging_is_user_range` 는 항상 `current_process->addrspace` 라는 **소프트웨어
  상태**를 검사하므로, 실제 CR3(하드웨어)가 그와 다른 채로 남아 있으면 검사와 실제 메모리 접근이 서로
  다른 주소 공간을 보는 사고(그리고 우연히 이전 프로세스의 남은 매핑을 새 프로세스인 척 접근하는 보안
  구멍)가 날 수 있었다.
- `process_create_user()` 는 새 프로세스의 모든 필드(주소 공간, ring3 진입점, 스택, 힙 범위)를
  **스케줄러 목록에 연결하기 전에** 채운다 — 그러지 않으면 인터럽트가 그 사이에 끼어들어 절반만
  초기화된 프로세스를 스케줄러가 골라 실행하는 경쟁이 생긴다(부모의 호출 스택에 있는 포인터를 넘기는
  방식도 검토했지만, 그 스택은 자식이 처음 스케줄되기 전에 재사용될 수 있어 더 위험해서 기각했다 —
  대신 `process_t` 자신에 필드를 두고 `current_process` 를 통해서만 읽는다).

### 9.3 ELF64 로더 (`kernel/process/elf.c`) — 1-1

부트로더(`bootloader/nytb/nytb3.c`)의 커널 ELF 로더와 같은 원칙(파일의 모든 값을 검증 전까지 공격자
통제 입력으로 취급)을 사용자 프로세스에도 적용했다. 커널에 아직 범용 힙이 없으므로 파일을 통째로
올리지 않고, VFS 핸들에서 세그먼트별로 **페이지 단위로 스트리밍**하며 바로 목적지 페이지에 쓴다.

- 제약(문서화된 한계): ET_EXEC(정적, no-PIE) 만 지원, `p_vaddr` 는 4KiB 정렬 필수(대부분의 표준
  링커 출력이 만족), W^X 는 `paging_map_page` 가 강제.
- 실패 시 정리 정책을 단순하게 통일했다: `elf_load()` 자신은 부분적으로 매핑한 것을 되돌리려 하지
  않는다(처음에는 되돌리려는 헬퍼를 만들었다가, "매핑 해제는 했지만 `pfa_free` 는 하지 않은" 프레임이
  생기는 더 위험한 상태를 만든다는 것을 깨닫고 제거했다). 실패하면 항상 호출자(`spawn.c`)가 주소 공간
  전체를 `paging_addrspace_destroy()` 로 버리므로, 정리는 그 한 곳에서만 정확하게 일어난다.

### 9.4 스택 규약 (1-3) / `NxProcessCreate` (`kernel/process/spawn.c`, syscall 64)

- 크기 **64KiB 고정, 자동 확장 없음** (요청서가 제시한 두 선택지 중 결정). 바로 아래 4KiB 는 매핑하지
  않는 가드 페이지라 오버플로는 즉시 `#PF` 로 드러나고 그 프로세스만 종료된다.
- 스택은 항상 `RW+NX`(`PAGE_NX` 는 이미 `paging.h` 에 있었다 — 요청서가 "이미 준비된 것"으로 지목한
  그대로 재사용). 진입 시 RSP 는 System V AMD64 초기 배치(`argc=0, argv=[NULL], envp=[NULL],
  auxv=[AT_NULL]`)를 가리킨다 — v1 은 `NxProcessCreate(const char *path)` 로 경로 하나만 받으므로
  argv/envp 전달은 아직 없다(그 값들이 전부 0 인 것은 우연이 아니라 PFA 가 항상 0 으로 지운 새 페이지를
  주기 때문이며, 나중에 argv/envp 를 채울 자리로 이 레이아웃을 그대로 쓸 수 있다).
- `NxProcessCreate` -> `process_spawn()`: 파일 열기 -> 주소 공간 생성 -> ELF 적재 -> 스택 매핑 ->
  `process_create_user()` 순서이며, 어느 단계에서 실패하든 그때까지 만든 것을 전부 정리한다.

### 9.5 힙(brk) — `NxVirtualAlloc` (`kernel/process/umem.c`, syscall 96) — 2-1

요청서의 "sbrk 스타일 하나만이라도" 를 따라 **부호 있는 증가분**(양수=늘리기, 음수=줄이기, 0=조회)
방식으로 구현했다. `NxVirtualFree`(97번)는 향후 mmap 스타일 API(주소/길이 직접 지정, 별도 보호 속성)를
위해 번호만 예약해 두고 v1 에서는 구현하지 않는다 — 지금은 존재하지 않는 별개 호출이 아니라, 이미
있는 호출에 음수를 넘기는 것 자체가 "해제"이기 때문이다.

- 늘릴 때는 새로 (항상 0 으로 채워진) 프레임을 매핑하고, 줄일 때는 새 break 를 완전히 넘어선 페이지만
  통째로 언맵+회수한다(부분적으로 걸친 페이지는 남겨 둔다 — 그 안의 아직 유효한 바이트를 건드리지
  않기 위해).
- **DoS 방지**: 한 번의 호출 상한(256MiB)과 프로세스당 누적 상한(512MiB)을 뒀다 — 유한한 PFA 를
  프로세스 하나가 한 번에, 또는 반복 호출로 고갈시키지 못하게 하는 최소한의 장치다(`nyx_abi.h` 의
  `NX_HEAP_GROW_MAX_PER_CALL`/`NX_HEAP_MAX_TOTAL`).

### 9.6 나머지 결정 사항 (요청서 2-2 ~ 2-5)

| 항목 | 결정 |
|---|---|
| 2-2 errno 매핑 | **nlibc 가 소유**한다(커널 ABI 에는 넣지 않는다). 이유: Nstatus 는 이 프로젝트가 이미 "매력적인 기능"으로 명시한 커널 고유 개념이고, libc 프로파일마다 다른 매핑을 쓸 수도 있으며, Nstatus 를 직접 쓰는 프로그램은 이 매핑이 아예 필요 없다. `userland/nlibc/library/errno.h` 에 C89 필수(EDOM/ERANGE) + 흔한 POSIX 이름 + `nx_errno_from_status()` 변환 함수로 구현해 두었다(닫힌 표라 nlibc 가 실제로 만들어지기 전에도 안전하게 먼저 정할 수 있는 부분이다). |
| 2-3 벽시계(`time()`) | **포기**(요청서가 제시한 선택지 중 하나). `NX_CLOCK_REALTIME`(1) 번호만 예약해 두고, `NxGetTime` 은 그 clock_id 에 `Nunsupported` 를 반환한다. RTC 드라이버가 생기기 전까지 libc 의 `time()`/`gettimeofday()` 는 이 오류를 `(time_t)-1` 로 옮겨야 한다 — 없는 값을 지어내지 않는 것이 "모른다"를 정직하게 표현하는 유일한 방법이다. |
| 2-4 `abort()` 종료 코드 | 새 시스템 콜 없이 기존 `NxProcessExit(exit_code)` 로 충분하다. `abort()` -> `NxProcessExit(134)`(128+SIGABRT, 쉘의 `$?` 관례와 일치)를 nlibc 쪽 관례로 정했다(문서화만, 아직 nlibc 코드는 없다). |
| 2-5 stdin/stdout/stderr | 이미 정확했다: `nx_handles_init()` 이 모든 새 프로세스(커널 스레드든 `process_create_user` 로 만든 사용자 프로세스든)에 콘솔에 새로 연결된 자기 자신의 0/1/2 핸들을 준다. "부모로부터 상속" 은 아니지만 콘솔이 시스템에 하나뿐인 지금은 사실상 동등하고, `printf` 가 갈 곳이 확실히 있다. |

### 9.7 검증 (`userland/elftest/`)

기존에 커널 이미지에 직접 링크되어 있던 `userland/helloworld/`(문서에 "가능한 빨리 제거 예정"이라고
적혀 있었다)를 삭제하고, **진짜 별도로 링크되는 ET_EXEC ELF64**(`userland/elftest/`, 자체 링커
스크립트로 `.text`/`.rodata`/`.data+.bss` 를 각각 페이지 정렬된 세그먼트로 분리)를 initrd 에 넣어
`app:/elftest.run` 으로 만들었다. 이 프로그램 자신이 다음을 내부적으로 검사하고 종료 코드로 결과를
알린다(42 = 전부 통과): 초기 스택 배치(`argc=0, argv[0]=NULL`), `.data` 초기값/`.bss` 0 초기화 후
쓰기, `.rodata` 읽기, 20KiB 지역 배열(여러 페이지에 걸침), 파일 열기/읽기/닫기, 힙 조회·확장(항상
0으로 채워짐 확인)·쓰기·축소(남은 부분 보존 확인)·재확장(다시 0으로 채워짐 확인 — **재사용된 프레임이
이전 내용을 새어 보내지 않는지**의 직접 증거), 그리고 잘못된 `NxVirtualAlloc` 요청(힙 시작 아래로
축소, 한 번에 과도한 요청)이 거부되고 `brk` 가 그대로인지까지.

커널 쪽 자체 점검(`spawn_tests()`)은 이 프로그램을 `process_spawn()` 으로 직접 실행해 종료 코드를
확인하고, **존재하지 않는 파일 / ELF 가 아닌 파일**(`app:/hellowld.run`, 평문 텍스트)에 대한
`NxProcessCreate` 가 프레임을 하나도 새어나가게 하지 않고 거부되는지, 그리고 무엇보다 —
**정상적으로 실행되고 끝난 프로세스가 `process_reap_terminated()` 이후 물리 프레임을 정확히 시작
개수만큼 돌려주는지**(`pfa_free_count()` 로 전/후 비교)를 확인한다. 이것이 "1-2. 프로세스 종료 시
페이지테이블 회수" 요구사항의 가장 직접적인 증거다. 기존 flat-바이너리 ring3 테스트도 이제 진짜
`addr_space_t` + PFA 프레임을 쓰도록 고쳤고(정적 `.bss` 배열을 매핑하면 `paging_addrspace_destroy()`
가 그 프레임을 실수로 "회수"해 커널 데이터를 오염시킨다), ring3 코드에서 `NxProcessCreate` 를 존재하지
않는 경로로 호출해 시스템 콜 경유 거부까지 확인한다.

**부팅 검증**: `SELFTEST=1` 이 UEFI 직접(q35+AHCI, pc+IDE, VGA 없음, `-cpu max`+3GB, `-O0`), **GRUB
BIOS**, **GRUB UEFI** 모두에서 79개 점검(기존 67개 + 새 12개) 전부 통과. `SELFTEST=2..7`(예외 검출),
`make check`(`-O0`/`-O2`), `make test-host`(FAT16 + Multiboot2 어댑터 퍼징) 모두 이번 변경 이후
재확인했다. 클린 리빌드(`make clean && make -B os`)로도 처음부터 다시 확인했다.

### 검증하지 못한 것 / 남은 문제

- 실제 하드웨어(전부 QEMU). 여러 사용자 프로세스가 동시에 각자의 ELF 를 적재하는 상황(현재도 격리는
  되지만, 동시 부하 상황에서의 PFA 락 경합은 부하 테스트를 하지 않았다).
- `NX_HEAP_MAX_TOTAL`/`NX_HEAP_GROW_MAX_PER_CALL` 상한에 정확히 부딪히는 경계값, 그리고 물리 프레임이
  실제로 고갈되는 상황(`pfa_alloc()` 이 `NoutOfMemory` 를 반환하는 경로 자체는 코드로 존재하지만
  실기로 RAM을 다 채워 확인하지는 않았다).
- `NX_USER_ELF_MAX_VADDR`/스택 고정 주소(`NX_USER_STACK_TOP`)를 벗어나는 극단적인 ELF(매우 큰 가상
  주소를 요구하는 세그먼트)는 거부되는 코드는 있지만 그런 파일로 실기 검증하지는 않았다.

### 다음에 만들 것 (요청서 3순위 + 이번에 새로 드러난 것)

1. `NxProcessWait`/`NxProcessKill`(요청서 3순위) — `process_get_exit_code()` 를 이미 만들어 뒀으므로
   그 위에 얇게 얹으면 된다.
2. 콘솔 입력(키보드 인터럽트 + 대기 중인 프로세스를 깨우는 큐) — 이게 있어야 `NxRead(stdin, ...)` 이
   `Nunsupported` 대신 실제로 동작한다(요청서 2-5 연장선).
3. `argv`/`envp` 전달 — 스택 레이아웃은 이미 이를 염두에 두고 설계했으므로(현재 필드들이 우연이 아니라
   전부 0인 이유), `NxProcessCreate` 의 인자를 늘리는 형태로 확장 가능하다.
4. `NxProtect`(요청서가 나중이어도 된다고 명시)와 진짜 mmap 스타일 `NxVirtualAlloc`/`NxVirtualFree`.
