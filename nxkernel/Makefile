# =====================================================================
# Nyxis OS 빌드 시스템
#
#   make            부트로더 + 커널 빌드            (out/BOOTX64.EFI, out/kernel.elf)
#   make os         부팅 가능한 ESP 디스크 이미지    (out/esp.img)
#   make run        QEMU(OVMF, q35+AHCI) 로 실행     (GTK 창 + 시리얼 stdio)
#   make run-nographic  화면 없이 시리얼만으로 실행
#   make iso        GRUB(Multiboot2) 부팅 ISO (BIOS/UEFI 겸용)   (out/nyxis.iso)
#   make run-grub-bios / run-grub-uefi   GRUB 으로 부팅
#   make run-usb    usb-storage 부팅 디스크 + std VGA 로 실행 (예전 virtio-gpu 는 BltOnly GOP 라 프레임버퍼가 없어 제거)
#   make check      C89 엄격 컴파일(-O0/-O2) 검사
#   make test-host  FAT16/VFS/램디스크 호스트 테스트 (회귀 + ASan/UBSan 변이 퍼징)
#   make nlibc-sdk  nlibc SDK (out/nlibc: nlcc/nlld 드라이버, 헤더, libnlibc.a, 링커 스크립트)
#   make test-nlibc nlibc(C 라이브러리) 호스트 테스트 (Linux 정적 바이너리 + glibc 와의 부동소수점 차분 퍼징)
#   make test-e2e   종단 간 검사 (SELFTEST 이미지 필요: 부팅 -> 로그인 -> 셸 -> 전원 끄기)
#   make test-disk  disk:/ 영구 저장 검사 (SELFTEST 이미지 필요: 쓰고 -> 재부팅 -> 그대로인지)
#   make disk       disk:/ 용 영구 디스크 이미지 (disk.img, 없을 때만 만든다. run 이 SATA 디스크로 붙인다)
#   make disk-reset disk.img 를 지우고 새로 만든다 (안의 파일이 모두 사라진다)
#   make users      로그인 계정 만들기 (config/users.db, 비밀번호는 터미널에서 묻는다). 일반 이미지 빌드에 필요
#   make clean
#
#   옵션:  DEBUG=1     부트로더 프레임버퍼 테스트 패턴 활성화 (-DNYTB_DEBUG)
#          SELFTEST=1  커널 부팅 후 자체 점검(kernel/selftest.c) 실행 (-DNYX_SELFTEST)
#
# [수정 이력 요약]
#  - 커널 컴파일 옵션 정비 (예전에는 -std=gnu11 + 경고 옵션 없음):
#      -std=c89 -pedantic -Wall -Wextra -Werror     : C89 및 경고=오류 (프로젝트 규칙)
#      -mno-red-zone                                : 인터럽트가 스택 아래(레드존)를 덮어쓰는 것 방지 (필수)
#      -mgeneral-regs-only                          : 커널에서 SSE/x87 레지스터 사용 금지
#                                                     (인터럽트 핸들러가 XMM 을 저장하지 않으므로)
#      -fno-pie -fno-pic -fcf-protection=none       : 고정 주소에 링크되는 정적 커널
#      -mcmodel=kernel                              : higher-half(0xFFFFFFFF80000000~) 링크 주소에 필요한 코드 모델
#      -fno-tree-loop-distribute-patterns           : memset/memcpy 구현이 자기 자신을 호출하는 재귀 방지
#      -fno-stack-protector -fno-builtin -nostdinc -ffreestanding
#  - 사용하지 않던 kernel/timer, pic, ata, inputs, graphics 도 빌드에 포함 (컴파일 검증).
#  - 규칙 반복(복붙 20여 개)을 패턴 규칙으로 통합, 헤더 의존성(-MMD) 추적.
#  - initrd.img 를 저장소의 추적 파일에 덮어쓰지 않고 out/ 에 생성.
#  - 링커: 4KiB 정렬 세그먼트(-z max-page-size=0x1000), NX 스택 표시.
#  - 부트로더를 gnu-efi 4.x 에서도 빌드 가능하게 (docs/AUDIT.md 10절):
#      objcopy 에 -j .rodata 추가 (4.x 링커 스크립트는 .rodata 를 별도 섹션으로 만든다. 빠지면 문자열/GUID 소실)
#      -fno-tree-loop-distribute-patterns : nytb3.c 의 자체 메모리 루프가 memcpy/memset 호출로 바뀌지 않도록
# =====================================================================

CC      = gcc
AS      = as
LD      = ld
OBJCOPY = objcopy

ROOT        = ..
INCLUDE_DIR = $(ROOT)/include
BUILD       = $(ROOT)/out
OPT        ?= -O2

# =========================
# EFI (64-bit UEFI)
# =========================
EFIINC  = /usr/include/efi
EFIARCH = /usr/include/efi/x86_64

EFILIB  = /usr/lib
CRT0    = /usr/lib/crt0-efi-x86_64.o

OVMF      = /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS = /usr/share/OVMF/OVMF_VARS_4M.fd

# =========================
# Bootloader Flags
#   gnu-efi 헤더는 '//' 주석과 함수 포인터->void* 변환(uefi_call_wrapper)을 쓰므로
#   -isystem 으로 포함하고 -pedantic 은 사용하지 않는다. (자체 코드는 make check 가 '//' 사용을 검사)
# =========================
CFLAGS = \
	-std=gnu89 \
	-Wall -Wextra -Werror \
	-Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wdeclaration-after-statement -Wvla \
	$(OPT) \
	-I$(INCLUDE_DIR) \
	-isystem $(EFIINC) \
	-isystem $(EFIARCH) \
	-ffreestanding \
	-fshort-wchar \
	-fpic \
	-mno-red-zone \
	-fno-stack-protector \
	-fno-builtin \
	-fno-common \
	-fno-tree-loop-distribute-patterns \
	-MMD -MP

ifeq ($(DEBUG),1)
CFLAGS += -DNYTB_DEBUG
endif

# =========================
# Kernel (64-bit)
# =========================
KERNEL_CFLAGS = \
	-std=c89 -pedantic \
	-Wall -Wextra -Werror \
	-Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wdeclaration-after-statement -Wvla \
	$(OPT) \
	-m64 \
	-ffreestanding -nostdinc \
	-fno-builtin -fno-stack-protector \
	-fno-pie -fno-pic -fcf-protection=none \
	-mcmodel=kernel \
	-fno-asynchronous-unwind-tables \
	-mno-red-zone -mgeneral-regs-only \
	-fno-tree-loop-distribute-patterns \
	-I. -I$(INCLUDE_DIR) -I$(ROOT) \
	-MMD -MP

