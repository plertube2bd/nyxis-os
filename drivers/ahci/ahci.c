/*
 * ahci.c - AHCI(SATA) 최소 드라이버 (폴링, DMA 읽기/쓰기/FLUSH, 연결된 디스크 전부)
 *
 * 디스크 i (포트 번호 순) 는 디스크 번호 AHCI_DISKNO_BASE + i 로 파일시스템 콜백(ahci_disk_read/write)에 쓰인다.
 *
 * [수정 이력 요약]  (기존 코드의 실제 버그들)
 *  - PCI 버스 순회 루프가 'for (u8 bus = 0; bus < 256; bus++)' 였다.
 *    u8 은 256 이 될 수 없으므로 "AHCI 컨트롤러가 없으면 무한 루프" 였다.
 *  - 하드웨어 응답을 기다리는 모든 루프에 상한(타임아웃)이 없었다.
 *    (ahci_stop_cmd, ahci_start_cmd, TFD BSY/DRQ 대기) -> 장치가 응답하지 않으면 부팅 정지.
 *  - PRDT 의 DBC 필드는 22비트(최대 4MiB)인데 섹터 수 검사가 없어서 큰 값이면
 *    필드가 넘쳐 DMA 가 잘못된 길이로 수행될 수 있었다. 또 LBA 는 48비트까지만 유효하다.
 *  - 버퍼 주소가 컨트롤러가 접근 가능한 범위(64비트 미지원이면 4GiB 이내)인지 검사하지 않음.
 *  - PCI 명령 레지스터의 Memory Space / Bus Master 를 켜지 않고 DMA 를 시작함.
 *  - BAR5 를 64비트 BAR 로 해석하려던 잘못된 코드 삭제 (AHCI 의 BAR5 는 항상 32비트).
 *  - 비트필드 FIS 구조체 제거 (ahci.h 참고), C89 호환.
 */

#include "ahci.h"
#include "lowlevel.h"
#include "nyxis.h"
#include "memory.h"
#include "phys.h"
#include "console/outputs/printk.h"

/* 상태 대기 반복 횟수 상한 (하드웨어가 응답하지 않아도 커널이 멈추지 않도록) */
#define AHCI_SPIN_LIMIT 2000000UL

/* AHCI controller base address */
static HBA_MEM *ahci_base = nNULL;

/* 컨트롤러가 접근 가능한 최대 DMA 주소 (S64A 미지원이면 4GiB) */
static bool ahci_dma64 = false;

/* Command list, FIS, and command tables for each port */
static u8 ahci_command_list[AHCI_MAX_PORTS][1024] __attribute__((aligned(1024)));
static u8 ahci_fis[AHCI_MAX_PORTS][256] __attribute__((aligned(256)));
static u8 ahci_cmd_tables[AHCI_MAX_PORTS][AHCI_MAX_CMD_SLOTS][256] __attribute__((aligned(256)));

/* ------------------------------------------------------------------ */
/* PCI 설정 공간 접근 (레거시 포트 0xCF8/0xCFC)                        */
/* ------------------------------------------------------------------ */

static u32 pci_address(u32 bus, u32 device, u32 function, u32 offset)
{
    return (1UL << 31) |
           (bus << 16) |
           (device << 11) |
           (function << 8) |
           (offset & 0xFCU);
}

static u32 pci_read_config(u32 bus, u32 device, u32 function, u32 offset)
{
    outl(0xCF8, pci_address(bus, device, function, offset));
    return inl(0xCFC);
}

static void pci_write_config(u32 bus, u32 device, u32 function, u32 offset, u32 value)
{
    outl(0xCF8, pci_address(bus, device, function, offset));
    outl(0xCFC, value);
}

static u32 pci_get_bar5(u32 bus, u32 device, u32 function)
{
    u32 bar = pci_read_config(bus, device, function, 0x24);

    /* I/O BAR 는 AHCI 의 ABAR 가 될 수 없다 */
    if (bar & 0x1U)
        return 0;

    return bar & 0xFFFFFFF0U;
}

