/* NYxis Tiny Bootloader : NYTB 3
 *
 * ESP(EFI System Partition)의 루트에서 kernel.elf 와 (선택) initrd.img 를 읽어
 * 메모리에 올리고, 부트 정보 구조체(NTBLI)를 만들어 커널로 점프하는 UEFI 앱이다.
 *
 * 디렉터리 구조 (ESP 기준):
 *   /EFI/BOOT/BOOTX64.EFI   (이 파일)
 *   /kernel.elf             (필수: x86_64 정적 ELF64 실행 파일, ET_EXEC)
 *   /initrd.img             (선택: FAT16 이미지)
 *
 * 커널 진입 규약: rdi = NTBLI 포인터 (System V ABI), 인터럽트 비활성, 롱 모드.
 *
 * [수정 이력 요약]
 *  1. initrd 주소 그림자 변수 버그: 블록 안에서 InitrdAddr 를 다시 선언해서 바깥
 *     변수가 항상 0 으로 남았다. 그래서 커널은 "initrd 없음" 으로 인식했다.
 *  2. NTBLI 정의를 커널과 공유하면서 크기/오프셋 불일치 제거 (types.h 참고).
 *  3. ELF 로더 보안 강화: 모든 오프셋/크기를 검사 (기존에는 e_phoff, e_phnum, p_offset,
 *     p_filesz 를 그대로 믿어서 악의적/손상된 kernel.elf 로 임의 메모리를 읽고 쓸 수 있었다).
 *     세그먼트를 하나씩 AllocateAddress 하던 것을 전체 범위 한 번 할당으로 바꿔
 *     같은 페이지를 공유하는 세그먼트에서 실패하던 문제도 해결.
 *  4. ELF32 경로 삭제: 64비트 UEFI 앱이 32비트 커널로 점프하면 롱 모드 상태에서 실행되어
 *     반드시 크래시한다.
 *  5. UEFI 메모리 맵을 커널에 전달, ExitBootServices 실패 시 재시도 (기존: 1회 실패하면 종료).
 *     memory_size 는 MMIO 를 제외한 RAM 의 끝 주소로 계산.
 *  6. GOP 가 없거나 BltOnly(직접 프레임버퍼 없음)여도 부팅을 계속한다.
 *     (기존: GOP 실패 시 부팅 중단, BltOnly 인데도 FrameBufferBase 를 그대로 전달)
 *  7. ACPI RSDP 전달 (기존: 항상 NULL).
 *  8. Print 서식 오류 수정 (%p 는 gnu-efi 미지원, %u 로 64비트 값 출력).
 *  9. 워치독 타이머 해제, ExitBootServices 직후 인터럽트 차단(cli), 파일 크기 상한/전체 읽기 확인.
 * 10. 디버그 테스트 패턴은 -DNYTB_DEBUG 일 때만 화면에 그린다 (기존: 항상 그림).
 * 11. C89 호환 (혼합 선언, for 루프 선언, 라인 주석 제거).
 */

#include <efi.h>
#include <efilib.h>
#include <efiprot.h>
#include <nyxis.h>

/* =========================================================
 * ELF64 정의
 * ========================================================= */

#define ELF_MAGIC     0x464C457FU

#define ET_EXEC       2
#define EM_X86_64     62
#define PT_LOAD       1
#define PF_X          1

#define ELFCLASS64    2
#define ELFDATA2LSB   1

#define NYTB_PAGE_SIZE 4096UL

/* 커널은 1MiB ~ 1GiB 사이의 물리 주소에만 적재할 수 있다 (커널이 항등 매핑을 전제로 링크됨) */
#define KERNEL_MIN_ADDR  0x100000UL
#define KERNEL_MAX_ADDR  0x40000000UL

/* 파일 크기 상한 (비정상적으로 큰 파일로 메모리를 소진시키는 것 방지) */
#define KERNEL_MAX_FILE  (64UL * 1024UL * 1024UL)
#define INITRD_MAX_FILE  (512UL * 1024UL * 1024UL)

