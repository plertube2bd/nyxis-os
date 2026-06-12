CC = gcc
AS = as
LD = ld
OBJCOPY = objcopy

ROOT = ..
INCLUDE_DIR = $(ROOT)/include
BUILD = $(ROOT)/out

# =========================
# EFI (64-bit UEFI)
# =========================
EFIINC = /usr/include/efi
EFIARCH = /usr/include/efi/x86_64

EFILIB = /usr/lib
CRT0 = /usr/lib/crt0-efi-x86_64.o

OVMF = /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS = /usr/share/OVMF/OVMF_VARS_4M.fd

# =========================
# Bootloader Flags
# =========================
CFLAGS = \
-I$(EFIINC) \
-I$(EFIARCH) \
-I$(INCLUDE_DIR) \
-std=gnu11 \
-ffreestanding \
-fshort-wchar \
-fpic \
-mno-red-zone \
-fno-stack-protector \
-fno-builtin \
-fno-common \
-Wall -Wextra

# =========================
# Kernel (64-bit)
# =========================
KERNEL_CFLAGS = \
	-nostdinc \
	-std=gnu11 \
	-fno-builtin \
	-fno-stack-protector \
	-I. \
	-I$(INCLUDE_DIR) \
	-Ikernel \
	-Iconsole/outputs \
	-m64

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
	-T kernel/linker64.lds

# =========================
# Sources
# =========================
BOOTLOADER_SRC = $(ROOT)/bootloader/nytb/nytb3.c
BOOTLOADER_OBJ = $(BUILD)/nytb3.o
BOOTLOADER_SO = $(BUILD)/BOOTX64.so
BOOTLOADER = $(BUILD)/BOOTX64.EFI

KERNEL = $(BUILD)/kernel.elf
KERNEL_OBJS = \
	$(BUILD)/start.o \
	$(BUILD)/kernel.o \
	$(BUILD)/memory.o \
	$(BUILD)/paging/paging.o \
	$(BUILD)/string.o \
	$(BUILD)/console/outputs/printk.o \
	$(BUILD)/console/outputs/outputs_base.o \
	$(BUILD)/ahci.o \
	$(BUILD)/ramdisk.o \
	$(BUILD)/filesystem/core.o \
	$(BUILD)/filesystem/vfs.o \
	$(BUILD)/filesystem/fat16/fat16.o \
	$(BUILD)/error_handling/panic.o \
	$(BUILD)/process/process.o \
	$(BUILD)/process/schedule.o \
	$(BUILD)/kernel/syscall/syscall.o \
	$(BUILD)/helloworld.o \
	$(BUILD)/interrupt/interrupt.o

# =========================
# Targets
# =========================
all: $(BOOTLOADER) $(KERNEL)

kernel: $(KERNEL)

$(BUILD):
	mkdir -p $(BUILD)

# Bootloader
$(BOOTLOADER_OBJ): $(BOOTLOADER_SRC) | $(BUILD)
	mkdir -p $(dir $@)
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
		-j .rela \
		-j .reloc \
		--target=efi-app-x86_64 \
		$< \
		$@

# Kernel
$(BUILD)/kernel.o: kernel/kernel.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/memory.o: $(ROOT)/nxkernel/lib/memory.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/paging/paging.o: kernel/paging/paging.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/string.o: $(ROOT)/nxkernel/lib/string.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/console/outputs/printk.o: console/outputs/printk.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/console/outputs/outputs_base.o: console/outputs/outputs_base.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/ahci.o: $(ROOT)/drivers/ahci/ahci.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/ramdisk.o: $(ROOT)/drivers/ramdisk/ramdisk.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/filesystem/core.o: $(ROOT)/drivers/filesystem/core.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -I$(ROOT) -c $< -o $@

$(BUILD)/filesystem/vfs.o: $(ROOT)/drivers/filesystem/vfs.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -I$(ROOT) -c $< -o $@

$(BUILD)/filesystem/fat16/fat16.o: $(ROOT)/drivers/filesystem/fat16/fat16.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -I$(ROOT) -c $< -o $@

$(BUILD)/error_handling/panic.o: kernel/error_handling/panic.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/process/process.o: kernel/process/process.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/process/schedule.o: kernel/process/schedule.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall/syscall.o: kernel/syscall/syscall.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/helloworld.o: $(ROOT)/userland/helloworld/helloworld.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -I$(ROOT)/userland/nlibc/library -c $< -o $@

$(BUILD)/interrupt/interrupt.o: kernel/interrupt/interrupt.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/start.o: kernel/start.s | $(BUILD)
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Kernel link
$(KERNEL): $(KERNEL_OBJS)
	$(LD) $(KERNEL_LDFLAGS) -o $@ $^

INITRD_IMG = $(ROOT)/userland/initrd/initrd.img
INITRD_RUN = $(ROOT)/userland/initrd/_ns_/app/hellowld.run
OS_IMG = $(BUILD)/esp.img

$(INITRD_IMG): $(INITRD_RUN)
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=16
	mkfs.vfat -F 16 -n INITRD $@
	mmd -i $@ _ns_
	mmd -i $@ _ns_/app
	mcopy -i $@ $(INITRD_RUN) ::_ns_/app/hellowld.run

os: $(BOOTLOADER) $(KERNEL) $(INITRD_IMG) $(OS_IMG)

$(OS_IMG): $(BOOTLOADER) $(KERNEL) $(INITRD_IMG)
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=64
	mkfs.vfat $@

	mmd -i $@ EFI
	mmd -i $@ EFI/BOOT

	mcopy -i $@ $(BOOTLOADER) ::EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(KERNEL) ::kernel.elf
	mcopy -i $@ $(INITRD_IMG) ::initrd.img

# =========================
# Run (UEFI x86_64)
# =========================
run: os
	mkdir -p $(BUILD)
	cp $(OVMF) $(BUILD)/OVMF_CODE.fd
	cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd

	env -i \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		DISPLAY=$(DISPLAY) \
		XDG_RUNTIME_DIR=$(XDG_RUNTIME_DIR) \
		DBUS_SESSION_BUS_ADDRESS=$(DBUS_SESSION_BUS_ADDRESS) \
		qemu-system-x86_64 \
			-drive if=pflash,format=raw,file=$(BUILD)/OVMF_CODE.fd \
			-drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
			-device usb-ehci,id=usb \
			-device usb-storage,drive=esp,bus=usb.0 \
			-drive file=$(OS_IMG),format=raw,if=none,id=esp \
			-device virtio-gpu-pci \
			-display gtk

clean:
	rm -rf $(BUILD)

.PHONY: all os run clean