static bool pci_is_ahci_device(u32 bus, u32 device, u32 function)
{
    u32 vendor_device = pci_read_config(bus, device, function, 0x0);
    u32 class_info;
    u8 prog_if;
    u8 subclass;
    u8 baseclass;

    if ((vendor_device & 0xFFFFU) == 0xFFFFU)
        return false;

    class_info = pci_read_config(bus, device, function, 0x08);
    prog_if   = (u8)((class_info >> 8) & 0xFFU);
    subclass  = (u8)((class_info >> 16) & 0xFFU);
    baseclass = (u8)((class_info >> 24) & 0xFFU);

    return (baseclass == 0x01 && subclass == 0x06 && prog_if == 0x01) ? true : false;
}

static Nstatus ahci_find_controller(void)
{
    u32 bus;
    u32 device;
    u32 function;

    /* bus 는 u32 로 선언해야 256 미만 조건이 성립한다 (u8 이면 무한 루프) */
    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            u32 vendor = pci_read_config(bus, device, 0, 0x0);
            u32 header;
            u32 functions;

            /* 함수 0 이 없으면 이 슬롯에는 장치가 없다 */
            if ((vendor & 0xFFFFU) == 0xFFFFU)
                continue;

            /* 헤더 타입의 bit7 = 멀티펑션 장치 */
            header = pci_read_config(bus, device, 0, 0x0C);
            functions = ((header >> 16) & 0x80U) ? 8U : 1U;

            for (function = 0; function < functions; function++) {
                u32 abar;
                u32 command;

                if (!pci_is_ahci_device(bus, device, function))
                    continue;

                abar = pci_get_bar5(bus, device, function);
                if (!abar)
                    continue;

                /* Memory Space(bit1) + Bus Master(bit2) 활성화. 없으면 DMA 가 동작하지 않는다 */
                command = pci_read_config(bus, device, function, 0x04);
                if ((command & 0x6U) != 0x6U) {
                    command |= 0x6U;
                    pci_write_config(bus, device, function, 0x04, command);
                }

                ahci_base = (HBA_MEM *)phys_to_virt((u64)abar);   /* MMIO 는 HHDM 을 통해 접근 */
                return NSTATUS_OK;
            }
        }
    }

    return NdeviceMissing;
}

/* ------------------------------------------------------------------ */
/* 포트 제어                                                           */
/* ------------------------------------------------------------------ */

static Nstatus ahci_stop_cmd(HBA_PORT *Port)
{
    u32 spin;

    Port->cmd &= ~HBA_PX_CMD_ST;
    Port->cmd &= ~HBA_PX_CMD_FRE;

    for (spin = 0; spin < AHCI_SPIN_LIMIT; spin++) {
        if (!(Port->cmd & (HBA_PX_CMD_FR | HBA_PX_CMD_CR)))
            return NSTATUS_OK;
        cpu_pause();
    }

    return Ntimeout;
}

static Nstatus ahci_start_cmd(HBA_PORT *Port)
{
    u32 spin;

    for (spin = 0; spin < AHCI_SPIN_LIMIT; spin++) {
        if (!(Port->cmd & HBA_PX_CMD_CR))
            break;
        cpu_pause();
    }
    if (Port->cmd & HBA_PX_CMD_CR)
        return Ntimeout;

    Port->cmd |= HBA_PX_CMD_FRE;
    Port->cmd |= HBA_PX_CMD_ST;
    return NSTATUS_OK;
}

/* 링크가 살아 있는지 (장치 감지 + 활성 상태) */
static bool ahci_port_link_up(HBA_PORT *Port)
{
    u32 ssts = Port->ssts;
    u8 ipm = (u8)((ssts >> 8) & 0x0FU);
    u8 det = (u8)(ssts & 0x0FU);

    return (det == HBA_PORT_DET_PRESENT && ipm == HBA_PORT_IPM_ACTIVE) ? true : false;
}

static Nstatus ahci_port_rebase(HBA_PORT *Port, i32 PortNo);

