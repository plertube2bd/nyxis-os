/*
 * multiboot2.c - Multiboot2 정보 -> NTBLI 변환 (설계는 multiboot2.h 참고)
 *
 * MBI 는 부트로더가 만든 "외부 입력" 이므로 모든 길이/오프셋을 검사한다.
 * (태그 크기 < 8, 총 크기 초과, 정렬 오류, 엔트리 크기 오류 등은 모두 거부)
 */

#include "kernel/boot/multiboot2.h"
#include "boot_info.h"
#include "memory.h"

/* Multiboot2 태그 종류 */
#define MB2_TAG_END        0U
#define MB2_TAG_MODULE     3U
#define MB2_TAG_MMAP       6U
#define MB2_TAG_FRAMEBUF   8U
#define MB2_TAG_ACPI_OLD   14U
#define MB2_TAG_ACPI_NEW   15U

/* Multiboot2 메모리 타입 */
#define MB2_MEM_AVAILABLE  1U
#define MB2_MEM_ACPI       3U
#define MB2_MEM_NVS        4U
#define MB2_MEM_BAD        5U

#define MB2_MIN_RSDP_SIZE  20U   /* ACPI 1.0 RSDP 크기 */

/* 정렬을 가정하지 않는 안전한 읽기 (MBI 는 8바이트 정렬이지만 방어적으로 memcpy) */
static u32 rd32(const u8 *p)
{
    u32 v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static u64 rd64(const u8 *p)
{
    u64 v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static u32 map_memory_type(u32 mb2_type)
{
    switch (mb2_type) {
    case MB2_MEM_AVAILABLE: return NTBLI_MEM_CONVENTIONAL;
    case MB2_MEM_ACPI:      return NTBLI_MEM_ACPI_RECLAIM;
    case MB2_MEM_NVS:       return NTBLI_MEM_ACPI_NVS;
    case MB2_MEM_BAD:       return NTBLI_MEM_UNUSABLE;
    default:                return NTBLI_MEM_RESERVED;
    }
}

/* MB2 메모리 맵 태그를 EFI 형식 배열로 변환. 성공하면 *count 에 항목 수. */
static Nstatus convert_mmap(const u8 *tag, u32 tag_size, NTBLI *out,
                            ntbli_memdesc_t *dst, usize cap_entries, usize *count)
{
    u32 entry_size;
    u32 n;
    u32 i;
    usize used = 0;

    if (tag_size < 16)
        return NinvalidFormat;

    entry_size = rd32(tag + 8);
    if (entry_size < 24)
        return NinvalidFormat;

    n = (tag_size - 16) / entry_size;

    for (i = 0; i < n; i++) {
        const u8 *e = tag + 16 + (usize)i * entry_size;
        u64 base = rd64(e);
        u64 len = rd64(e + 8);
        u32 type = map_memory_type(rd32(e + 16));
        u64 end;
        u64 start;

        if (len == 0 || base + len < base)
            continue;                       /* 크기 0 또는 오버플로 항목은 무시 */

        end = base + len;
        start = base;

        /* 사용 가능 메모리는 안쪽으로(4KiB 정렬), 그 외는 바깥쪽으로 정렬 */
        if (type == NTBLI_MEM_CONVENTIONAL) {
            start = (start + (NTBLI_PAGE_SIZE - 1)) & ~(NTBLI_PAGE_SIZE - 1);
            end &= ~(NTBLI_PAGE_SIZE - 1);
            if (end <= start)
                continue;
        } else {
            start &= ~(NTBLI_PAGE_SIZE - 1);
            end = (end + (NTBLI_PAGE_SIZE - 1)) & ~(NTBLI_PAGE_SIZE - 1);
            if (end < start)
                continue;
        }

        if (used >= cap_entries)
            break;                          /* 버퍼가 차면 나머지는 버린다 */

        dst[used].type = type;
        dst[used].pad = 0;
        dst[used].phys_start = start;
        dst[used].virt_start = 0;
        dst[used].num_pages = (end - start) / NTBLI_PAGE_SIZE;
        dst[used].attribute = 0;
        used++;

        if ((type == NTBLI_MEM_CONVENTIONAL || type == NTBLI_MEM_ACPI_RECLAIM ||
             type == NTBLI_MEM_ACPI_NVS) && end > out->memory_size)
            out->memory_size = end;
    }

    *count = used;
    return NSTATUS_OK;
}

static void convert_framebuffer(const u8 *tag, u32 tag_size, NTBLI *out)
{
    u64 addr;
    u32 pitch;
    u32 width;
    u32 height;
    u8 bpp;
    u8 fbtype;
    u8 red_pos;
    u8 green_pos;
    u8 blue_pos;

    if (tag_size < 38)
        return;

    addr = rd64(tag + 8);
    pitch = rd32(tag + 16);
    width = rd32(tag + 20);
    height = rd32(tag + 24);
    bpp = tag[28];
    fbtype = tag[29];
    red_pos = tag[32];
    green_pos = tag[34];
    blue_pos = tag[36];

    /* 직접 색상(type 1) 32bpp 만 지원. EGA 텍스트 등은 프레임버퍼 없음으로 취급 */
    if (fbtype != 1 || bpp != 32 || addr == 0 || width == 0 || height == 0)
        return;
    if ((pitch & 3U) != 0 || pitch < width * 4U)
        return;

    out->framebuffer_base = (void *)(usize)addr;
    out->framebuffer_size = (u64)pitch * height;
    out->width = width;
    out->height = height;
    out->pixels_per_scan_line = pitch / 4U;

    if (red_pos == 16 && green_pos == 8 && blue_pos == 0)
        out->pixel_format = NTBLI_PIXEL_BGR;        /* 메모리 순서 B,G,R,x */
    else if (red_pos == 0 && green_pos == 8 && blue_pos == 16)
        out->pixel_format = NTBLI_PIXEL_RGB;
    else
        out->pixel_format = NTBLI_PIXEL_BITMASK;    /* 미지원 (커널이 콘솔을 끔) */
}

Nstatus multiboot2_to_ntbli(u64 mbi_phys, NTBLI *out, void *memmap_buf, usize memmap_cap)
{
    const u8 *mbi;
    u32 total;
    u32 off;
    bool have_rsdp_new = false;
    usize count = 0;

    if (!out || !memmap_buf)
        return NinvalidArg;

    if (mbi_phys == 0 || (mbi_phys & 7UL) != 0 || mbi_phys >= 0x100000000UL)
        return NinvalidPointer;

    mbi = (const u8 *)(usize)mbi_phys;
    total = rd32(mbi);

    if (total < 16 || total > MB2_MAX_INFO_SIZE || mbi_phys + total > 0x100000000UL)
        return NinvalidFormat;

    memset(out, 0, sizeof(*out));
    out->magic = NTBLI_MAGIC;
    out->version = NTBLI_VERSION;
    out->size = (u32)sizeof(NTBLI);
    out->pixel_format = NTBLI_PIXEL_BLTONLY;

    off = 8;
    while (off + 8 <= total) {
        const u8 *tag = mbi + off;
        u32 type = rd32(tag);
        u32 size = rd32(tag + 4);

        if (type == MB2_TAG_END)
            break;
        if (size < 8 || size > total - off)
            return NinvalidFormat;

        switch (type) {
        case MB2_TAG_MODULE:
            /* 첫 번째 모듈을 initrd 로 사용 */
            if (size >= 16 && out->initrd_base == nNULL) {
                u32 mstart = rd32(tag + 8);
                u32 mend = rd32(tag + 12);

                if (mend > mstart) {
                    out->initrd_base = (void *)(usize)mstart;
                    out->initrd_size = (u64)(mend - mstart);
                }
            }
            break;

        case MB2_TAG_MMAP:
            if (convert_mmap(tag, size, out, (ntbli_memdesc_t *)memmap_buf,
                             memmap_cap / sizeof(ntbli_memdesc_t), &count) != NSTATUS_OK)
                return NinvalidFormat;
            out->memmap_base = memmap_buf;
            out->memmap_size = (u64)count * sizeof(ntbli_memdesc_t);
            out->memmap_desc_size = sizeof(ntbli_memdesc_t);
            out->memmap_desc_version = 1;
            break;

        case MB2_TAG_FRAMEBUF:
            convert_framebuffer(tag, size, out);
            break;

        case MB2_TAG_ACPI_NEW:
            if (size >= 8 + MB2_MIN_RSDP_SIZE) {
                out->Rsdp = (void *)(usize)(mbi_phys + off + 8);
                have_rsdp_new = true;
            }
            break;

        case MB2_TAG_ACPI_OLD:
            if (!have_rsdp_new && size >= 8 + MB2_MIN_RSDP_SIZE)
                out->Rsdp = (void *)(usize)(mbi_phys + off + 8);
            break;

        default:
            break;
        }

        off += (size + 7U) & ~7U;           /* 태그는 8바이트 정렬 */
    }

    /* 메모리 맵이 없으면 사용할 수 있는 메모리를 알 수 없으므로 부팅 불가 */
    if (out->memmap_base == nNULL || out->memory_size == 0)
        return NinvalidFormat;

    return NSTATUS_OK;
}