#define ELF_MAX_PHNUM    32

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

NX_STATIC_ASSERT(elf64_header_size, sizeof(ELF64_HEADER) == 64);
NX_STATIC_ASSERT(elf64_phdr_size, sizeof(ELF64_PROGRAM_HEADER) == 56);

/* 커널 진입점: System V ABI (rdi = NTBLI*). UEFI 의 MS ABI 와 다르므로 명시한다. */
typedef void (__attribute__((sysv_abi)) *kernel_entry_t)(NTBLI *);

/* ACPI GUID (UEFI 규격) */
static EFI_GUID g_acpi20_guid = ACPI_20_TABLE_GUID;
static EFI_GUID g_acpi10_guid = ACPI_TABLE_GUID;

/* =========================================================
 * 유틸
 * ========================================================= */

static UINT64 align_down(UINT64 v, UINT64 a)
{
    return v & ~(a - 1);
}

/* 호출 전에 v + a 가 오버플로하지 않음을 호출자가 보장해야 한다 */
static UINT64 align_up(UINT64 v, UINT64 a)
{
    return (v + a - 1) & ~(a - 1);
}

/* =========================================================
 * 파일 읽기
 * ========================================================= */

/* 파일 크기를 얻는다 */
static EFI_STATUS get_file_size(EFI_FILE_HANDLE file, UINT64 *size)
{
    EFI_STATUS Status;
    EFI_FILE_INFO *FileInfo;
    UINTN InfoSize = 0;

    Status = uefi_call_wrapper(file->GetInfo, 4, file, &GenericFileInfo, &InfoSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL)
        return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;

    FileInfo = AllocatePool(InfoSize);
    if (!FileInfo)
        return EFI_OUT_OF_RESOURCES;

    Status = uefi_call_wrapper(file->GetInfo, 4, file, &GenericFileInfo, &InfoSize, FileInfo);
    if (!EFI_ERROR(Status))
        *size = FileInfo->FileSize;

    FreePool(FileInfo);
    return Status;
}

/* 파일 전체를 pool 메모리로 읽는다. 읽은 바이트 수가 파일 크기와 다르면 실패. */
static EFI_STATUS read_file(EFI_FILE_HANDLE file, UINT64 max_size, VOID **buffer, UINTN *size)
{
    EFI_STATUS Status;
    UINT64 file_size = 0;
    UINTN read_size;
    VOID *data;

    Status = get_file_size(file, &file_size);
    if (EFI_ERROR(Status))
        return Status;

    if (file_size == 0 || file_size > max_size)
        return EFI_BAD_BUFFER_SIZE;

    data = AllocatePool((UINTN)file_size);
    if (!data)
        return EFI_OUT_OF_RESOURCES;

    read_size = (UINTN)file_size;
    Status = uefi_call_wrapper(file->Read, 3, file, &read_size, data);
    if (EFI_ERROR(Status) || read_size != (UINTN)file_size) {
        FreePool(data);
        return EFI_ERROR(Status) ? Status : EFI_END_OF_FILE;
    }

    *buffer = data;
    *size = read_size;
    return EFI_SUCCESS;
}

/*
 * initrd 를 "페이지 단위 메모리" 로 직접 읽는다. (pool 로 읽고 다시 복사하면 메모리가 2배 필요)
 * 성공하면 *base 에 물리 주소, *size 에 바이트 수.
 */