ifneq ($(SELFTEST),)
KERNEL_CFLAGS += -DNYX_SELFTEST=$(SELFTEST)
endif

ASFLAGS = --64

# =========================
# EFI Linker Flags
# =========================
EFI_LDFLAGS = \
	-nostdlib \
	-znocombreloc \
	-T $(EFILIB)/elf_x86_64_efi.lds \
	-shared \
	-Bsymbolic \
	-L $(EFILIB) \
	-lgnuefi \
	-lefi

# =========================
# Kernel Linker Flags
# =========================
KERNEL_LDFLAGS = \
	-m elf_x86_64 \
	-nostdlib \
	-z max-page-size=0x1000 \
	-z noexecstack \
	--no-warn-rwx-segments \
	-T kernel/linker64.lds

# =========================
# Sources
# =========================
BOOTLOADER_SRC = $(ROOT)/bootloader/nytb/nytb3.c
BOOTLOADER_OBJ = $(BUILD)/nytb3.o
BOOTLOADER_SO  = $(BUILD)/BOOTX64.so
BOOTLOADER     = $(BUILD)/BOOTX64.EFI

KERNEL = $(BUILD)/kernel.elf

# C 소스 (nxkernel 기준 상대 경로)
KERNEL_C_SRCS = \
	kernel/kernel.c \
	kernel/boot/multiboot2.c \
	kernel/error_handling/panic.c \
	kernel/interrupt/interrupt.c \
	kernel/interrupt/gdt.c \
	kernel/paging/paging.c \
	kernel/process/process.c \
	kernel/process/elf.c \
	kernel/process/umem.c \
	kernel/process/spawn.c \
	kernel/mm/pfa.c \
	kernel/mm/kheap.c \
	kernel/crypto/sha256.c \
	kernel/security/auth.c \
	kernel/syscall/nspath.c \
	kernel/info/info.c \
	kernel/ipc/channel.c \
	kernel/dev/device.c \
	kernel/acpi/acpi.c \
	kernel/process/waitq.c \
	kernel/process/signal.c \
	kernel/process/schedule.c \
	kernel/syscall/syscall.c \
	kernel/syscall/uaccess.c \
	kernel/syscall/handles.c \
	kernel/timer/pit/pit_base.c \
	kernel/timer/rtc/rtc.c \
	kernel/exec/exec.c \
	kernel/ipc/pipe.c \
	kernel/security/perm.c \
	kernel/syscall/syscall_ext.c \
	lib/memory.c \
	lib/string.c \
	console/outputs/printk.c \
	console/outputs/conout.c \
	console/outputs/ansi.c \
	console/outputs/outputs_base.c \
	console/outputs/serial.c \
	console/inputs/inputs_base.c \
	console/inputs/tty.c \
	graphics/graphics_base.c \
	$(ROOT)/drivers/ahci/ahci.c \
	$(ROOT)/drivers/ata/ata.c \
	$(ROOT)/drivers/pic/pic.c \
	$(ROOT)/drivers/ramdisk/ramdisk.c \
	$(ROOT)/drivers/filesystem/core.c \
	$(ROOT)/drivers/filesystem/vfs.c \
	$(ROOT)/drivers/filesystem/fat16/fat16.c \
	$(ROOT)/drivers/filesystem/nyfs/nyfs.c

ifneq ($(SELFTEST),)
KERNEL_C_SRCS += kernel/selftest.c
endif

# 어셈블리 소스
KERNEL_S_SRCS = \
	kernel/boot/boot.s \
	kernel/interrupt/isr_stubs.s \
	kernel/process/switch.s \
	kernel/syscall/syscall_entry.s \
	kernel/syscall/uaccess_asm.s \
	kernel/exec/fork_return.s

ifneq ($(SELFTEST),)
KERNEL_S_SRCS += kernel/selftest_user.s
endif

# 콘솔 폰트
#   CONSOLE_FONT=8x16  (기본) 내장 TrueType 폰트(console/fonts/Hack-Regular.ttf)를 8x16 셀로 래스터화 -> CONFIG_FONT_8x16
#   CONSOLE_FONT=16x16 예전 16x16 비트맵 폰트만 사용
#   TTF 초기화가 실패하면 실행 중에도 16x16 비트맵으로 되돌아간다.
CONSOLE_FONT ?= 8x16
FONT_TTF     = console/fonts/Hack-Regular.ttf
ifeq ($(CONSOLE_FONT),8x16)
KERNEL_CFLAGS += -DCONFIG_FONT_8x16
KERNEL_C_SRCS += console/fonts/ttf.c
KERNEL_S_SRCS += console/fonts/font_blob.s
else ifneq ($(CONSOLE_FONT),16x16)
$(error CONSOLE_FONT 는 8x16 또는 16x16 이어야 합니다 (현재: $(CONSOLE_FONT)))
endif

# 소스 경로 -> 오브젝트 경로 ("../" 접두사 제거하여 out/obj 아래에 배치)
obj_of = $(BUILD)/obj/$(patsubst ../%,%,$(1))

KERNEL_OBJS = \
	$(foreach s,$(KERNEL_S_SRCS),$(call obj_of,$(s:.s=.o))) \
	$(foreach s,$(KERNEL_C_SRCS),$(call obj_of,$(s:.c=.o)))

# =========================
# Targets
# =========================
all: $(BOOTLOADER) $(KERNEL)

kernel: $(KERNEL)

# Bootloader
$(BOOTLOADER_OBJ): $(BOOTLOADER_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOTLOADER_SO): $(BOOTLOADER_OBJ)
	$(LD) \
		$(CRT0) \
		$(BOOTLOADER_OBJ) \
		$(EFI_LDFLAGS) \
		-o $@

$(BOOTLOADER): $(BOOTLOADER_SO)
	$(OBJCOPY) \
		-j .text \
		-j .sdata \
		-j .data \
		-j .rodata \
		-j .dynamic \
		-j .dynsym \
		-j .rel \
		-j .rela \
		-j .rel.* \
		-j .rela.* \
		-j .reloc \
		--output-target=efi-app-x86_64 \
		$< \
		$@

# Kernel: C
$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# ../ 로 시작하는 소스는 위 패턴과 경로가 달라 명시 규칙이 필요하다
$(BUILD)/obj/drivers/%.o: $(ROOT)/drivers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# Kernel: assembly
$(BUILD)/obj/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# .incbin 으로 넣는 폰트 파일이 바뀌면 다시 어셈블
$(BUILD)/obj/console/fonts/font_blob.o: $(FONT_TTF)

# Kernel link
$(KERNEL): $(KERNEL_OBJS) kernel/linker64.lds
	$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null) $(BUILD)/nytb3.d

