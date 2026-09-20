# =====================================================================
# Nyxis OS 빌드 시스템
#
#   make            부트로더 + 커널 빌드            (out/BOOTX64.EFI, out/kernel.elf)
#   make os         부팅 가능한 ESP 디스크 이미지    (out/esp.img)
#   make run        QEMU(OVMF, q35+AHCI) 로 실행     (GTK 창 + 시리얼 stdio)
#   make run-nographic  화면 없이 시리얼만으로 실행
#   make run-usb    예전 방식(usb-storage + virtio-gpu) 으로 실행
#   make check      C89 엄격 컴파일(-O0/-O2) 검사
#   make test-host  FAT16/VFS/램디스크 호스트 테스트 (회귀 + ASan/UBSan 변이 퍼징)
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
#      -fno-pie -fno-pic -fcf-protection=none       : 고정 주소(1MiB)에 링크되는 정적 커널
#      -fno-tree-loop-distribute-patterns           : memset/memcpy 구현이 자기 자신을 호출하는 재귀 방지
#      -fno-stack-protector -fno-builtin -nostdinc -ffreestanding
#  - 사용하지 않던 kernel/timer, pic, ata, inputs, graphics 도 빌드에 포함 (컴파일 검증).
#  - 규칙 반복(복붙 20여 개)을 패턴 규칙으로 통합, 헤더 의존성(-MMD) 추적.
#  - initrd.img 를 저장소의 추적 파일에 덮어쓰지 않고 out/ 에 생성.
#  - 링커: 4KiB 정렬 세그먼트(-z max-page-size=0x1000), NX 스택 표시.
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
	-std=c89 \
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
	kernel/error_handling/panic.c \
	kernel/interrupt/interrupt.c \
	kernel/interrupt/gdt.c \
	kernel/paging/paging.c \
	kernel/process/process.c \
	kernel/process/schedule.c \
	kernel/syscall/syscall.c \
	kernel/timer/pit/pit_base.c \
	lib/memory.c \
	lib/string.c \
	console/outputs/printk.c \
	console/outputs/outputs_base.c \
	console/outputs/serial.c \
	console/inputs/inputs_base.c \
	graphics/graphics_base.c \
	$(ROOT)/drivers/ahci/ahci.c \
	$(ROOT)/drivers/ata/ata.c \
	$(ROOT)/drivers/pic/pic.c \
	$(ROOT)/drivers/ramdisk/ramdisk.c \
	$(ROOT)/drivers/filesystem/core.c \
	$(ROOT)/drivers/filesystem/vfs.c \
	$(ROOT)/drivers/filesystem/fat16/fat16.c

ifneq ($(SELFTEST),)
KERNEL_C_SRCS += kernel/selftest.c
endif

# 어셈블리 소스
KERNEL_S_SRCS = \
	kernel/start.s \
	kernel/interrupt/isr_stubs.s \
	kernel/process/switch.s

# 임시로 커널 이미지에 함께 링크되는 유저랜드 예제 (유저 프로그램 로더가 생기면 제거 예정)
USER_HELLO_SRC = $(ROOT)/userland/helloworld/helloworld.c
USER_HELLO_OBJ = $(BUILD)/obj/helloworld.o

# 소스 경로 -> 오브젝트 경로 ("../" 접두사 제거하여 out/obj 아래에 배치)
obj_of = $(BUILD)/obj/$(patsubst ../%,%,$(1))

KERNEL_OBJS = \
	$(foreach s,$(KERNEL_S_SRCS),$(call obj_of,$(s:.s=.o))) \
	$(foreach s,$(KERNEL_C_SRCS),$(call obj_of,$(s:.c=.o))) \
	$(USER_HELLO_OBJ)

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
		-j .dynamic \
		-j .dynsym \
		-j .rel \
		-j .rela \
		-j .rel.* \
		-j .rela.* \
		-j .reloc \
		--target=efi-app-x86_64 \
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

$(USER_HELLO_OBJ): $(USER_HELLO_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -I$(ROOT)/userland/nlibc/library -c $< -o $@

# Kernel: assembly
$(BUILD)/obj/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Kernel link
$(KERNEL): $(KERNEL_OBJS) kernel/linker64.lds
	$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null) $(BUILD)/nytb3.d

# =========================
# Disk images
# =========================
INITRD_RUN = $(ROOT)/userland/initrd/_ns_/app/hellowld.run
INITRD_IMG = $(BUILD)/initrd.img
OS_IMG     = $(BUILD)/esp.img

$(INITRD_IMG): $(INITRD_RUN)
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

run: prepare-run
	env -i \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		DISPLAY=$(DISPLAY) \
		XDG_RUNTIME_DIR=$(XDG_RUNTIME_DIR) \
		DBUS_SESSION_BUS_ADDRESS=$(DBUS_SESSION_BUS_ADDRESS) \
		qemu-system-x86_64 \
			-machine q35 \
			$(QEMU_COMMON) \
			-drive file=$(OS_IMG),format=raw \
			-vga std \
			-serial stdio \
			-display gtk

run-nographic: prepare-run
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_COMMON) \
		-drive file=$(OS_IMG),format=raw \
		-vga std \
		-display none \
		-serial mon:stdio

run-usb: prepare-run
	env -i \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		DISPLAY=$(DISPLAY) \
		XDG_RUNTIME_DIR=$(XDG_RUNTIME_DIR) \
		DBUS_SESSION_BUS_ADDRESS=$(DBUS_SESSION_BUS_ADDRESS) \
		qemu-system-x86_64 \
			$(QEMU_COMMON) \
			-device usb-ehci,id=usb \
			-device usb-storage,drive=esp,bus=usb.0 \
			-drive file=$(OS_IMG),format=raw,if=none,id=esp \
			-device virtio-gpu-pci \
			-serial stdio \
			-display gtk

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
SEED      ?= 1

test-host: $(INITRD_IMG)
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
		$(HOST_DIR)/fat16_test.c $(HOST_OUT)/*.o -o $(HOST_OUT)/fat16_test
	$(HOST_OUT)/fat16_test $(INITRD_IMG) $(ITER) $(SEED)

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

.PHONY: all kernel os run run-nographic run-usb prepare-run check test-host clean