/*
 * 포트를 COMRESET 하여 장치 시그니처를 다시 받는다.
 *
 * [왜 필요한가] UEFI 의 AHCI 드라이버는 ExitBootServices 시점에 포트의 FIS 수신/명령 엔진을
 *   멈춘다. 그러면 장치는 연결되어 있어도(SSTS.DET=3) 시그니처(PxSIG)가 0xFFFFFFFF 로 남아
 *   있어서, 시그니처만 보고 장치를 찾던 예전 코드는 "No SATA device found" 였다.
 *   (FIS 수신 엔진을 켜고 링크를 재설정해야 장치의 D2H 레지스터 FIS 가 도착한다)
 */
static Nstatus ahci_port_reset(HBA_PORT *Port)
{
    u32 spin;

    Port->sctl = (Port->sctl & ~0x0FU) | 1U;      /* DET=1: COMRESET 시작 */
    for (spin = 0; spin < 3000; spin++)           /* 최소 1ms 유지 (port 0x80 쓰기 ~1us) */
        io_wait();
    Port->sctl &= ~0x0FU;                         /* DET=0: 재설정 종료 */

    for (spin = 0; spin < AHCI_SPIN_LIMIT; spin++) {
        if (ahci_port_link_up(Port))
            break;
        cpu_pause();
    }
    if (!ahci_port_link_up(Port))
        return Ntimeout;

    Port->serr = 0xFFFFFFFFU;

    /* 장치가 BSY/DRQ 를 내리고 시그니처 FIS 를 보낼 때까지 대기 */
    for (spin = 0; spin < AHCI_SPIN_LIMIT; spin++) {
        if (!(Port->tfd & (0x80U | 0x08U)) && Port->sig != 0xFFFFFFFFU)
            return NSTATUS_OK;
        cpu_pause();
    }

    return Ntimeout;
}

/* 이 포트에 ATA(SATA 디스크) 장치가 있는지 확인한다. 준비(rebase/reset)도 함께 수행한다. */
static bool ahci_probe_port(HBA_PORT *Port, i32 PortNo)
{
    if (!ahci_port_link_up(Port))
        return false;

    if (NSTATUS_IS_ERR(ahci_port_rebase(Port, PortNo)))
        return false;

    if (Port->sig == 0xFFFFFFFFU || (Port->tfd & (0x80U | 0x08U))) {
        if (NSTATUS_IS_ERR(ahci_port_reset(Port)))
            return false;
    }

    return (Port->sig == SATA_SIG_ATA) ? true : false;
}

static i32 ahci_find_cmdslot(HBA_PORT *Port)
{
    u32 slots = Port->sact | Port->ci;
    i32 i;

    for (i = 0; i < AHCI_MAX_CMD_SLOTS; i++) {
        if (!(slots & (1U << i)))
            return i;
    }
    return -1;
}

static Nstatus ahci_port_rebase(HBA_PORT *Port, i32 PortNo)
{
    HBA_CMD_HEADER *CmdHeader;
    u64 clb_addr;
    u64 fb_addr;
    i32 slot;
    Nstatus status;

    if (!Port || PortNo < 0 || PortNo >= AHCI_MAX_PORTS)
        return NinvalidArg;

    status = ahci_stop_cmd(Port);
    if (NSTATUS_IS_ERR(status))
        return status;

    /* 컨트롤러 레지스터에는 "물리 주소" 를, CPU 접근에는 커널 가상 주소를 사용한다 */
    clb_addr = virt_to_phys(&ahci_command_list[PortNo]);
    Port->clb = (u32)clb_addr;
    Port->clbu = (u32)(clb_addr >> 32);
    memset(ahci_command_list[PortNo], 0, 1024);

    fb_addr = virt_to_phys(&ahci_fis[PortNo]);
    Port->fb = (u32)fb_addr;
    Port->fbu = (u32)(fb_addr >> 32);
    memset(ahci_fis[PortNo], 0, 256);

    CmdHeader = (HBA_CMD_HEADER *)(void *)ahci_command_list[PortNo];
    for (slot = 0; slot < AHCI_MAX_CMD_SLOTS; slot++) {
        u64 tbl_addr = virt_to_phys(&ahci_cmd_tables[PortNo][slot]);

        CmdHeader[slot].flags = (u16)(sizeof(FIS_REG_H2D) / sizeof(u32));
        CmdHeader[slot].prdtl = 1;
        CmdHeader[slot].ctba = (u32)tbl_addr;
        CmdHeader[slot].ctbau = (u32)(tbl_addr >> 32);
        memset(ahci_cmd_tables[PortNo][slot], 0, 256);
    }

    Port->serr = 0xFFFFFFFFU;   /* 이전 에러 상태 클리어 */
    Port->is = 0xFFFFFFFFU;

    return ahci_start_cmd(Port);
}