# =========================
# Disk images
# =========================
# =========================
# 사용자 프로그램 (커널과 별개로 링크되는 정적 ELF64 ET_EXEC, initrd 에 넣어 NxProcessCreate 로 실행)
#   유저 코드도 프로젝트 규칙(C89, -Werror)을 따른다. 커널 전용 옵션(-mcmodel=kernel 등)은 쓰지 않는다.
# =========================
USER_CFLAGS = \
	-std=c89 -pedantic -Wall -Wextra -Werror \
	-Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wdeclaration-after-statement -Wvla \
	$(OPT) -m64 -ffreestanding -nostdinc -fno-builtin -fno-stack-protector \
	-fno-pie -fno-pic -fcf-protection=none -fno-asynchronous-unwind-tables \
	-fno-tree-loop-distribute-patterns -mgeneral-regs-only \
	-I$(INCLUDE_DIR) -I$(ROOT)/userland/nlibc/library

ELFTEST_DIR = $(ROOT)/userland/elftest
ULIB_DIR    = $(ROOT)/userland/lib
USER_LDS    = $(ELFTEST_DIR)/linker.lds
USR_DIR     = $(BUILD)/usr
ELFTEST_ELF = $(USR_DIR)/elftest.run
USER_LD     = $(LD) -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments -T $(USER_LDS)

# 공용 런타임 (crt0 + 시스템 콜 래퍼/문자열/콘솔)
ULIB_OBJS = $(BUILD)/obj/ulib/crt0.o $(BUILD)/obj/ulib/nyx.o

$(BUILD)/obj/ulib/nyx.o: $(ULIB_DIR)/nyx.c $(ULIB_DIR)/nyx.h $(INCLUDE_DIR)/nyx_abi.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I$(ULIB_DIR) -c $< -o $@