static EFI_STATUS load_initrd(EFI_FILE_HANDLE file, EFI_PHYSICAL_ADDRESS *base, UINTN *size)
{
    EFI_STATUS Status;
    UINT64 file_size = 0;
    UINTN pages;
    UINTN read_size;
    EFI_PHYSICAL_ADDRESS addr = 0;

    Status = get_file_size(file, &file_size);
    if (EFI_ERROR(Status))
        return Status;

    if (file_size == 0 || file_size > INITRD_MAX_FILE)
        return EFI_BAD_BUFFER_SIZE;

    pages = (UINTN)((file_size + NYTB_PAGE_SIZE - 1) / NYTB_PAGE_SIZE);

    Status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(Status))
        return Status;

    read_size = (UINTN)file_size;
    Status = uefi_call_wrapper(file->Read, 3, file, &read_size, (VOID *)(UINTN)addr);
    if (EFI_ERROR(Status) || read_size != (UINTN)file_size) {
        uefi_call_wrapper(BS->FreePages, 2, addr, pages);
        return EFI_ERROR(Status) ? Status : EFI_END_OF_FILE;
    }

    *base = addr;
    *size = read_size;
    return EFI_SUCCESS;
}

/* =========================================================
 * ELF 로드
 * ========================================================= */

/*
 * ELF64 kernel 을 검증하고 p_paddr 위치에 적재한다.
 * 모든 값은 신뢰하지 않고 범위/오버플로를 검사한다.
 */
static EFI_STATUS load_elf_kernel(
    VOID *elf_buffer,
    UINTN elf_size,
    kernel_entry_t *entry_point
) {
    const ELF64_HEADER *hdr = (const ELF64_HEADER *)elf_buffer;
    const UINT8 *base = (const UINT8 *)elf_buffer;
    UINT64 lo = 0xFFFFFFFFFFFFFFFFUL;
    UINT64 hi = 0;
    UINTN loads = 0;
    BOOLEAN entry_ok = FALSE;
    EFI_PHYSICAL_ADDRESS addr;
    UINTN pages;
    EFI_STATUS Status;
    UINTN i;

    if (elf_size < sizeof(ELF64_HEADER))
        return EFI_INVALID_PARAMETER;

    if (hdr->e_magic != ELF_MAGIC || hdr->e_class != ELFCLASS64 ||
        hdr->e_data != ELFDATA2LSB || hdr->e_type != ET_EXEC ||
        hdr->e_machine != EM_X86_64)
        return EFI_UNSUPPORTED;

    if (hdr->e_phentsize != sizeof(ELF64_PROGRAM_HEADER) ||
        hdr->e_phnum == 0 || hdr->e_phnum > ELF_MAX_PHNUM)
        return EFI_INVALID_PARAMETER;

    /* 프로그램 헤더 테이블이 파일 안에 있는지 (오버플로 없는 뺄셈 비교) */
    if (hdr->e_phoff > elf_size ||
        (UINT64)hdr->e_phnum * hdr->e_phentsize > elf_size - hdr->e_phoff)
        return EFI_INVALID_PARAMETER;

    /* 1차 패스: 모든 PT_LOAD 검증 + 전체 주소 범위 계산 */
    for (i = 0; i < hdr->e_phnum; i++) {
        const ELF64_PROGRAM_HEADER *ph =
            (const ELF64_PROGRAM_HEADER *)(const VOID *)(base + hdr->e_phoff + i * sizeof(ELF64_PROGRAM_HEADER));
        UINT64 end;

        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_memsz == 0 || ph->p_filesz > ph->p_memsz)
            return EFI_INVALID_PARAMETER;

        /* 파일 내 데이터 범위 */
        if (ph->p_offset > elf_size || ph->p_filesz > elf_size - ph->p_offset)
            return EFI_INVALID_PARAMETER;

        /* 적재 주소 범위 */
        if (ph->p_paddr < KERNEL_MIN_ADDR || ph->p_paddr >= KERNEL_MAX_ADDR ||
            ph->p_memsz > KERNEL_MAX_ADDR - ph->p_paddr)
            return EFI_INVALID_PARAMETER;

        end = ph->p_paddr + ph->p_memsz;

        if (align_down(ph->p_paddr, NYTB_PAGE_SIZE) < lo)
            lo = align_down(ph->p_paddr, NYTB_PAGE_SIZE);
        if (align_up(end, NYTB_PAGE_SIZE) > hi)
            hi = align_up(end, NYTB_PAGE_SIZE);

        /* 진입점은 실행 가능한 PT_LOAD 안에 있어야 한다 */
        if ((ph->p_flags & PF_X) && hdr->e_entry >= ph->p_vaddr &&
            hdr->e_entry - ph->p_vaddr < ph->p_memsz &&
            ph->p_vaddr == ph->p_paddr)
            entry_ok = TRUE;

        loads++;
    }

    if (loads == 0 || !entry_ok || hi <= lo)
        return EFI_INVALID_PARAMETER;

    /* 전체 범위를 한 번에 할당 (같은 페이지를 공유하는 세그먼트가 있어도 안전) */
    addr = lo;
    pages = (UINTN)((hi - lo) / NYTB_PAGE_SIZE);

    Status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(Status))
        return Status;

    SetMem((VOID *)(UINTN)lo, (UINTN)(hi - lo), 0);

    /* 2차 패스: 파일 데이터 복사 (범위는 1차 패스에서 이미 검증됨) */
    for (i = 0; i < hdr->e_phnum; i++) {
        const ELF64_PROGRAM_HEADER *ph =
            (const ELF64_PROGRAM_HEADER *)(const VOID *)(base + hdr->e_phoff + i * sizeof(ELF64_PROGRAM_HEADER));

        if (ph->p_type != PT_LOAD || ph->p_filesz == 0)
            continue;

        CopyMem((VOID *)(UINTN)ph->p_paddr,
                (VOID *)(base + ph->p_offset),
                (UINTN)ph->p_filesz);
    }

    *entry_point = (kernel_entry_t)(UINTN)hdr->e_entry;

    Print(L"Loaded ELF64 kernel: 0x%lx - 0x%lx\n", lo, hi);

    return EFI_SUCCESS;
}