/* ------------------------------------------------------------------ */
/* 명령 실행 (DMA, 폴링)                                               */
/* ------------------------------------------------------------------ */

/*
 * 명령 하나를 보내고 끝날 때까지 기다린다.
 *   command: ATA_CMD_READ_DMA_EX / ATA_CMD_WRITE_DMA_EX / ATA_CMD_FLUSH_CACHE_EX
 *   데이터 없는 명령(FLUSH)은 SectorCount = 0, Buffer = NULL.
 */
static Nstatus ahci_issue(HBA_PORT *Port, u8 command, u64 StartLba, u32 SectorCount, void *Buffer)
{
    bool has_data = (SectorCount != 0) ? true : false;
    bool is_write = (command == ATA_CMD_WRITE_DMA_EX) ? true : false;
    i32 slot;
    i32 port_no;
    HBA_CMD_HEADER *CmdHeader;
    HBA_CMD_TBL *CmdTbl;
    HBA_PRDT_ENTRY *Prdt;
    FIS_REG_H2D *Fis;
    u64 buffer_addr = 0;
    u32 timeout;

    if (!Port || !ahci_base)
        return NinvalidArg;

    if (has_data) {
        if (!Buffer)
            return NinvalidArg;
        /* PRDT 하나로 전송 가능한 크기(4MiB) 및 48비트 LBA 범위 검사 */
        if (SectorCount > AHCI_MAX_SECTORS_PER_READ)
            return NinvalidArg;
        if (StartLba >= (1UL << 48) || (StartLba + SectorCount) > (1UL << 48))
            return NinvalidArg;

        /* DMA 는 물리 주소가 필요하다. 커널 이미지/HHDM 안의 (물리적으로 연속인) 버퍼만 허용한다 */
        if (!virt_is_direct(Buffer))
            return NinvalidPointer;
        buffer_addr = virt_to_phys(Buffer);

        /* DMA 버퍼는 2바이트 정렬이어야 한다 (dba bit0 = 0) */
        if (buffer_addr & 1UL)
            return NinvalidArg;

        /* 64비트 DMA 를 지원하지 않는 컨트롤러는 4GiB 이내 버퍼만 접근 가능 */
        if (!ahci_dma64 && (buffer_addr + ((u64)SectorCount << 9)) > 0x100000000UL)
            return NinvalidPointer;
    }

    slot = ahci_find_cmdslot(Port);
    if (slot < 0)
        return Nbusy;

    /* 이 포트의 command list / command table 은 정적 배열이므로 포트 번호로 바로 찾는다 */
    port_no = (i32)(Port - &ahci_base->ports[0]);
    if (port_no < 0 || port_no >= AHCI_MAX_PORTS)
        return NinvalidArg;

    CmdHeader = (HBA_CMD_HEADER *)(void *)ahci_command_list[port_no];
    CmdHeader += slot;
    CmdHeader->prdtl = has_data ? 1 : 0;
    CmdHeader->prdbc = 0;
    CmdHeader->flags = (u16)((sizeof(FIS_REG_H2D) / sizeof(u32)) | (is_write ? AHCI_CMD_HDR_WRITE : 0U));

    CmdTbl = (HBA_CMD_TBL *)(void *)ahci_cmd_tables[port_no][slot];
    memset(CmdTbl, 0, 256);

    if (has_data) {
        Prdt = (HBA_PRDT_ENTRY *)((u8 *)CmdTbl + 0x80);
        Prdt[0].dba = (u32)buffer_addr;
        Prdt[0].dbau = (u32)(buffer_addr >> 32);
        Prdt[0].rsv0 = 0;
        Prdt[0].dbc = (SectorCount << 9) - 1;    /* 최대 4MiB - 1 (22비트) */
    }

    Fis = (FIS_REG_H2D *)(void *)&CmdTbl->cfis;
    memset(Fis, 0, sizeof(FIS_REG_H2D));
    Fis->fis_type = FIS_TYPE_REG_H2D;
    Fis->pmport_c = FIS_H2D_C;
    Fis->command = command;
    Fis->device = (u8)(1U << 6);              /* LBA 모드 */
    if (has_data) {
        Fis->lba0 = (u8)StartLba;
        Fis->lba1 = (u8)(StartLba >> 8);
        Fis->lba2 = (u8)(StartLba >> 16);
        Fis->lba3 = (u8)(StartLba >> 24);
        Fis->lba4 = (u8)(StartLba >> 32);
        Fis->lba5 = (u8)(StartLba >> 40);
        Fis->countl = (u8)(SectorCount & 0xFFU);
        Fis->counth = (u8)((SectorCount >> 8) & 0xFFU);
    }

    Port->is = 0xFFFFFFFFU;

    /* 장치가 BSY/DRQ 상태에서 벗어날 때까지 대기 (상한 있음) */
    for (timeout = 0; timeout < AHCI_SPIN_LIMIT; timeout++) {
        if (!(Port->tfd & (0x80U | 0x08U)))
            break;
        cpu_pause();
    }
    if (Port->tfd & (0x80U | 0x08U))
        return Ntimeout;

    Port->ci = 1U << slot;

    /* 쓰기와 FLUSH 는 읽기보다 오래 걸릴 수 있다 (실제 디스크의 캐시 비우기). 상한을 넉넉히 */
    for (timeout = 0; timeout < AHCI_SPIN_LIMIT * 8UL; timeout++) {
        if (!(Port->ci & (1U << slot)))
            break;
        if (Port->is & (1U << 30))          /* Task File Error */
            return Nio;
        cpu_pause();
    }

    if (Port->ci & (1U << slot))
        return Ntimeout;

    if ((Port->is & (1U << 30)) || (Port->tfd & 0x01U))
        return Nio;

    return NSTATUS_OK;
}

