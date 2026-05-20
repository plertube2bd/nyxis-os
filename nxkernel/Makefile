CC = x86_64-elf-gcc
AS = as
LD = ld

ROOT = ..
BUILD = $(ROOT)/out

# =========================
# EFI (64-bit UEFI)
# =========================
EFIINC = /usr/include/efi
EFIARCH = /usr/include/efi/x86_64

EFILIB = /usr/lib
CRT0 =

OVMF = /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS = /usr/share/OVMF/OVMF_VARS_4M.fd

# =========================
# Bootloader Flags
# =========================
CFLAGS = \
-I$(EFIINC) \
-I$(EFIARCH) \
	-I$(ROOT)/nxkernel \
-fshort-wchar \
-std=gnu11 \
-ffreestanding \
-nostdlib -nostartfiles -nodefaultlibs \
-fno-stack-protector \
-fPIC \
-mno-red-zone \
-fno-builtin \
-fno-common \
-fno-omit-frame-pointer \
-fno-delete-null-pointer-checks \
-fno-strict-aliasing \
-mno-mmx -mno-sse -mno-sse2 \
-g \
-Wall -Wextra \
-Wshadow \
-Wundef \
-Werror \
-Wcast-align \
-Wwrite-strings \
-Wstrict-prototypes \
-Wmissing-prototypes \
-Wformat=2

# =========================
# Kernel (64-bit)
# =========================
KERNEL_CFLAGS = \
	-nostdinc \
	-I. \
	-I$(ROOT)/nxkernel \
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
BOOTLOADER_SRC = $(ROOT)/bootloader/nytb3.c
BOOTLOADER_OBJ = $(BUILD)/nytb3.o
BOOTLOADER = $(BUILD)/BOOTX64.EFI

KERNEL = $(BUILD)/kernel.elf
KERNEL_OBJS = \
	$(BUILD)/start.o \
	$(BUILD)/kernel.o \
	$(BUILD)/memory.o \
	$(BUILD)/console/outputs/printk.o \
	$(BUILD)/console/outputs/outputs_base.o \
	$(BUILD)/ahci.o

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

$(BOOTLOADER): $(BOOTLOADER_OBJ)
	$(LD) $(EFI_LDFLAGS) $< -o $@

# Kernel
$(BUILD)/kernel.o: kernel/kernel.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/paging/paging.o: kernel/paging/paging.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/memory.o: $(ROOT)/nxkernel/include/memory.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/console/outputs/printk.o: console/outputs/printk.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/console/outputs/outputs_base.o: console/outputs/outputs_base.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/ahci.o: $(ROOT)/drivers/unfinished/ahci.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/start.o: kernel/start.s | $(BUILD)
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Kernel link
$(KERNEL): $(KERNEL_OBJS)
	$(LD) $(KERNEL_LDFLAGS) -o $@ $^

# =========================
# Run (UEFI x86_64)
# =========================
run: $(BOOTLOADER) $(KERNEL)
	dd if=/dev/zero of=esp.img bs=1M count=64
	mkfs.vfat esp.img

	mmd -i esp.img EFI
	mmd -i esp.img EFI/BOOT

	mcopy -i esp.img $(BOOTLOADER) ::EFI/BOOT/BOOTX64.EFI
	mcopy -i esp.img $(KERNEL) ::kernel.elf

	mkdir -p $(BUILD)
	cp $(OVMF) $(BUILD)/OVMF_CODE.fd
	cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd

	qemu-system-x86_64 \
		-drive if=pflash,format=raw,file=$(BUILD)/OVMF_CODE.fd \
		-drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
		-device usb-ehci,id=usb \
		-device usb-storage,drive=esp,bus=usb.0 \
		-drive file=esp.img,format=raw,if=none,id=esp \
		-nographic

clean:
	rm -f $(KERNEL_OBJ)
	rm -f $(BOOTLOADER_OBJ)
	rm -f $(BOOTLOADER)
	rm -f $(KERNEL)
	rm -f esp.img
	rm -rf $(BUILD)

.PHONY: all run clean