/* =========================================================
 * GOP (프레임버퍼)
 * ========================================================= */

/*
 * 사용할 GOP 모드 선택: 직접 프레임버퍼 접근이 가능한 모드(BltOnly 제외) 중
 * 1024x768 > 800x600 > 현재 모드 순으로 선호한다.
 * 적당한 모드가 없으면 GOP 의 현재 모드를 반환한다.
 */
static UINT32 get_preferred_gop_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop)
{
    UINT32 selected = Gop->Mode->Mode;
    BOOLEAN have_800 = FALSE;
    UINT32 mode;

    for (mode = 0; mode < Gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info = NULL;
        UINTN InfoSize = 0;
        EFI_STATUS Status;
        UINT32 w;
        UINT32 h;
        UINT32 fmt;

        Status = uefi_call_wrapper(Gop->QueryMode, 4, Gop, mode, &InfoSize, &Info);
        if (EFI_ERROR(Status) || !Info)
            continue;

        w = Info->HorizontalResolution;
        h = Info->VerticalResolution;
        fmt = Info->PixelFormat;
        FreePool(Info);

        if (fmt == PixelBltOnly)
            continue;

        if (w == 1024 && h == 768)
            return mode;

        if (w == 800 && h == 600 && !have_800) {
            selected = mode;
            have_800 = TRUE;
        }
    }

    return selected;
}