Nstatus ahci_read(HBA_PORT *Port, u64 StartLba, u32 SectorCount, void *Buffer)
{
    if (SectorCount == 0)
        return NinvalidArg;
    return ahci_issue(Port, ATA_CMD_READ_DMA_EX, StartLba, SectorCount, Buffer);
}

Nstatus ahci_write(HBA_PORT *Port, u64 StartLba, u32 SectorCount, const void *Buffer)
{
    if (SectorCount == 0)
        return NinvalidArg;
    return ahci_issue(Port, ATA_CMD_WRITE_DMA_EX, StartLba, SectorCount, (void *)(usize)Buffer);
}

Nstatus ahci_flush(HBA_PORT *Port)
{
    return ahci_issue(Port, ATA_CMD_FLUSH_CACHE_EX, 0, 0, nNULL);
}

/* ------------------------------------------------------------------ */
/* 디스크 번호 + 바이트 단위 접근 (파일시스템 콜백)                     */
/* ------------------------------------------------------------------ */

static HBA_PORT *g_disks[AHCI_MAX_PORTS];
static u32 g_disk_count = 0;

/*
 * 섹터 단위 DMA 와 임의의 (offset, size) 사이의 중간 버퍼. 커널 이미지 안에 있으므로 물리적으로 연속이고
 * DMA 가능하다 (호출자 버퍼는 kmalloc 영역일 수도 있어 직접 DMA 하지 않는다). 커널은 비선점이고 시스템
 * 콜 안은 인터럽트가 꺼져 있으므로 버퍼 하나를 함께 써도 된다.
 */
#define AHCI_BOUNCE_SIZE (64U * 1024U)
static u8 g_bounce[AHCI_BOUNCE_SIZE] __attribute__((aligned(4096)));