$(BUILD)/obj/ulib/crt0.o: $(ULIB_DIR)/crt0.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# userland/ 의 프로그램 디렉터리는 자동으로 찾는다 (새 디렉터리를 만들면 그대로 rootfs 에 들어간다):
#   - main.c 가 "nyx.h" 를 포함하면 시스템 프로그램 (userland/lib 의 작은 런타임): USER_PROGS
#   - 그 밖에 *.c 가 있는 디렉터리는 nlibc 앱 (디렉터리의 *.c 를 모두 묶는다): NLIBC_APPS
#   - 자기 규칙이 따로 있는 디렉터리는 뺀다: USERLAND_SPECIAL
USERLAND_SPECIAL = lib nlibc initrd elftest coreutils kilo nlcc
USERLAND_DIRS    = $(filter-out $(USERLAND_SPECIAL),$(sort $(notdir $(patsubst %/,%,$(dir $(wildcard $(ROOT)/userland/*/*.c))))))

# 시스템 프로그램: userland/<이름>/main.c -> fs:/usr/<이름>.run (= app:/<이름>.run)
USER_PROGS     = $(filter $(USERLAND_DIRS),$(notdir $(patsubst %/main.c,%,$(shell grep -l '^\#include "nyx.h"' $(ROOT)/userland/*/main.c 2>/dev/null))))
USER_PROG_ELFS = $(addprefix $(USR_DIR)/,$(addsuffix .run,$(USER_PROGS)))

$(USER_PROG_ELFS): $(USR_DIR)/%.run: $(ROOT)/userland/%/main.c $(ULIB_OBJS) $(ULIB_DIR)/nyx.h $(USER_LDS)
	@mkdir -p $(BUILD)/obj/user $(USR_DIR)
	$(CC) $(USER_CFLAGS) -I$(ULIB_DIR) -c $< -o $(BUILD)/obj/user/$*.o
	$(USER_LD) -o $@ $(ULIB_OBJS) $(BUILD)/obj/user/$*.o

# elftest: 자체 점검용 (자기 start.s 로 진입 레지스터를 검사하고, 공용 런타임도 쓴다)
$(ELFTEST_ELF): $(ELFTEST_DIR)/elftest.c $(ELFTEST_DIR)/systest.c $(ELFTEST_DIR)/start.s $(USER_LDS) $(BUILD)/obj/ulib/nyx.o $(ULIB_DIR)/nyx.h
	@mkdir -p $(BUILD)/obj/elftest $(USR_DIR)
	$(CC) $(USER_CFLAGS) -I$(ULIB_DIR) -c $(ELFTEST_DIR)/elftest.c -o $(BUILD)/obj/elftest/elftest.o
	$(CC) $(USER_CFLAGS) -I$(ULIB_DIR) -c $(ELFTEST_DIR)/systest.c -o $(BUILD)/obj/elftest/systest.o
	$(AS) $(ASFLAGS) $(ELFTEST_DIR)/start.s -o $(BUILD)/obj/elftest/start.o
	$(USER_LD) -o $@ $(BUILD)/obj/elftest/start.o $(BUILD)/obj/elftest/elftest.o $(BUILD)/obj/elftest/systest.o $(BUILD)/obj/ulib/nyx.o

# nlibc: C89 표준 C 라이브러리 (userland/nlibc). 헤더 userland/nlibc/library, 소스 userland/nlibc/src
#   libnlibc.a + nlcrt0.o 로 링크한다. nlibc 헤더가 include/ 의 커널 헤더(string.h, stdint.h ...)를 가리도록
#   -I 순서가 USER_CFLAGS 와 반대다.
NLIBC_DIR    = $(ROOT)/userland/nlibc
#   커널이 프로세스마다 FPU/SSE 상태를 저장하므로(process.h 의 fpu_state) nlibc 프로그램은 부동소수점을 쓴다:
#   -mgeneral-regs-only 를 뺀다.
NLIBC_CFLAGS = $(filter-out -I% -mgeneral-regs-only,$(USER_CFLAGS)) -D_NYX_SOURCE -I$(NLIBC_DIR)/library -I$(INCLUDE_DIR)
NLIBC_MODS   = ctype dirent env fpconv locale malloc math posix_fd posix_path printf process scanf signal start stdio stdlib string termios time
NLIBC_ASM_MODS = setjmp sigtramp
NLIBC_HDRS   = $(wildcard $(NLIBC_DIR)/library/*.h $(NLIBC_DIR)/library/sys/*.h) $(NLIBC_DIR)/src/nl_internal.h $(INCLUDE_DIR)/nyx_abi.h
NLIBC_OBJS   = $(patsubst %,$(BUILD)/obj/nlibc/%.o,$(NLIBC_MODS) $(NLIBC_ASM_MODS) nxsys)
NLIBC_A      = $(BUILD)/obj/nlibc/libnlibc.a
NLIBC_CRT0   = $(BUILD)/obj/nlibc/nlcrt0.o

$(BUILD)/obj/nlibc/%.o: $(NLIBC_DIR)/src/%.c $(NLIBC_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(NLIBC_CFLAGS) -c $< -o $@

$(BUILD)/obj/nlibc/%.o: $(NLIBC_DIR)/src/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(NLIBC_CRT0): $(NLIBC_DIR)/src/crt0.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(NLIBC_A): $(NLIBC_OBJS)
	$(AR) rcs $@ $^

# nlibc SDK: $(BUILD)/nlibc/{bin,include,lib}. 호스트 gcc/ld 로 Nyxis 실행 파일을 만드는 드라이버
#   nlcc (컴파일 + 링크) / nlld (링크만) 와 링커 스크립트 nlibc.ld. 사용 예:
#     out/nlibc/bin/nlcc -O2 -o hello.run hello.c
NLIBC_SDK   = $(BUILD)/nlibc
NLCC        = $(NLIBC_SDK)/bin/nlcc
NLIBC_STAMP = $(NLIBC_SDK)/.stamp

$(NLIBC_STAMP): $(NLIBC_A) $(NLIBC_CRT0) $(NLIBC_HDRS) $(NLIBC_DIR)/nlibc.ld $(NLIBC_DIR)/tools/nlcc
	@mkdir -p $(NLIBC_SDK)/bin $(NLIBC_SDK)/include $(NLIBC_SDK)/lib
	@mkdir -p $(NLIBC_SDK)/include/sys
	cp $(NLIBC_DIR)/library/*.h $(INCLUDE_DIR)/nyx_abi.h $(NLIBC_SDK)/include/
	cp $(NLIBC_DIR)/library/sys/*.h $(NLIBC_SDK)/include/sys/
	cp $(NLIBC_A) $(NLIBC_SDK)/lib/libnlibc.a
	cp $(NLIBC_CRT0) $(NLIBC_SDK)/lib/nlcrt0.o
	cp $(NLIBC_DIR)/nlibc.ld $(NLIBC_SDK)/lib/nlibc.ld
	cp $(NLIBC_DIR)/tools/nlcc $(NLIBC_SDK)/bin/nlcc
	ln -sf nlcc $(NLIBC_SDK)/bin/nlld
	@touch $@

nlibc-sdk: $(NLIBC_STAMP)

# nlibc 로 만드는 프로그램 (SDK 의 nlcc 로 빌드한다: 드라이버 자체의 검사이기도 하다).
#   -fno-builtin: 검사 프로그램이 sin(상수) 같은 호출을 컴파일러가 미리 계산해 버리지 않도록
NLIBC_PROG_CFLAGS = -std=c89 -pedantic -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes \
	-Wshadow -Wdeclaration-after-statement -Wvla $(OPT) -fno-builtin

#   nlibctest: nlibc 검사 (fs:/usr/nlibctest.run, nysh 에서 `nlibctest`. 호스트에서는 make test-nlibc)
#   fputest  : 커널의 프로세스별 FPU/SSE 상태 저장 검사 (fs:/usr/fputest.run)
NLIBC_TEST_ELF = $(USR_DIR)/nlibctest.run
FPUTEST_ELF    = $(USR_DIR)/fputest.run

$(NLIBC_TEST_ELF): $(NLIBC_DIR)/test/nlibc_test.c $(NLIBC_STAMP)
	@mkdir -p $(USR_DIR)
	$(NLCC) $(NLIBC_PROG_CFLAGS) -o $@ $<

$(FPUTEST_ELF): $(NLIBC_DIR)/test/fputest.c $(NLIBC_STAMP)
	@mkdir -p $(USR_DIR)
	$(NLCC) $(NLIBC_PROG_CFLAGS) -o $@ $<

# 예제: 드라이버 기본값(hosted, 내장 함수 허용)으로 빌드한다 (fs:/usr/hello.run)
HELLO_ELF = $(USR_DIR)/hello.run

$(HELLO_ELF): $(NLIBC_DIR)/examples/hello.c $(NLIBC_STAMP)
	@mkdir -p $(USR_DIR)
	$(NLCC) $(OPT) -Wall -Wextra -Werror -o $@ $<

# 명령어 (userland/coreutils): 각각 fs:/usr/<이름>.run (= nysh 에서 이름만으로 실행)
COREUTILS_DIR  = $(ROOT)/userland/coreutils
COREUTILS      = cat echo clear rm touch mkdir rmdir ls cp mv chmod head tail wc pwd sleep true false cmp nyfetch
COREUTILS_ELFS = $(addprefix $(USR_DIR)/,$(addsuffix .run,$(COREUTILS)))

# nlibc 앱 (userland/<이름>/*.c -> fs:/usr/<이름>.run): 셸 nysh, ntui ... (위의 USERLAND_DIRS 참고)
NLIBC_APPS      = $(filter-out $(USER_PROGS),$(USERLAND_DIRS))
NLIBC_APP_ELFS  = $(addprefix $(USR_DIR)/,$(addsuffix .run,$(NLIBC_APPS)))

define NLIBC_APP_RULE
$(USR_DIR)/$(1).run: $(wildcard $(ROOT)/userland/$(1)/*.c $(ROOT)/userland/$(1)/*.h) $(NLIBC_STAMP)
	@mkdir -p $(USR_DIR)
	$(NLCC) $(NLIBC_PROG_CFLAGS) -D_POSIX_C_SOURCE=200809L -o $$@ $(wildcard $(ROOT)/userland/$(1)/*.c)
endef
$(foreach a,$(NLIBC_APPS),$(eval $(call NLIBC_APP_RULE,$(a))))

$(COREUTILS_ELFS): $(USR_DIR)/%.run: $(COREUTILS_DIR)/%.c $(COREUTILS_DIR)/common.c $(COREUTILS_DIR)/common.h $(NLIBC_STAMP)
	@mkdir -p $(USR_DIR)
	$(NLCC) $(NLIBC_PROG_CFLAGS) -D_POSIX_C_SOURCE=200809L -o $@ $< $(COREUTILS_DIR)/common.c

# ------------------------------------------------------------------
# kilo (텍스트 편집기) = third_party/kilo (upstream 그대로) + userland/kilo/kilo-nyxis.patch -> fs:/usr/kilo.run
#   C99 소스라 -std=gnu99. upstream 의 경고(문자열 배열)는 그대로 두므로 -Werror 는 쓰지 않는다.
KILO_DIR = $(ROOT)/third_party/kilo
KILO_SRC = $(BUILD)/kilo/kilo.c
KILO_ELF = $(USR_DIR)/kilo.run

$(KILO_SRC): $(KILO_DIR)/kilo.c $(ROOT)/userland/kilo/kilo-nyxis.patch
	@rm -rf $(BUILD)/kilo
	@mkdir -p $(BUILD)/kilo
	cp $(KILO_DIR)/kilo.c $(KILO_SRC)
	patch -s -p1 -d $(BUILD)/kilo < $(ROOT)/userland/kilo/kilo-nyxis.patch

$(KILO_ELF): $(KILO_SRC) $(NLIBC_STAMP)
	@mkdir -p $(USR_DIR)
	$(NLCC) -std=gnu99 $(OPT) -Wall -o $@ $(KILO_SRC)

# ------------------------------------------------------------------
# nlcc (Nyxis 안의 C 컴파일러) = TinyCC + userland/nlcc/ (설정과 패치). 자세한 것은 userland/nlcc/README.md
#   $(NLCC_SRC)      : third_party/tinycc 에 tinycc-nyxis.patch 를 적용한 사본 + config.h
#   $(NLCC_HOST)     : 같은 소스를 호스트 gcc 로 빌드한 tcc (sysroot 를 가리킨다). libtcc1.a 와 빈 crti/crtn 을 만든다
#   $(NLCC_ROOT)/lib : Nyxis 의 app:/lib (tcc/ = tcc 헤더 + libtcc1.a, nlibc/ = 헤더 + crt1.o crti.o crtn.o libc.a)
#   $(NLCC_ROOT)/src : Nyxis 의 app:/src/tcc (부트스트랩: Nyxis 안에서 nlcc 가 자기 자신을 컴파일한다)
#   nlcc.run         : stage0 = 호스트 nlcc(gcc) 로 빌드한 Nyxis 용 tcc
HOST_CC   ?= gcc
TCC_DIR    = $(ROOT)/third_party/tinycc
NLCC_DIR   = $(ROOT)/userland/nlcc
NLCC_SRC   = $(BUILD)/nlcc/src
NLCC_ROOT  = $(BUILD)/nlcc/root
NLCC_HOST  = $(BUILD)/nlcc/tcc-host
NLCC_ELF   = $(USR_DIR)/nlcc.run
NLCC_STAMP = $(BUILD)/nlcc/.root
TCC_SRCS   = $(wildcard $(TCC_DIR)/*.c $(TCC_DIR)/*.h $(TCC_DIR)/*.def $(TCC_DIR)/include/*.h $(TCC_DIR)/lib/*)
TCC_FLAGS  = -DONE_SOURCE=1 -I$(NLCC_SRC)
LIBTCC1_SRCS = libtcc1.c stdatomic.c atomic.S builtin.c alloca.S alloca-bt.S va_list.c dsohandle.c

$(NLCC_SRC)/.patched: $(TCC_SRCS) $(NLCC_DIR)/tinycc-nyxis.patch $(NLCC_DIR)/config.h
	@rm -rf $(NLCC_SRC)
	@mkdir -p $(NLCC_SRC)/include $(NLCC_SRC)/lib
	cp $(TCC_DIR)/*.c $(TCC_DIR)/*.h $(TCC_DIR)/*.def $(TCC_DIR)/COPYING $(NLCC_SRC)/
	cp $(TCC_DIR)/include/*.h $(NLCC_SRC)/include/
	cp $(TCC_DIR)/lib/* $(NLCC_SRC)/lib/
	patch -s -p1 -d $(NLCC_SRC) < $(NLCC_DIR)/tinycc-nyxis.patch
	cp $(NLCC_DIR)/config.h $(NLCC_SRC)/config.h
	@touch $@

$(NLCC_HOST): $(NLCC_SRC)/.patched
	$(HOST_CC) -O2 -w $(TCC_FLAGS) -DCONFIG_TCCDIR='"$(abspath $(NLCC_ROOT))/lib/tcc"' -DCONFIG_NLIBC_DIR='"$(abspath $(NLCC_ROOT))/lib/nlibc"' \
		$(NLCC_SRC)/tcc.c -o $@ -lm

$(NLCC_ELF): $(NLCC_SRC)/.patched $(NLIBC_STAMP)
	@mkdir -p $(USR_DIR)
	$(NLCC) -std=gnu99 -O2 -w $(TCC_FLAGS) -o $@ $(NLCC_SRC)/tcc.c

$(NLCC_STAMP): $(NLCC_HOST) $(NLIBC_STAMP) $(wildcard $(NLIBC_DIR)/examples/*.c)
	@rm -rf $(NLCC_ROOT)
	@mkdir -p $(NLCC_ROOT)/lib/tcc/include $(NLCC_ROOT)/lib/nlibc/lib $(NLCC_ROOT)/src $(BUILD)/nlcc/obj
	cp $(NLCC_SRC)/include/*.h $(NLCC_ROOT)/lib/tcc/include/
	cp -r $(NLIBC_SDK)/include $(NLCC_ROOT)/lib/nlibc/
	cp $(NLIBC_SDK)/lib/nlcrt0.o $(NLCC_ROOT)/lib/nlibc/lib/crt1.o
	cp $(NLIBC_SDK)/lib/libnlibc.a $(NLCC_ROOT)/lib/nlibc/lib/libc.a
	: > $(BUILD)/nlcc/obj/empty.c
	$(NLCC_HOST) -c $(BUILD)/nlcc/obj/empty.c -o $(NLCC_ROOT)/lib/nlibc/lib/crti.o
	cp $(NLCC_ROOT)/lib/nlibc/lib/crti.o $(NLCC_ROOT)/lib/nlibc/lib/crtn.o
	@for f in $(LIBTCC1_SRCS); do \
		echo "  TCC $$f"; \
		$(NLCC_HOST) -c $(NLCC_SRC)/lib/$$f -I$(NLCC_SRC) -o $(BUILD)/nlcc/obj/$$f.o || exit 1; \
	done
	$(NLCC_HOST) -ar rcs $(NLCC_ROOT)/lib/tcc/libtcc1.a $(addprefix $(BUILD)/nlcc/obj/,$(addsuffix .o,$(LIBTCC1_SRCS)))
	cp -r $(NLCC_SRC) $(NLCC_ROOT)/src/tcc
	@mkdir -p $(NLCC_ROOT)/src/examples
	cp $(NLIBC_DIR)/examples/*.c $(NLCC_ROOT)/src/examples/
	rm -f $(NLCC_ROOT)/src/tcc/.patched
	@touch $@

nlcc: $(NLCC_ELF) $(NLCC_STAMP)

INITRD_RUN = $(ROOT)/userland/initrd/_ns_/app/hellowld.run
INITRD_IMG = $(BUILD)/initrd.img
OS_IMG     = $(BUILD)/esp.img
NYFS_PACK  = $(ROOT)/tools/nyfs-utils/build/nyfs-pack
ROOTFS_MANIFEST = $(BUILD)/rootfs.manifest
ROOTFS_SIZE = 32M

# =========================
# 영구 저장소 disk:/ (재부팅해도 남는다)
#   QEMU q35 에서 두 번째 -drive 는 AHCI 의 다음 포트에 SATA 디스크로 붙는다. 커널(mount_disk)이 nyfs 매직이
#   있는 디스크를 disk:/ 로 마운트한다 (커널은 포맷하지 않는다). 이미지는 out/ 밖에 두어 make clean 에도 남는다.
#   뿌리 디렉터리는 0777 (tmp:/ 처럼 누구나 만들 수 있다. 각자 만든 파일의 권한은 그 파일의 모드대로).
# =========================
DISK_IMG  ?= $(ROOT)/disk.img
DISK_SIZE ?= 64M
QEMU_DISK  = -drive file=$(DISK_IMG),format=raw

disk: $(DISK_IMG)

$(DISK_IMG): | $(NYFS_PACK)
	@mkdir -p $(BUILD)
	printf 'd / 0777 0 0\n' > $(BUILD)/disk.manifest
	$(NYFS_PACK) -s $(DISK_SIZE) -o $@ $(BUILD)/disk.manifest

disk-reset:
	rm -f $(DISK_IMG)
	$(MAKE) disk

# =========================
# 사용자 DB (roi:/users)
#   실제 이미지: config/users.db (git 이 추적하지 않음). `make users` 로 만든다 (비밀번호는 터미널에서 묻는다).
#   SELFTEST 이미지: 빌드마다 무작위 비밀번호의 root/test 계정 ($(BUILD)/test-password 에 기록, 테스트 하네스용)
# =========================
USERS_DB ?= $(ROOT)/config/users.db

ifneq ($(SELFTEST),)
ROOT_USERS_DB  = $(BUILD)/test-users.db
ROOTFS_EXTRA   = --file /roi/test_password=$(BUILD)/test-password:0600
$(ROOT_USERS_DB): $(ROOT)/tools/mkusers.py
	@mkdir -p $(BUILD)
	python3 $(ROOT)/tools/mkusers.py --test $(ROOT_USERS_DB) $(BUILD)/test-password
else
ROOT_USERS_DB  = $(USERS_DB)
ROOTFS_EXTRA   =
$(USERS_DB):
	@echo "error: no user database ($(USERS_DB))." >&2
	@echo "       run 'make users' to create accounts (or build with SELFTEST=1 for a test image)." >&2
	@false
endif

users:
	python3 $(ROOT)/tools/mkusers.py $(USERS_DB)

# 종단 간 검사: SELFTEST 이미지로 부팅 -> 로그인 -> nysh -> poweroff 까지 시리얼과 PS/2 키보드 두 경로로 확인
test-e2e:
	@if [ ! -f $(BUILD)/test-password ]; then echo "build a test image first: make clean && make SELFTEST=1 os" >&2; exit 1; fi
	python3 $(ROOT)/tools/e2e_login_test.py $(BUILD) serial
	python3 $(ROOT)/tools/e2e_login_test.py $(BUILD) ps2

# disk:/ 가 재부팅 뒤에도 남는지 (SELFTEST 이미지 + 새 디스크 이미지로 두 번 부팅)
test-disk: $(NYFS_PACK)
	@if [ ! -f $(BUILD)/test-password ]; then echo "build a test image first: make clean && make SELFTEST=1 os" >&2; exit 1; fi
	python3 $(ROOT)/tools/e2e_disk_test.py $(BUILD) $(NYFS_PACK)

$(NYFS_PACK):
	$(MAKE) -C $(ROOT)/tools/nyfs-utils pack

# 루트 파일시스템 = nyfs 이미지 (커널이 fs:/ 로 마운트)
ROI_SRC   = $(ROOT)/rootfs/roi
ROI_FILES = $(shell find $(ROI_SRC) -type f 2>/dev/null)

$(INITRD_IMG): $(USER_PROG_ELFS) $(ELFTEST_ELF) $(NLIBC_TEST_ELF) $(FPUTEST_ELF) $(HELLO_ELF) $(COREUTILS_ELFS) $(NLIBC_APP_ELFS) $(KILO_ELF) $(NLCC_ELF) $(NLCC_STAMP) $(INITRD_RUN) $(ROOT_USERS_DB) $(NYFS_PACK) $(ROOT)/tools/mkrootfs.py $(ROI_FILES)
	python3 $(ROOT)/tools/mkrootfs.py --users $(ROOT_USERS_DB) --bin $(USR_DIR) --roi $(ROI_SRC) \
		--file /usr/hellowld.run=$(INITRD_RUN):0644 --tree /usr/lib=$(NLCC_ROOT)/lib --tree /usr/src=$(NLCC_ROOT)/src \
		$(ROOTFS_EXTRA) > $(ROOTFS_MANIFEST)
	$(NYFS_PACK) -s $(ROOTFS_SIZE) -o $@ $(ROOTFS_MANIFEST)

# FAT16 은 운영체제에서 쓰지 않는다: 호스트 테스트(make test-host)용 이미지만 만든다
FAT16_TEST_IMG = $(BUILD)/fat16-test.img

$(FAT16_TEST_IMG): $(INITRD_RUN)
	@mkdir -p $(BUILD)
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=16 status=none
	mkfs.vfat -F 16 -n INITRD $@
	mmd -i $@ ::_ns_
	mmd -i $@ ::_ns_/app
	mcopy -i $@ $(INITRD_RUN) ::_ns_/app/hellowld.run

os: $(OS_IMG)

$(OS_IMG): $(BOOTLOADER) $(KERNEL) $(INITRD_IMG)
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mkfs.vfat $@
	mmd -i $@ ::EFI
	mmd -i $@ ::EFI/BOOT
	mcopy -i $@ $(BOOTLOADER) ::EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(KERNEL) ::kernel.elf
	mcopy -i $@ $(INITRD_IMG) ::initrd.img

# =========================
# Run (UEFI x86_64, QEMU + OVMF)
# =========================
# OVMF_VARS 는 실행마다 새 복사본을 사용한다 (원본 펌웨어 변수 파일을 수정하지 않음)
QEMU_COMMON = \
	-m 1024 \
	-drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	-drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd

prepare-run: os
	cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd

run: prepare-run $(DISK_IMG)
	env -i \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		DISPLAY=$(DISPLAY) \
		XAUTHORITY=$(XAUTHORITY) \
		WAYLAND_DISPLAY=$(WAYLAND_DISPLAY) \
		XDG_RUNTIME_DIR=$(XDG_RUNTIME_DIR) \
		DBUS_SESSION_BUS_ADDRESS=$(DBUS_SESSION_BUS_ADDRESS) \
		qemu-system-x86_64 \
			-machine q35 \
			$(QEMU_COMMON) \
			-drive file=$(OS_IMG),format=raw \
			$(QEMU_DISK) \
			-vga std \
			-serial stdio \
			-display gtk

run-nographic: prepare-run $(DISK_IMG)
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_COMMON) \
		-drive file=$(OS_IMG),format=raw \
		$(QEMU_DISK) \
		-vga std \
		-display none \
		-serial mon:stdio

run-usb: prepare-run
	env -i \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		DISPLAY=$(DISPLAY) \
		XAUTHORITY=$(XAUTHORITY) \
		WAYLAND_DISPLAY=$(WAYLAND_DISPLAY) \
		XDG_RUNTIME_DIR=$(XDG_RUNTIME_DIR) \
		DBUS_SESSION_BUS_ADDRESS=$(DBUS_SESSION_BUS_ADDRESS) \
		qemu-system-x86_64 \
			$(QEMU_COMMON) \
			-device usb-ehci,id=usb \
			-device usb-storage,drive=esp,bus=usb.0 \
			-drive file=$(OS_IMG),format=raw,if=none,id=esp \
			-vga std \
			-serial stdio \
			-display gtk

# =========================
# GRUB (Multiboot2) 부팅 이미지: BIOS/UEFI 겸용 ISO
#   OS 는 UEFI 에 종속되지 않는다. GRUB 이 Multiboot2 로 커널을 적재하면 kernel/boot/boot.s 의
#   32비트 진입점(_start_mb)에서 시작하고, 부트 정보는 multiboot2.c 가 NTBLI 로 변환한다.
# =========================
ISO_DIR = $(BUILD)/isodir
ISO_IMG = $(BUILD)/nyxis.iso

iso: $(ISO_IMG)

$(ISO_IMG): $(KERNEL) $(INITRD_IMG)
	rm -rf $(ISO_DIR)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	cp $(INITRD_IMG) $(ISO_DIR)/boot/initrd.img
	printf 'insmod all_video\nset timeout=0\nset default=0\nset gfxmode=1024x768x32\nset gfxpayload=keep\nmenuentry "Nyxis OS" {\n\tmultiboot2 /boot/kernel.elf\n\tmodule2 /boot/initrd.img initrd\n\tboot\n}\n' > $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR) >/dev/null 2>&1

# BIOS(SeaBIOS) + GRUB
run-grub-bios: iso
	env -i \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		DISPLAY=$(DISPLAY) \
		XAUTHORITY=$(XAUTHORITY) \
		WAYLAND_DISPLAY=$(WAYLAND_DISPLAY) \
		XDG_RUNTIME_DIR=$(XDG_RUNTIME_DIR) \
		DBUS_SESSION_BUS_ADDRESS=$(DBUS_SESSION_BUS_ADDRESS) \
		qemu-system-x86_64 -m 1024 -cdrom $(ISO_IMG) -vga std -serial stdio -display gtk

# UEFI(OVMF) + GRUB
run-grub-uefi: iso
	cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	env -i \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		DISPLAY=$(DISPLAY) \
		XAUTHORITY=$(XAUTHORITY) \
		WAYLAND_DISPLAY=$(WAYLAND_DISPLAY) \
		XDG_RUNTIME_DIR=$(XDG_RUNTIME_DIR) \
		DBUS_SESSION_BUS_ADDRESS=$(DBUS_SESSION_BUS_ADDRESS) \
		qemu-system-x86_64 -machine q35 $(QEMU_COMMON) -cdrom $(ISO_IMG) -vga std -serial stdio -display gtk

# =========================
# Host test: 커널 파일시스템 코드를 리눅스에서 ASan/UBSan 으로 실행
#   (NYX_HOST_TEST 는 cli/sti 같은 특권 명령을 no-op 으로 바꾸는 스위치. 커널 빌드에서는 쓰지 않는다)
# =========================
HOST_DIR   = $(ROOT)/tests/host
HOST_OUT   = $(ROOT)/out-host
HOST_SAN   = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1
HOST_KSRCS = $(ROOT)/drivers/filesystem/core.c $(ROOT)/drivers/filesystem/vfs.c \
             $(ROOT)/drivers/filesystem/fat16/fat16.c $(ROOT)/drivers/ramdisk/ramdisk.c
ITER      ?= 2000
FP_ITER   ?= 300000
SEED      ?= 1

test-host: $(FAT16_TEST_IMG)
	@mkdir -p $(HOST_OUT)
	@for s in $(HOST_KSRCS); do \
		o=$(HOST_OUT)/$$(basename $$s .c).o; \
		echo "  CC(host,san) $$s"; \
		$(CC) -std=c89 -pedantic -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes -Wshadow \
			-Wdeclaration-after-statement -nostdinc -DNYX_HOST_TEST $(HOST_SAN) \
			-I$(INCLUDE_DIR) -I$(ROOT) -I. -c $$s -o $$o || exit 1; \
	done
	$(CC) -std=gnu99 -Wall -Wextra -DNYX_HOST_TEST $(HOST_SAN) \
		-idirafter $(INCLUDE_DIR) -I$(ROOT) -I. \
		$(HOST_DIR)/fat16_test.c $(HOST_OUT)/core.o $(HOST_OUT)/vfs.o $(HOST_OUT)/fat16.o $(HOST_OUT)/ramdisk.o -o $(HOST_OUT)/fat16_test
	$(HOST_OUT)/fat16_test $(FAT16_TEST_IMG) $(ITER) $(SEED)
	$(CC) -std=c89 -pedantic -Wall -Wextra -Werror -Wmissing-prototypes -Wshadow -Wdeclaration-after-statement -nostdinc \
		-DNYX_HOST_TEST $(HOST_SAN) -I$(INCLUDE_DIR) -I$(ROOT) -I. -c kernel/boot/multiboot2.c -o $(HOST_OUT)/mb2_adapter.o
	$(CC) -std=gnu99 -Wall -Wextra -DNYX_HOST_TEST $(HOST_SAN) -idirafter $(INCLUDE_DIR) -I$(ROOT) -I. \
		$(HOST_DIR)/mb2_test.c $(HOST_OUT)/mb2_adapter.o -o $(HOST_OUT)/mb2_test
	$(HOST_OUT)/mb2_test 20000

# nlibc 호스트 검사: 같은 libc 소스를 플랫폼 계층만 Linux 시스템 콜(tests/host/nlibc/sys_linux.c)로 바꿔
# 정적 Linux 바이너리로 링크해 실행한다 (커널 없이 수 초 만에 libc 회귀 확인)
NLIBC_HOST_OUT = $(HOST_OUT)/nlibc
NLIBC_HOST_SRCS = $(patsubst %,$(NLIBC_DIR)/src/%.c,$(NLIBC_MODS)) $(HOST_DIR)/nlibc/sys_linux.c
NLIBC_HOST_LIB  = $(patsubst %,$(NLIBC_HOST_OUT)/%.o,$(NLIBC_MODS) $(NLIBC_ASM_MODS)) $(NLIBC_HOST_OUT)/sys_linux.o

# 호스트용 nlibc 오브젝트 (crt0.o + 모듈들 + Linux 플랫폼 계층)
nlibc-host-lib:
	@mkdir -p $(NLIBC_HOST_OUT)/tmp
	@for s in $(NLIBC_HOST_SRCS); do \
		echo "  CC(nlibc,host) $$s"; \
		$(CC) $(NLIBC_CFLAGS) -c $$s -o $(NLIBC_HOST_OUT)/$$(basename $$s .c).o || exit 1; \
	done
	$(AS) $(ASFLAGS) $(NLIBC_DIR)/src/crt0.s -o $(NLIBC_HOST_OUT)/crt0.o
	@for m in $(NLIBC_ASM_MODS); do $(AS) $(ASFLAGS) $(NLIBC_DIR)/src/$$m.s -o $(NLIBC_HOST_OUT)/$$m.o || exit 1; done

test-nlibc: nlibc-host-lib
	$(CC) $(NLIBC_CFLAGS) -c $(NLIBC_DIR)/test/nlibc_test.c -o $(NLIBC_HOST_OUT)/nlibc_test.o
	$(LD) -static -nostdlib -o $(NLIBC_HOST_OUT)/nlibc_test $(NLIBC_HOST_OUT)/crt0.o $(NLIBC_HOST_OUT)/nlibc_test.o \
		$(NLIBC_HOST_LIB)
	@$(NLIBC_HOST_OUT)/nlibc_test $(NLIBC_HOST_OUT)/tmp/ > $(NLIBC_HOST_OUT)/out.txt; rc=$$?; \
		cat $(NLIBC_HOST_OUT)/out.txt; \
		if [ $$rc -ne 0 ]; then echo "nlibc: $$rc failure(s)"; exit 1; fi; \
		tail -n 1 $(NLIBC_HOST_OUT)/out.txt | grep -qx 'atexit 2 1' || { echo "nlibc: atexit/flush check failed"; exit 1; }
	@# 2단계: 부동소수점 변환/수학 함수를 glibc 와 차분 비교 (nlibc 쪽 이름은 tests/host/nlibc/rename.h 로 nl_* 로 바꾼다)
	@for m in printf fpconv math; do \
		echo "  CC(nlibc,fpdiff) $$m.c"; \
		$(CC) -std=c89 -pedantic -Wall -Wextra -Werror -ffreestanding -nostdinc -fno-builtin $(HOST_SAN) \
			-I$(NLIBC_DIR)/library -I$(INCLUDE_DIR) -include $(HOST_DIR)/nlibc/rename.h \
			-c $(NLIBC_DIR)/src/$$m.c -o $(NLIBC_HOST_OUT)/fpdiff_$$m.o || exit 1; \
	done
	$(CC) -O1 -Wall -Wextra $(HOST_SAN) $(HOST_DIR)/nlibc/fpdiff.c \
		$(NLIBC_HOST_OUT)/fpdiff_printf.o $(NLIBC_HOST_OUT)/fpdiff_fpconv.o $(NLIBC_HOST_OUT)/fpdiff_math.o \
		-lm -o $(NLIBC_HOST_OUT)/fpdiff
	$(NLIBC_HOST_OUT)/fpdiff $(FP_ITER) $(SEED)
	@echo "[test-nlibc] OK"

# 명령어 호스트 검사: coreutils 를 호스트용 nlibc 로 링크해 tests/host/coreutils/run.sh 로 동작을 확인한다
CU_HOST_OUT = $(HOST_OUT)/coreutils

test-coreutils: nlibc-host-lib
	@mkdir -p $(CU_HOST_OUT)/bin
	$(CC) $(NLIBC_CFLAGS) -c $(COREUTILS_DIR)/common.c -o $(CU_HOST_OUT)/common.o
	@for u in $(COREUTILS); do \
		echo "  CC(coreutils,host) $$u"; \
		$(CC) $(NLIBC_CFLAGS) -c $(COREUTILS_DIR)/$$u.c -o $(CU_HOST_OUT)/$$u.o || exit 1; \
		$(LD) -static -nostdlib -o $(CU_HOST_OUT)/bin/$$u $(NLIBC_HOST_OUT)/crt0.o $(CU_HOST_OUT)/$$u.o \
			$(CU_HOST_OUT)/common.o $(NLIBC_HOST_LIB) || exit 1; \
	done
	sh $(HOST_DIR)/coreutils/run.sh $(CU_HOST_OUT)/bin $(CU_HOST_OUT)/work
	$(CC) $(NLIBC_CFLAGS) -c $(ROOT)/userland/nysh/main.c -o $(CU_HOST_OUT)/nysh.o
	$(LD) -static -nostdlib -o $(CU_HOST_OUT)/bin/nysh $(NLIBC_HOST_OUT)/crt0.o $(CU_HOST_OUT)/nysh.o $(NLIBC_HOST_LIB)
	sh $(HOST_DIR)/nysh/run.sh $(CU_HOST_OUT)/bin $(CU_HOST_OUT)/work-nysh

# =========================
# Check: C89 준수/경고 검사
# =========================
check:
	@echo "[check] -O0 빌드 (-std=c89 -pedantic -Werror; '//' 주석은 컴파일러가 오류로 처리)"
	$(MAKE) -s -B BUILD=$(ROOT)/out-check-O0 OPT=-O0 all
	@echo "[check] -O2 빌드"
	$(MAKE) -s -B BUILD=$(ROOT)/out-check-O2 OPT=-O2 all
	@rm -rf $(ROOT)/out-check-O0 $(ROOT)/out-check-O2
	@echo "[check] OK"

clean:
	rm -rf $(BUILD)

.PHONY: test-disk disk disk-reset all kernel os iso run run-nographic run-usb run-grub-bios run-grub-uefi prepare-run check test-host test-nlibc test-coreutils nlibc-host-lib nlibc-sdk nlcc clean users test-e2e
