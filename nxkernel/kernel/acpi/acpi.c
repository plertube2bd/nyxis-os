/*
 * acpi.c - ACPI S5 전원 끄기 (설계는 acpi.h 참고)
 *
 * 테이블은 모두 물리 주소로 서로를 가리킨다. HHDM 은 최소 4GiB 를 메모리 종류와 무관하게 매핑하므로
 * (paging.c) ACPI 테이블(보통 4GiB 아래의 ACPI reclaim/NVS 영역)은 phys_to_virt 로 읽을 수 있다.
 * 4GiB 위나 HHDM 범위 밖의 주소는 읽지 않고 실패로 처리한다.
 */

#include "kernel/acpi/acpi.h"
#include "kernel/paging/paging.h"
#include "console/outputs/printk.h"
#include "lowlevel.h"
#include "phys.h"
#include "memory.h"

#define SLP_EN (1U << 13)

static bool phys_readable(u64 phys, u64 len)
{
    u64 limit = paging_hhdm_limit();

    return (phys != 0 && phys < limit && len <= limit - phys) ? true : false;
}

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }

/* 물리 주소의 ACPI 테이블 헤더를 검증하고 가상 주소를 돌려준다 (서명/길이/체크섬) */
static const u8 *map_table(u64 phys, const char *sig)
{
    const u8 *t;
    u32 len;
    u32 i;
    u8 sum = 0;

    if (!phys_readable(phys, 36))
        return nNULL;
    t = (const u8 *)phys_to_virt(phys);
    if (sig && memcmp(t, sig, 4) != 0)
        return nNULL;
    len = rd32(t + 4);
    if (len < 36 || len > 0x100000U || !phys_readable(phys, len))
        return nNULL;
    for (i = 0; i < len; i++)
        sum = (u8)(sum + t[i]);
    return sum == 0 ? t : nNULL;
}

static const u8 *find_fadt(const u8 *rsdp)
{
    const u8 *sdt;
    u32 entry_size;
    u32 n;
    u32 i;

    if (memcmp(rsdp, "RSD PTR ", 8) != 0)
        return nNULL;

    if (rsdp[15] >= 2 && rd64(rsdp + 24) != 0) {
        sdt = map_table(rd64(rsdp + 24), "XSDT");
        entry_size = 8;
    } else {
        sdt = map_table((u64)rd32(rsdp + 16), "RSDT");
        entry_size = 4;
    }
    if (!sdt)
        return nNULL;

    n = (rd32(sdt + 4) - 36U) / entry_size;
    for (i = 0; i < n; i++) {
        u64 phys = entry_size == 8 ? rd64(sdt + 36 + i * 8U) : (u64)rd32(sdt + 36 + i * 4U);
        const u8 *t = map_table(phys, "FACP");

        if (t)
            return t;
    }
    return nNULL;
}

/* AML 정수 하나: BytePrefix(0x0A) n / ZeroOp / OneOp. 성공하면 바이트 수 */
static u32 aml_small_int(const u8 *p, const u8 *end, u32 *out)
{
    if (p >= end)
        return 0;
    if (p[0] == 0x0A && p + 1 < end) {
        *out = p[1];
        return 2;
    }
    if (p[0] == 0x00 || p[0] == 0x01) {
        *out = p[0];
        return 1;
    }
    return 0;
}

/* DSDT 에서 Name(_S5, Package(){a, b, ...}) 를 찾아 SLP_TYPa/b 를 얻는다 */
static bool find_s5(const u8 *dsdt, u32 *typ_a, u32 *typ_b)
{
    u32 len = rd32(dsdt + 4);
    const u8 *end = dsdt + len;
    const u8 *p;

    for (p = dsdt + 36; p + 8 < end; p++) {
        u32 pkglen_bytes;
        u32 used;

        if (memcmp(p, "_S5_", 4) != 0)
            continue;
        /* NameOp(0x08) 이 바로 앞에 있거나 루트 접두어 '\' 를 사이에 둔다 */
        if (!(p[-1] == 0x08 || (p[-1] == '\\' && p[-2] == 0x08)))
            continue;
        p += 4;
        if (*p != 0x12)                    /* PackageOp */
            continue;
        p++;
        pkglen_bytes = (u32)((*p >> 6) & 3U) + 1U;   /* PkgLength 인코딩 */
        p += pkglen_bytes;
        p++;                               /* NumElements */
        used = aml_small_int(p, end, typ_a);
        if (!used)
            return false;
        p += used;
        if (!aml_small_int(p, end, typ_b))
            *typ_b = *typ_a;
        return true;
    }
    return false;
}

static void spin_delay(u32 n)
{
    volatile u32 i;

    for (i = 0; i < n; i++)
        cpu_pause();
}

Nstatus acpi_power_off(const void *rsdp_virt)
{
    const u8 *fadt;
    const u8 *dsdt = nNULL;
    u32 pm1a;
    u32 pm1b;
    u32 typ_a = 0;
    u32 typ_b = 0;
    u32 flen;

    if (rsdp_virt && (fadt = find_fadt((const u8 *)rsdp_virt)) != nNULL) {
        flen = rd32(fadt + 4);
        if (flen >= 148 && rd64(fadt + 140) != 0)
            dsdt = map_table(rd64(fadt + 140), "DSDT");
        if (!dsdt)
            dsdt = map_table((u64)rd32(fadt + 40), "DSDT");
        pm1a = rd32(fadt + 64);
        pm1b = rd32(fadt + 68);

        if (dsdt && pm1a && find_s5(dsdt, &typ_a, &typ_b)) {
            u32 smi_cmd = rd32(fadt + 48);
            u8 acpi_enable = fadt[52];

            /* ACPI 모드로 (SCI_EN) */
            if (!(inw((u16)pm1a) & 1U) && smi_cmd && acpi_enable) {
                u32 tries;

                outb((u16)smi_cmd, acpi_enable);
                for (tries = 0; tries < 300 && !(inw((u16)pm1a) & 1U); tries++)
                    spin_delay(100000);
            }

            printk("acpi: entering S5 (SLP_TYPa=%u SLP_TYPb=%u)\n", typ_a, typ_b);
            outw((u16)pm1a, (u16)((typ_a << 10) | SLP_EN));
            if (pm1b)
                outw((u16)pm1b, (u16)((typ_b << 10) | SLP_EN));
            spin_delay(10000000);
        } else {
            printk("acpi: \\_S5 or PM1 control block not found\n");
        }
    } else {
        printk("acpi: no usable RSDP/FADT\n");
    }

    /* 마지막 수단: 에뮬레이터 고정 포트 (QEMU q35/piix, 옛 Bochs/QEMU) */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    spin_delay(10000000);
    return Nunsupported;
}