/* GOP 를 설정하고 NTBLI 프레임버퍼 필드를 채운다. GOP 가 없어도 실패하지 않는다. */
static VOID setup_framebuffer(NTBLI *bi)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    EFI_STATUS Status;
    UINT32 preferred;

    bi->framebuffer_base = NULL;
    bi->framebuffer_size = 0;
    bi->width = 0;
    bi->height = 0;
    bi->pixels_per_scan_line = 0;
    bi->pixel_format = NTBLI_PIXEL_BLTONLY;

    Status = uefi_call_wrapper(BS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (VOID **)&Gop);
    if (EFI_ERROR(Status) || !Gop || !Gop->Mode) {
        Print(L"No GOP (%r): continuing without framebuffer\n", Status);
        return;
    }

    preferred = get_preferred_gop_mode(Gop);
    if (preferred != Gop->Mode->Mode) {
        Status = uefi_call_wrapper(Gop->SetMode, 2, Gop, preferred);
        if (EFI_ERROR(Status))
            Print(L"GOP SetMode failed (%r): keeping current mode\n", Status);
    }

    Info = Gop->Mode->Info;
    if (!Info || Info->PixelFormat == PixelBltOnly || Info->PixelFormat == PixelFormatMax ||
        Gop->Mode->FrameBufferBase == 0) {
        Print(L"GOP has no linear framebuffer: continuing without framebuffer\n");
        return;
    }

    bi->framebuffer_base = (VOID *)(UINTN)Gop->Mode->FrameBufferBase;
    bi->framebuffer_size = Gop->Mode->FrameBufferSize;
    bi->width = Info->HorizontalResolution;
    bi->height = Info->VerticalResolution;
    bi->pixels_per_scan_line = Info->PixelsPerScanLine;
    bi->pixel_format = (UINT32)Info->PixelFormat;

    Print(L"Framebuffer: %ux%u stride=%u format=%u base=0x%lx size=0x%lx\n",
          bi->width, bi->height, bi->pixels_per_scan_line, bi->pixel_format,
          (UINT64)Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);

#ifdef NYTB_DEBUG
    /* 디버그 빌드에서만: 화면 좌상단에 테스트 패턴 (프레임버퍼 접근 확인용) */
    if (Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ||
        Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
        UINT32 *fb = (UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
        UINT32 rows = (Info->VerticalResolution < 64) ? Info->VerticalResolution : 64;
        UINT32 cols = (Info->HorizontalResolution < 256) ? Info->HorizontalResolution : 256;
        UINT32 x;
        UINT32 y;

        for (y = 0; y < rows; y++)
            for (x = 0; x < cols; x++)
                fb[(UINTN)y * Info->PixelsPerScanLine + x] = 0x00FF0000U | ((y << 8) ^ x);
    }
#endif
}

/* =========================================================
 * ACPI
 * ========================================================= */

/* ACPI 2.0 RSDP 를 우선, 없으면 1.0 RSDP 를 찾는다 */
static VOID *find_rsdp(EFI_SYSTEM_TABLE *SystemTable)
{
    VOID *rsdp10 = NULL;
    UINTN i;

    for (i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &SystemTable->ConfigurationTable[i];

        if (CompareMem(&t->VendorGuid, &g_acpi20_guid, sizeof(EFI_GUID)) == 0)
            return t->VendorTable;
        if (CompareMem(&t->VendorGuid, &g_acpi10_guid, sizeof(EFI_GUID)) == 0)
            rsdp10 = t->VendorTable;
    }

    return rsdp10;
}

/* =========================================================
 * 메모리 맵 + ExitBootServices
 * ========================================================= */

/* RAM 으로 사용되는 메모리 타입인지 (MMIO/예약/사용불가 제외) */
static BOOLEAN is_ram_type(UINT32 type)
{
    switch (type) {
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
    case EfiConventionalMemory:
    case EfiACPIReclaimMemory:
    case EfiACPIMemoryNVS:
        return TRUE;
    default:
        return FALSE;
    }
}

/*
 * 메모리 맵을 얻고 ExitBootServices 를 호출한다.
 * 맵을 얻는 사이에 맵이 바뀌면 ExitBootServices 가 EFI_INVALID_PARAMETER 로 실패하므로
 * 최신 맵 키로 여러 번 재시도한다. 이 함수 안에서는 Print 를 호출하지 않는다.
 * (재할당은 BUFFER_TOO_SMALL 일 때만 한다)
 */