u32 ahci_disk_count(void)
{
    return g_disk_count;
}

static HBA_PORT *disk_port(u32 diskno)
{
    if (diskno < AHCI_DISKNO_BASE || diskno - AHCI_DISKNO_BASE >= g_disk_count)
        return nNULL;
    return g_disks[diskno - AHCI_DISKNO_BASE];
}

/*
 * 한 번에 중간 버퍼만큼씩: 시작 섹터부터 필요한 섹터 수를 덮어 읽거나 쓴다.
 * 쓰기에서 첫/끝 섹터가 일부만 바뀌면 먼저 읽어 와서 나머지를 보존한다 (read-modify-write).
 */
static Nstatus disk_rw(u32 diskno, usize offset, u8 *buf, usize size, bool write)
{
    HBA_PORT *Port = disk_port(diskno);
    Nstatus status;

    if (!Port)
        return NdeviceMissing;
    if (!buf && size)
        return NinvalidArg;

    while (size > 0) {
        u64 lba = (u64)offset >> 9;
        usize head = offset & 511U;
        usize span = head + size;
        usize n;
        u32 nsec;

        if (span > AHCI_BOUNCE_SIZE)
            span = AHCI_BOUNCE_SIZE;
        n = span - head;
        nsec = (u32)((span + 511U) >> 9);

        if (!write || head != 0 || (span & 511U) != 0) {
            status = ahci_read(Port, lba, nsec, g_bounce);
            if (NSTATUS_IS_ERR(status))
                return status;
        }
        if (write) {
            memcpy(g_bounce + head, buf, n);
            status = ahci_write(Port, lba, nsec, g_bounce);
            if (NSTATUS_IS_ERR(status))
                return status;
        } else {
            memcpy(buf, g_bounce + head, n);
        }

        offset += n;
        buf += n;
        size -= n;
    }
    return NSTATUS_OK;
}

Nstatus ahci_disk_read(u32 diskno, usize offset, void *buffer, usize size, NTBLI *info)
{
    (void)info;
    return disk_rw(diskno, offset, (u8 *)buffer, size, false);
}

Nstatus ahci_disk_write(u32 diskno, usize offset, const void *buffer, usize size, NTBLI *info)
{
    (void)info;
    return disk_rw(diskno, offset, (u8 *)(usize)buffer, size, true);
}

Nstatus ahci_flush_all(void)
{
    Nstatus result = NSTATUS_OK;
    u32 i;

    for (i = 0; i < g_disk_count; i++) {
        Nstatus status = ahci_flush(g_disks[i]);

        if (NSTATUS_IS_ERR(status))
            result = status;
    }
    return result;
}

Nstatus ahci_init(void)
{
    u32 pi;
    i32 i;
    Nstatus status;

    status = ahci_find_controller();
    if (NSTATUS_IS_ERR(status)) {
        printk_warn("AHCI controller not found\n");
        return status;
    }

    /* 레지스터가 전부 1 이면 실제로는 장치가 없는 것 */
    if (ahci_base->vs == 0xFFFFFFFFU || ahci_base->cap == 0xFFFFFFFFU) {
        ahci_base = nNULL;
        return NdeviceMissing;
    }

    ahci_base->ghc |= HBA_GHC_AE;
    ahci_dma64 = (ahci_base->cap & HBA_CAP_S64A) ? true : false;

    /* 연결된 SATA 디스크를 모두 준비해 디스크 번호를 붙인다 (포트 번호 순) */
    g_disk_count = 0;
    pi = ahci_base->pi;
    for (i = 0; i < AHCI_MAX_PORTS; i++, pi >>= 1) {
        HBA_PORT *Port = (HBA_PORT *)&ahci_base->ports[i];

        if ((pi & 1U) && ahci_probe_port(Port, i))
            g_disks[g_disk_count++] = Port;
    }

    if (g_disk_count == 0) {
        printk_warn("AHCI: no SATA disk found\n");
        return NdeviceMissing;
    }

    printk_ok("AHCI: %u SATA disk(s)\n", g_disk_count);
    return Nok;
}
