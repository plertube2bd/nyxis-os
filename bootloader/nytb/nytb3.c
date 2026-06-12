/* NYxis Tiny Bootloader : NYTB 3
 * This is first bootloader stage, which is a UEFI application that loads the kernel from the EFI System Partition and jumps to it.
 * It also sets up a simple boot information structure (NTBLI) that is passed to the kernel, which contains information about the framebuffer and memory size.
 * This bootloader is designed to be simple and easy to understand, and is not optimized for size or performance.
 * It is also not intended to be a general-purpose bootloader, but rather a simple loader for the Nyxis OS kernel
 * Directiry structure:
 * - EFI/BOOT/
 *   - nytbARCH.efi (this file) (ARCH = x64 or ia32)
 *   - kernel.elf (the kernel to load)
 * must kernel be in the same directory as the bootloader, and must be named "kernel.elf"
 * kernel can't be PE/COFF or anything, it must be static ELF file, and must be compiled for the correct architecture:
 * x86_64-elf-gcc for 64-bit
 * i386-elf-gcc for 32-bit
 * This bootloader does not support loading from other filesystems or from network, and does not support any advanced features like command line arguments or multiple kernel images.
 * must kernel entry get a pointer to NTBLI structure, and must not expect any other arguments or environment variables.
 * now code ;)
*/


#include <efi.h>
#include <efilib.h>
#include <efiprot.h>
#include <nyxis.h>

/* =========================================================
 * ELF32 DEFINITIONS
 * ========================================================= */

#define ELF_MAGIC 0x464C457F

#define ET_EXEC 2
#define EM_386  3
#define EM_X86_64 62

#define PT_LOAD 1

#define PAGE_SIZE 4096

#define ELFCLASS32 1
#define ELFCLASS64 2

typedef struct {
    UINT32 e_magic;

    UINT8  e_class;
    UINT8  e_data;
    UINT8  e_version;
    UINT8  e_osabi;
    UINT8  e_abiversion;
    UINT8  pad[7];

    UINT16 e_type;
    UINT16 e_machine;

    UINT32 e_version2;

    UINT64 e_entry;

    UINT64 e_phoff;
    UINT64 e_shoff;

    UINT32 e_flags;

    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;

    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;

} ELF64_HEADER;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;

    UINT64 p_offset;

    UINT64 p_vaddr;
    UINT64 p_paddr;

    UINT64 p_filesz;
    UINT64 p_memsz;

    UINT64 p_align;

} ELF64_PROGRAM_HEADER;

typedef struct {
    UINT32 e_magic;

    UINT8  e_class;
    UINT8  e_data;
    UINT8  e_version;
    UINT8  e_osabi;
    UINT8  e_abiversion;
    UINT8  pad[7];

    UINT16 e_type;
    UINT16 e_machine;

    UINT32 e_version2;
    UINT32 e_entry;

    UINT32 e_phoff;
    UINT32 e_shoff;

    UINT32 e_flags;

    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;

    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;

} ELF_HEADER;

typedef struct {
    UINT32 p_type;

    UINT32 p_offset;

    UINT32 p_vaddr;
    UINT32 p_paddr;

    UINT32 p_filesz;
    UINT32 p_memsz;

    UINT32 p_flags;
    UINT32 p_align;

} ELF_PROGRAM_HEADER;

typedef void (*kernel_entry_t)(NTBLI*);

EFI_STATUS read_file(EFI_FILE_HANDLE file, VOID** buffer, UINTN* size);
EFI_STATUS load_elf_kernel(VOID* elf_buffer, UINTN elf_size, kernel_entry_t* entry_point);
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable);

/* =========================================================
 * READ FILE
 * ========================================================= */

EFI_STATUS
read_file(
    EFI_FILE_HANDLE file,
    VOID** buffer,
    UINTN* size
) {
    EFI_STATUS Status;

    EFI_FILE_INFO* FileInfo;

    UINTN InfoSize = 0;

    Status = uefi_call_wrapper(
        file->GetInfo,
        4,
        file,
        &GenericFileInfo,
        &InfoSize,
        NULL
    );

    if (Status != EFI_BUFFER_TOO_SMALL) {
        return Status;
    }

    FileInfo = AllocatePool(InfoSize);

    if (!FileInfo) {
        return EFI_OUT_OF_RESOURCES;
    }

    Status = uefi_call_wrapper(
        file->GetInfo,
        4,
        file,
        &GenericFileInfo,
        &InfoSize,
        FileInfo
    );

    if (EFI_ERROR(Status)) {
        FreePool(FileInfo);
        return Status;
    }

    *size = FileInfo->FileSize;

    *buffer = AllocatePool(*size);

    if (!*buffer) {
        FreePool(FileInfo);
        return EFI_OUT_OF_RESOURCES;
    }

    Status = uefi_call_wrapper(
        file->Read,
        3,
        file,
        size,
        *buffer
    );

    FreePool(FileInfo);

    return Status;
}