static EFI_STATUS exit_boot_services_with_map(EFI_HANDLE ImageHandle, NTBLI *bi)
{
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    UINTN alloc_size;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    EFI_STATUS Status;
    UINTN attempt;

    /* 필요한 크기 조회 */
    Status = uefi_call_wrapper(BS->GetMemoryMap, 5, &map_size, NULL, &map_key, &desc_size, &desc_version);
    if (Status != EFI_BUFFER_TOO_SMALL)
        return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;

    /* 버퍼 할당 자체가 맵에 항목을 추가할 수 있으므로 여유분(16개)을 둔다 */
    alloc_size = map_size + desc_size * 16;
    Status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, alloc_size, (VOID **)&map);
    if (EFI_ERROR(Status))
        return Status;

    for (attempt = 0; attempt < 8; attempt++) {
        map_size = alloc_size;
        Status = uefi_call_wrapper(BS->GetMemoryMap, 5, &map_size, map, &map_key, &desc_size, &desc_version);

        if (Status == EFI_BUFFER_TOO_SMALL) {
            uefi_call_wrapper(BS->FreePool, 1, map);
            map = NULL;
            alloc_size = map_size + desc_size * 16;
            Status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, alloc_size, (VOID **)&map);
            if (EFI_ERROR(Status))
                return Status;
            continue;
        }
        if (EFI_ERROR(Status))
            return Status;

        Status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
        if (!EFI_ERROR(Status))
            break;
        /* 맵 키가 오래되었다면 다시 GetMemoryMap 부터 */
    }

    if (EFI_ERROR(Status))
        return Status;

    /* --- 여기서부터 부트 서비스 사용 불가. 메모리 쓰기만 한다 --- */
    {
        UINT64 max_ram_end = 0;
        UINTN count = map_size / desc_size;
        UINTN idx;

        for (idx = 0; idx < count; idx++) {
            const EFI_MEMORY_DESCRIPTOR *d =
                (const EFI_MEMORY_DESCRIPTOR *)(const VOID *)((const UINT8 *)map + idx * desc_size);
            UINT64 end;

            if (!is_ram_type(d->Type))
                continue;
            if (d->NumberOfPages > (0x1000000000000UL / NYTB_PAGE_SIZE))
                continue;

            end = d->PhysicalStart + d->NumberOfPages * NYTB_PAGE_SIZE;
            if (end > max_ram_end)
                max_ram_end = end;
        }

        bi->memory_size = max_ram_end;
        bi->memmap_base = map;
        bi->memmap_size = map_size;
        bi->memmap_desc_size = desc_size;
        bi->memmap_desc_version = desc_version;
    }

    return EFI_SUCCESS;
}

/* =========================================================
 * EFI MAIN
 * ========================================================= */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS
