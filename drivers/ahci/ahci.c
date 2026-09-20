/*
 * ahci.c - AHCI(SATA) 읽기 전용 최소 드라이버 (폴링, 포트 1개)
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

                ahci_base = (HBA_MEM *)(usize)abar;
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

HBA_PORT *ahci_find_port(void)
{
    u32 pi;
    i32 i;

    if (!ahci_base)
        return nNULL;

    pi = ahci_base->pi;
    for (i = 0; i < AHCI_MAX_PORTS; i++) {
        if ((pi & 1U) && ahci_probe_port((HBA_PORT *)&ahci_base->ports[i], i))
            return (HBA_PORT *)&ahci_base->ports[i];
        pi >>= 1;
    }

    return nNULL;
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

    clb_addr = (u64)(usize)&ahci_command_list[PortNo];
    Port->clb = (u32)clb_addr;
    Port->clbu = (u32)(clb_addr >> 32);
    memset((void *)(usize)clb_addr, 0, 1024);

    fb_addr = (u64)(usize)&ahci_fis[PortNo];
    Port->fb = (u32)fb_addr;
    Port->fbu = (u32)(fb_addr >> 32);
    memset((void *)(usize)fb_addr, 0, 256);

    CmdHeader = (HBA_CMD_HEADER *)(usize)clb_addr;
    for (slot = 0; slot < AHCI_MAX_CMD_SLOTS; slot++) {
        u64 tbl_addr = (u64)(usize)&ahci_cmd_tables[PortNo][slot];

        CmdHeader[slot].flags = (u16)(sizeof(FIS_REG_H2D) / sizeof(u32));
        CmdHeader[slot].prdtl = 1;
        CmdHeader[slot].ctba = (u32)tbl_addr;
        CmdHeader[slot].ctbau = (u32)(tbl_addr >> 32);
        memset((void *)(usize)tbl_addr, 0, 256);
    }

    Port->serr = 0xFFFFFFFFU;   /* 이전 에러 상태 클리어 */
    Port->is = 0xFFFFFFFFU;

    return ahci_start_cmd(Port);
}

/* ------------------------------------------------------------------ */
/* 읽기 (DMA)                                                          */
/* ------------------------------------------------------------------ */

Nstatus ahci_read(
    HBA_PORT *Port,
    u64 StartLba,
    u32 SectorCount,
    void *Buffer
) {
    i32 slot;
    HBA_CMD_HEADER *CmdHeader;
    HBA_CMD_TBL *CmdTbl;
    HBA_PRDT_ENTRY *Prdt;
    FIS_REG_H2D *Fis;
    u64 table_addr;
    u64 buffer_addr;
    u32 timeout;

    if (!Port || !Buffer || SectorCount == 0)
        return NinvalidArg;

    /* PRDT 하나로 전송 가능한 크기(4MiB) 및 48비트 LBA 범위 검사 */
    if (SectorCount > AHCI_MAX_SECTORS_PER_READ)
        return NinvalidArg;
    if (StartLba >= (1UL << 48) || (StartLba + SectorCount) > (1UL << 48))
        return NinvalidArg;

    buffer_addr = (u64)(usize)Buffer;

    /* DMA 버퍼는 2바이트 정렬이어야 한다 (dba bit0 = 0) */
    if (buffer_addr & 1UL)
        return NinvalidArg;

    /* 64비트 DMA 를 지원하지 않는 컨트롤러는 4GiB 이내 버퍼만 접근 가능 */
    if (!ahci_dma64 && (buffer_addr + ((u64)SectorCount << 9)) > 0x100000000UL)
        return NinvalidPointer;

    slot = ahci_find_cmdslot(Port);
    if (slot < 0)
        return Nbusy;

    CmdHeader = (HBA_CMD_HEADER *)(usize)(((u64)Port->clbu << 32) | Port->clb);
    CmdHeader += slot;
    CmdHeader->prdtl = 1;
    CmdHeader->prdbc = 0;
    CmdHeader->flags = (u16)(sizeof(FIS_REG_H2D) / sizeof(u32));

    table_addr = ((u64)CmdHeader->ctbau << 32) | CmdHeader->ctba;
    CmdTbl = (HBA_CMD_TBL *)(usize)table_addr;
    memset(CmdTbl, 0, 256);

    Prdt = (HBA_PRDT_ENTRY *)((u8 *)CmdTbl + 0x80);
    Prdt[0].dba = (u32)buffer_addr;
    Prdt[0].dbau = (u32)(buffer_addr >> 32);
    Prdt[0].rsv0 = 0;
    Prdt[0].dbc = (SectorCount << 9) - 1;    /* 최대 4MiB - 1 (22비트) */

    Fis = (FIS_REG_H2D *)(void *)&CmdTbl->cfis;
    memset(Fis, 0, sizeof(FIS_REG_H2D));
    Fis->fis_type = FIS_TYPE_REG_H2D;
    Fis->pmport_c = FIS_H2D_C;
    Fis->command = ATA_CMD_READ_DMA_EX;
    Fis->lba0 = (u8)StartLba;
    Fis->lba1 = (u8)(StartLba >> 8);
    Fis->lba2 = (u8)(StartLba >> 16);
    Fis->lba3 = (u8)(StartLba >> 24);
    Fis->lba4 = (u8)(StartLba >> 32);
    Fis->lba5 = (u8)(StartLba >> 40);
    Fis->device = (u8)(1U << 6);              /* LBA 모드 */
    Fis->countl = (u8)(SectorCount & 0xFFU);
    Fis->counth = (u8)((SectorCount >> 8) & 0xFFU);

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

    for (timeout = 0; timeout < AHCI_SPIN_LIMIT; timeout++) {
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

Nstatus ahci_init(void)
{
    static u8 Buffer[512];
    HBA_PORT *Port;
    i32 i;
    Nstatus status;

    status = ahci_find_controller();
    if (NSTATUS_IS_ERR(status)) {
        printk("AHCI controller not found\n");
        return status;
    }

    /* 레지스터가 전부 1 이면 실제로는 장치가 없는 것 */
    if (ahci_base->vs == 0xFFFFFFFFU || ahci_base->cap == 0xFFFFFFFFU) {
        ahci_base = nNULL;
        return NdeviceMissing;
    }

    printk("AHCI controller found\n");

    ahci_base->ghc |= HBA_GHC_AE;
    ahci_dma64 = (ahci_base->cap & HBA_CAP_S64A) ? true : false;

    Port = ahci_find_port();
    if (!Port) {
        printk("No SATA device found\n");
        return NdeviceMissing;
    }

    status = ahci_read(Port, 0, 1, Buffer);
    if (NSTATUS_IS_ERR(status)) {
        printk("AHCI read failed: %r\n", status);
        return status;
    }

    printk("AHCI read success\n");
    for (i = 0; i < 16; i++)
        printk("%02x ", (unsigned int)Buffer[i]);
    printk("\n");

    return Nok;
}