/* =========================================================
 * LOAD ELF
 * ========================================================= */
EFI_STATUS
load_elf_kernel(
    VOID* elf_buffer,
    UINTN elf_size,
    kernel_entry_t* entry_point
) {
    (void)elf_size;
    UINT8 elf_class = *((UINT8*)elf_buffer + 4);

    UINTN i;

    if (elf_class == ELFCLASS32) {

        ELF_HEADER* hdr = (ELF_HEADER*)elf_buffer;

        if (hdr->e_magic != ELF_MAGIC)
            return EFI_INVALID_PARAMETER;

        if (hdr->e_machine != EM_386)
            return EFI_INVALID_PARAMETER;

        for (i = 0; i < hdr->e_phnum; i++) {

            ELF_PROGRAM_HEADER* phdr =
                (ELF_PROGRAM_HEADER*)(
                    (UINTN)elf_buffer +
                    hdr->e_phoff +
                    i * hdr->e_phentsize
                );

            if (phdr->p_type != PT_LOAD)
                continue;

            EFI_PHYSICAL_ADDRESS addr =
                (EFI_PHYSICAL_ADDRESS)phdr->p_paddr;

            UINTN pages =
                (phdr->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;

            EFI_STATUS Status =
                uefi_call_wrapper(
                    BS->AllocatePages,
                    4,
                    AllocateAddress,
                    EfiLoaderData,
                    pages,
                    &addr
                );

            if (EFI_ERROR(Status))
                return Status;

            SetMem((VOID*)(UINTN)addr,
                   phdr->p_memsz,
                   0);

            CopyMem(
                (VOID*)(UINTN)addr,
                (VOID*)((UINTN)elf_buffer + phdr->p_offset),
                phdr->p_filesz
            );
        }

        *entry_point =
            (kernel_entry_t)(UINTN)hdr->e_entry;

        Print(L"Loaded ELF32 kernel\n");

        return EFI_SUCCESS;
    }

    else if (elf_class == ELFCLASS64) {

        ELF64_HEADER* hdr = (ELF64_HEADER*)elf_buffer;

        if (hdr->e_magic != ELF_MAGIC)
            return EFI_INVALID_PARAMETER;

        if (hdr->e_machine != EM_X86_64)
            return EFI_INVALID_PARAMETER;

        for (i = 0; i < hdr->e_phnum; i++) {

            ELF64_PROGRAM_HEADER* phdr =
                (ELF64_PROGRAM_HEADER*)(
                    (UINTN)elf_buffer +
                    hdr->e_phoff +
                    i * hdr->e_phentsize
                );

            if (phdr->p_type != PT_LOAD)
                continue;

            EFI_PHYSICAL_ADDRESS addr =
                (EFI_PHYSICAL_ADDRESS)phdr->p_paddr;

            UINTN pages =
                (phdr->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;

            EFI_STATUS Status =
                uefi_call_wrapper(
                    BS->AllocatePages,
                    4,
                    AllocateAddress,
                    EfiLoaderData,
                    pages,
                    &addr
                );

            if (EFI_ERROR(Status))
                return Status;

            SetMem(
                (VOID*)(UINTN)addr,
                (UINTN)phdr->p_memsz,
                0
            );

            CopyMem(
                (VOID*)(UINTN)addr,
                (VOID*)((UINTN)elf_buffer + phdr->p_offset),
                (UINTN)phdr->p_filesz
            );
        }

        *entry_point =
            (kernel_entry_t)(UINTN)hdr->e_entry;

        Print(L"Loaded ELF64 kernel\n");

        return EFI_SUCCESS;
    }

    return EFI_UNSUPPORTED;
}

static UINT32 get_preferred_gop_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL* Gop)
{
    if (!Gop)
        return 0;

    UINT32 selected = Gop->Mode->Mode;

    for (UINT32 mode = 0; mode < Gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
        UINTN InfoSize;
        EFI_STATUS Status = uefi_call_wrapper(
            Gop->QueryMode,
            4,
            Gop,
            mode,
            &InfoSize,
            &Info
        );

        if (EFI_ERROR(Status))
            continue;

        Print(L"GOP mode %u: %ux%u stride=%u format=%u\n",
            mode,
            Info->HorizontalResolution,
            Info->VerticalResolution,
            Info->PixelsPerScanLine,
            Info->PixelFormat
        );

        if (Info->HorizontalResolution == 1024 && Info->VerticalResolution == 768) {
            selected = mode;
            break;
        }

        if (Info->HorizontalResolution == 800 && Info->VerticalResolution == 600 && selected == Gop->Mode->Mode) {
            selected = mode;
        }
    }

    return selected;
}

#define NYTB_DEBUG
#ifdef NYTB_DEBUG
static void fill_framebuffer_test(
    EFI_GRAPHICS_OUTPUT_PROTOCOL* Gop
)
{
    if (!Gop || !Gop->Mode || !Gop->Mode->Info)
        return;

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info = Gop->Mode->Info;
    u32 width = Info->HorizontalResolution;
    u32 height = Info->VerticalResolution;
    u32 rows = (height < 64) ? height : 64;
    u32 cols = (width < 256) ? width : 256;

    if (Gop->Mode->FrameBufferBase && Info->PixelFormat != PixelBltOnly) {
        u32* fb = (u32*)(UINTN)Gop->Mode->FrameBufferBase;
        u32 pitch = Info->PixelsPerScanLine;

        for (u32 y = 0; y < rows; y++) {
            for (u32 x = 0; x < cols; x++) {
                fb[y * pitch + x] = 0x00FF0000u | ((y << 8) ^ x);
            }
        }

        Print(L"Framebuffer direct write used\n");
        return;
    }

    EFI_STATUS Status;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel;
    Pixel.Blue = 0x00;
    Pixel.Green = 0x00;
    Pixel.Red = 0xFF;
    Pixel.Reserved = 0x00;

    Status = uefi_call_wrapper(
        Gop->Blt,
        10,
        Gop,
        &Pixel,
        EfiBltVideoFill,
        0,
        0,
        0,
        0,
        cols,
        rows,
        0
    );

    if (EFI_ERROR(Status)) {
        Print(L"GOP Blt fill failed: %r\n", Status);
    } else {
        Print(L"GOP Blt fill used\n");
    }
}
#endif
/* =========================================================
 * EFI MAIN
 * ========================================================= */

EFI_STATUS
EFIAPI
efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE* SystemTable
) {
    EFI_STATUS Status;

    EFI_LOADED_IMAGE* LoadedImage;

    EFI_FILE_IO_INTERFACE* FileSystem;

    EFI_FILE_HANDLE RootDir;
    EFI_FILE_HANDLE KernelFile;
    EFI_FILE_HANDLE InitrdFile;

    VOID* KernelBuffer = NULL;
    VOID* InitrdBuffer = NULL;

    UINTN KernelSize = 0;
    UINTN InitrdSize = 0;
    EFI_PHYSICAL_ADDRESS InitrdAddr = 0;

    kernel_entry_t KernelEntry = NULL;

    EFI_GRAPHICS_OUTPUT_PROTOCOL* Gop;

    NTBLI* BootInfo;

    /* Memory map */

    UINTN MemoryMapSize = 0;

    EFI_MEMORY_DESCRIPTOR* MemoryMap = NULL;

    UINTN MapKey;

    UINTN DescriptorSize;

    UINT32 DescriptorVersion;

    InitializeLib(ImageHandle, SystemTable);

    Print(L"Nyxis Bootloader v1.0\n");
    Print(L"=====================\n");

    /* =====================================================
     * Loaded image
     * ===================================================== */

    Status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        ImageHandle,
        &LoadedImageProtocol,
        (VOID**)&LoadedImage
    );

    if (EFI_ERROR(Status)) {
        Print(L"HandleProtocol failed: %r\n", Status);
        return Status;
    }

    /* =====================================================
     * File system
     * ===================================================== */

    Status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        LoadedImage->DeviceHandle,
        &FileSystemProtocol,
        (VOID**)&FileSystem
    );

    if (EFI_ERROR(Status)) {
        Print(L"FileSystemProtocol failed: %r\n", Status);
        return Status;
    }

    /* =====================================================
     * Open volume
     * ===================================================== */

    Status = uefi_call_wrapper(
        FileSystem->OpenVolume,
        2,
        FileSystem,
        &RootDir
    );

    if (EFI_ERROR(Status)) {
        Print(L"OpenVolume failed: %r\n", Status);
        return Status;
    }

    /* =====================================================
     * Open kernel
     * ===================================================== */

    Print(L"Opening kernel.elf...\n");

    Status = uefi_call_wrapper(
        RootDir->Open,
        5,
        RootDir,
        &KernelFile,
        L"kernel.elf",
        EFI_FILE_MODE_READ,
        0
    );

    if (EFI_ERROR(Status)) {
        Print(L"Failed to open kernel.elf: %r\n", Status);
        return Status;
    }

    /* =====================================================
     * Read kernel
     * ===================================================== */

    Status = read_file(
        KernelFile,
        &KernelBuffer,
        &KernelSize
    );

    if (EFI_ERROR(Status)) {
        Print(L"Failed to read kernel: %r\n", Status);
        return Status;
    }

    uefi_call_wrapper(KernelFile->Close, 1, KernelFile);

    Print(L"Opening initrd.img...\n");
    Status = uefi_call_wrapper(
        RootDir->Open,
        5,
        RootDir,
        &InitrdFile,
        L"initrd.img",
        EFI_FILE_MODE_READ,
        0
    );

    if (!EFI_ERROR(Status)) {
        Status = read_file(
            InitrdFile,
            &InitrdBuffer,
            &InitrdSize
        );

        if (EFI_ERROR(Status)) {
            Print(L"Failed to read initrd.img: %r\n", Status);
            FreePool(InitrdBuffer);
            InitrdBuffer = NULL;
            InitrdSize = 0;
        } else {
            EFI_PHYSICAL_ADDRESS InitrdAddr = 0;
            UINTN InitrdPages = (InitrdSize + PAGE_SIZE - 1) / PAGE_SIZE;

            Status = uefi_call_wrapper(
                BS->AllocatePages,
                4,
                AllocateAnyPages,
                EfiLoaderData,
                InitrdPages,
                &InitrdAddr
            );

            if (EFI_ERROR(Status)) {
                Print(L"Initrd allocation failed: %r\n", Status);
                FreePool(InitrdBuffer);
                InitrdBuffer = NULL;
                InitrdSize = 0;
                InitrdAddr = 0;
            } else {
                CopyMem((VOID*)(UINTN)InitrdAddr, InitrdBuffer, InitrdSize);
                InitrdBuffer = NULL;
                Print(L"Loaded initrd.img, %u bytes at %p\n", InitrdSize, (VOID*)(UINTN)InitrdAddr);
            }
        }

        uefi_call_wrapper(InitrdFile->Close, 1, InitrdFile);
    } else {
        Print(L"No initrd.img found, continuing without initrd\n");
    }

    uefi_call_wrapper(RootDir->Close, 1, RootDir);

    Print(L"Kernel size: %u bytes\n", KernelSize);

    /* =====================================================
     * Load ELF
     * ===================================================== */

    Status = load_elf_kernel(
        KernelBuffer,
        KernelSize,
        &KernelEntry
    );

    if (EFI_ERROR(Status)) {
        Print(L"ELF loading failed: %r\n", Status);
        FreePool(KernelBuffer);
        return Status;
    }

    Print(L"Kernel entry: 0x%x\n", (UINTN)KernelEntry);

    /* =====================================================
     * GOP
     * ===================================================== */

    Status = uefi_call_wrapper(
        BS->LocateProtocol,
        3,
        &GraphicsOutputProtocol,
        NULL,
        (VOID**)&Gop
    );

    if (EFI_ERROR(Status)) {
        Print(L"GOP failed: %r\n", Status);
        FreePool(KernelBuffer);
        return Status;
    }

    UINT32 preferred_mode = get_preferred_gop_mode(Gop);

    if (preferred_mode != Gop->Mode->Mode) {
        Status = uefi_call_wrapper(
            Gop->SetMode,
            2,
            Gop,
            preferred_mode
        );

        if (EFI_ERROR(Status)) {
            Print(L"GOP SetMode failed: %r\n", Status);
            FreePool(KernelBuffer);
            return Status;
        }
    }

    Print(L"Selected GOP mode: %u\n", Gop->Mode->Mode);
    Print(L"Framebuffer base: %p size: %u\n",
        (VOID*)(UINTN)Gop->Mode->FrameBufferBase,
        Gop->Mode->FrameBufferSize
    );
    Print(L"Resolution: %ux%u stride: %u format: %u\n",
        Gop->Mode->Info->HorizontalResolution,
        Gop->Mode->Info->VerticalResolution,
        Gop->Mode->Info->PixelsPerScanLine,
        Gop->Mode->Info->PixelFormat
    );

    fill_framebuffer_test(Gop);

    /* =====================================================
     * Boot info
     * ===================================================== */

    BootInfo = AllocatePool(sizeof(NTBLI));

    if (!BootInfo) {
        Print(L"BootInfo allocation failed\n");
        FreePool(KernelBuffer);
        return EFI_OUT_OF_RESOURCES;
    }

    BootInfo->version = 1;
    BootInfo->id = NULL;
    BootInfo->memory_size = 0;
    BootInfo->initrd_base = (VOID*)(UINTN)InitrdAddr;
    BootInfo->initrd_size = InitrdSize;

    BootInfo->framebuffer_base =
        (VOID*)(UINTN)Gop->Mode->FrameBufferBase;

    BootInfo->framebuffer_size =
        Gop->Mode->FrameBufferSize;

    BootInfo->width =
        Gop->Mode->Info->HorizontalResolution;

    BootInfo->height =
        Gop->Mode->Info->VerticalResolution;

    BootInfo->pixels_per_scan_line =
        Gop->Mode->Info->PixelsPerScanLine;

    Print(L"Framebuffer: %ux%u\n",
        BootInfo->width,
        BootInfo->height
    );

    /* =====================================================
     * Free temp kernel buffer BEFORE ExitBootServices
     * ===================================================== */

    FreePool(KernelBuffer);

    /* =====================================================
     * Get Memory Map
     * ===================================================== */

    Status = uefi_call_wrapper(
        BS->GetMemoryMap,
        5,
        &MemoryMapSize,
        NULL,
        &MapKey,
        &DescriptorSize,
        &DescriptorVersion
    );

    if (Status != EFI_BUFFER_TOO_SMALL) {
        Print(L"GetMemoryMap size failed: %r\n", Status);
        return Status;
    }

    MemoryMapSize += DescriptorSize * 8;

    Status = uefi_call_wrapper(
        BS->AllocatePool,
        3,
        EfiLoaderData,
        MemoryMapSize,
        (VOID**)&MemoryMap
    );

    if (EFI_ERROR(Status)) {
        Print(L"MemoryMap allocation failed\n");
        return Status;
    }

    Status = uefi_call_wrapper(
        BS->GetMemoryMap,
        5,
        &MemoryMapSize,
        MemoryMap,
        &MapKey,
        &DescriptorSize,
        &DescriptorVersion
    );

    if (EFI_ERROR(Status)) {
        Print(L"GetMemoryMap failed: %r\n", Status);
        return Status;
    }

    {
        UINT64 max_address = 0;
        UINTN descriptor_count = MemoryMapSize / DescriptorSize;

        for (UINTN index = 0; index < descriptor_count; index++) {
            EFI_MEMORY_DESCRIPTOR* descriptor =
                (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemoryMap + index * DescriptorSize);

            UINT64 end_address =
                descriptor->PhysicalStart + descriptor->NumberOfPages * PAGE_SIZE;

            if (end_address > max_address)
                max_address = end_address;
        }

        BootInfo->memory_size = (usize)max_address;
    }

    /* =====================================================
     * Exit Boot Services
     * ===================================================== */

    Status = uefi_call_wrapper(
        BS->ExitBootServices,
        2,
        ImageHandle,
        MapKey
    );

    if (EFI_ERROR(Status)) {
        return Status;
    }

    /* =====================================================
     * JUMP TO KERNEL
     * ===================================================== */

    KernelEntry(BootInfo);

    /* Should never return */

    for (;;) {
        __asm__ __volatile__("cli");
        __asm__ __volatile__("hlt");
    }

    return EFI_SUCCESS;
}