EFIAPI
efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE *LoadedImage;
    EFI_FILE_IO_INTERFACE *FileSystem;
    EFI_FILE_HANDLE RootDir = NULL;
    EFI_FILE_HANDLE KernelFile = NULL;
    EFI_FILE_HANDLE InitrdFile = NULL;
    VOID *KernelBuffer = NULL;
    UINTN KernelSize = 0;
    EFI_PHYSICAL_ADDRESS InitrdAddr = 0;
    UINTN InitrdSize = 0;
    kernel_entry_t KernelEntry = NULL;
    NTBLI *BootInfo = NULL;

    InitializeLib(ImageHandle, SystemTable);

    Print(L"Nyxis Bootloader v1.1\n");
    Print(L"=====================\n");

    /* 펌웨어 워치독(기본 5분) 해제 */
    uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0, 0, NULL);

    /* ---- 부트 디바이스의 파일시스템 열기 ---- */
    Status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) {
        Print(L"HandleProtocol failed: %r\n", Status);
        return Status;
    }

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, LoadedImage->DeviceHandle, &FileSystemProtocol, (VOID **)&FileSystem);
    if (EFI_ERROR(Status)) {
        Print(L"FileSystemProtocol failed: %r\n", Status);
        return Status;
    }

    Status = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &RootDir);
    if (EFI_ERROR(Status)) {
        Print(L"OpenVolume failed: %r\n", Status);
        return Status;
    }

    /* ---- kernel.elf 읽기 ---- */
    Print(L"Opening kernel.elf...\n");
    Status = uefi_call_wrapper(RootDir->Open, 5, RootDir, &KernelFile, L"kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to open kernel.elf: %r\n", Status);
        uefi_call_wrapper(RootDir->Close, 1, RootDir);
        return Status;
    }

    Status = read_file(KernelFile, KERNEL_MAX_FILE, &KernelBuffer, &KernelSize);
    uefi_call_wrapper(KernelFile->Close, 1, KernelFile);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to read kernel: %r\n", Status);
        uefi_call_wrapper(RootDir->Close, 1, RootDir);
        return Status;
    }
    Print(L"Kernel size: %lu bytes\n", (UINT64)KernelSize);

    /* ---- initrd.img 읽기 (선택) ---- */
    Print(L"Opening initrd.img...\n");
    Status = uefi_call_wrapper(RootDir->Open, 5, RootDir, &InitrdFile, L"initrd.img", EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(Status)) {
        Status = load_initrd(InitrdFile, &InitrdAddr, &InitrdSize);
        uefi_call_wrapper(InitrdFile->Close, 1, InitrdFile);

        if (EFI_ERROR(Status)) {
            Print(L"Failed to load initrd.img (%r): continuing without initrd\n", Status);
            InitrdAddr = 0;
            InitrdSize = 0;
        } else {
            Print(L"Loaded initrd.img, %lu bytes at 0x%lx\n", (UINT64)InitrdSize, (UINT64)InitrdAddr);
        }
    } else {
        Print(L"No initrd.img found, continuing without initrd\n");
    }

    uefi_call_wrapper(RootDir->Close, 1, RootDir);

    /* ---- ELF 적재 ---- */
    Status = load_elf_kernel(KernelBuffer, KernelSize, &KernelEntry);
    FreePool(KernelBuffer);          /* 임시 버퍼는 더 필요 없다 (ExitBootServices 전에 해제) */
    KernelBuffer = NULL;
    if (EFI_ERROR(Status)) {
        Print(L"ELF loading failed: %r\n", Status);
        return Status;
    }

    Print(L"Kernel entry: 0x%lx\n", (UINT64)(UINTN)KernelEntry);

    /* ---- 부트 정보 ---- */
    BootInfo = AllocatePool(sizeof(NTBLI));
    if (!BootInfo) {
        Print(L"BootInfo allocation failed\n");
        return EFI_OUT_OF_RESOURCES;
    }
    ZeroMem(BootInfo, sizeof(NTBLI));

    BootInfo->magic = NTBLI_MAGIC;
    BootInfo->version = NTBLI_VERSION;
    BootInfo->size = (UINT32)sizeof(NTBLI);
    BootInfo->initrd_base = InitrdSize ? (VOID *)(UINTN)InitrdAddr : NULL;
    BootInfo->initrd_size = InitrdSize;
    BootInfo->Rsdp = find_rsdp(SystemTable);

    setup_framebuffer(BootInfo);

    Print(L"Exiting boot services...\n");

    /* ---- 메모리 맵 확보 + ExitBootServices (이후 Print 금지) ---- */
    Status = exit_boot_services_with_map(ImageHandle, BootInfo);
    if (EFI_ERROR(Status)) {
        Print(L"ExitBootServices failed: %r\n", Status);
        return Status;
    }

    /* 커널이 자체 IDT 를 갖추기 전에 펌웨어 인터럽트가 끼어들지 않도록 차단 */
    __asm__ __volatile__("cli");

    KernelEntry(BootInfo);

    /* Should never return */
    for (;;) {
        __asm__ __volatile__("cli");
        __asm__ __volatile__("hlt");
    }

    return EFI_SUCCESS;
}
