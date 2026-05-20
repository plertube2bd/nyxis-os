// do not use in real os. it's unfinished.

#include "ahci.h"
#include "include/lowlevel.h"
#include "include/nyxis.h"
#include "include/memory.h"
#include "console/outputs/printk.h"

static volatile HBA_MEM* ahci_base = nNULL;

static u8 ahci_command_list[AHCI_MAX_PORTS][1024] __attribute__((aligned(1024)));
static u8 ahci_fis[AHCI_MAX_PORTS][256] __attribute__((aligned(256)));
static u8 ahci_cmd_tables[AHCI_MAX_PORTS][AHCI_MAX_CMD_SLOTS][256] __attribute__((aligned(256)));

static u32 pci_read_config(u8 bus, u8 device, u8 function, u8 offset) {
    u32 address = (1u << 31) |
                    ((u32)bus << 16) |
                    ((u32)device << 11) |
                    ((u32)function << 8) |
                    (offset & 0xFC);

    outl(0xCF8, address);
    return inl(0xCFC);
}

static u64 pci_get_bar5(u8 bus, u8 device, u8 function) {
    u32 bar_low = pci_read_config(bus, device, function, 0x24);
    if ((bar_low & 0x1) != 0) {
        return 0;
    }

    u64 bar = bar_low & 0xFFFFFFF0u;
    if ((bar_low & 0x6u) == 0x4u) {
        u32 bar_high = pci_read_config(bus, device, function, 0x28);
        bar |= ((u64)bar_high << 32);
    }

    return bar;
}

static bool pci_is_ahci_device(u8 bus, u8 device, u8 function) {
    u32 vendor_device = pci_read_config(bus, device, function, 0x0);
    if ((vendor_device & 0xFFFFu) == 0xFFFFu) {
        return false;
    }

    u32 class_info = pci_read_config(bus, device, function, 0x08);
    u8 prog_if = (class_info >> 8) & 0xFF;
    u8 subclass = (class_info >> 16) & 0xFF;
    u8 baseclass = (class_info >> 24) & 0xFF;

    return baseclass == 0x01 && subclass == 0x06 && prog_if == 0x01;
}

static Nstatus ahci_find_controller(void) {
    for (u8 bus = 0; bus < 256; bus++) {
        for (u8 device = 0; device < 32; device++) {
            u32 header = pci_read_config(bus, device, 0, 0x0C);
            if ((header & 0xFFFFu) == 0xFFFFu) {
                continue;
            }

            u8 header_type = (header >> 16) & 0xFF;
            u8 functions = (header_type & 0x80u) ? 8 : 1;

            for (u8 function = 0; function < functions; function++) {
                if (!pci_is_ahci_device(bus, device, function)) {
                    continue;
                }

                u64 abar = pci_get_bar5(bus, device, function);
                if (!abar) {
                    continue;
                }

                ahci_base = (HBA_MEM*)(usize)abar;
                return NSTATUS_OK;
            }
        }
    }

    return NdeviceMissing;
}

static void ahci_stop_cmd(HBA_PORT* Port) {
    Port->cmd &= ~HBA_PX_CMD_ST;
    Port->cmd &= ~HBA_PX_CMD_FRE;

    while (Port->cmd & HBA_PX_CMD_FR || Port->cmd & HBA_PX_CMD_CR) {
        ;
    }
}

static void ahci_start_cmd(HBA_PORT* Port) {
    while (Port->cmd & HBA_PX_CMD_CR) {
        ;
    }

    Port->cmd |= HBA_PX_CMD_FRE;
    Port->cmd |= HBA_PX_CMD_ST;
}

static i32 ahci_check_type(HBA_PORT* Port) {
    u32 ssts = Port->ssts;

    u8 ipm = (ssts >> 8) & 0x0F;
    u8 det = ssts & 0x0F;

    if (det != HBA_PORT_DET_PRESENT) {
        return 0;
    }

    if (ipm != HBA_PORT_IPM_ACTIVE) {
        return 0;
    }

    return Port->sig == SATA_SIG_ATA;
}

HBA_PORT* ahci_find_port(void) {
    if (!ahci_base) {
        return nNULL;
    }

    u32 pi = ahci_base->pi;
    for (i32 i = 0; i < AHCI_MAX_PORTS; i++) {
        if ((pi & 1u) && ahci_check_type(&ahci_base->ports[i])) {
            return &ahci_base->ports[i];
        }
        pi >>= 1;
    }

    return nNULL;
}

static i32 ahci_find_cmdslot(HBA_PORT* Port) {
    u32 slots = Port->sact | Port->ci;
    for (i32 i = 0; i < AHCI_MAX_CMD_SLOTS; i++) {
        if (!(slots & (1u << i))) {
            return i;
        }
    }
    return -1;
}

static Nstatus ahci_port_rebase(HBA_PORT* Port, i32 PortNo) {
    if (!Port || PortNo < 0 || PortNo >= AHCI_MAX_PORTS) {
        return NinvalidArg;
    }

    ahci_stop_cmd(Port);

    u64 clb_addr = (u64)(usize)&ahci_command_list[PortNo];
    Port->clb = (u32)clb_addr;
    Port->clbu = (u32)(clb_addr >> 32);
    memset((void*)(usize)clb_addr, 0, 1024);

    u64 fb_addr = (u64)(usize)&ahci_fis[PortNo];
    Port->fb = (u32)fb_addr;
    Port->fbu = (u32)(fb_addr >> 32);
    memset((void*)(usize)fb_addr, 0, 256);

    HBA_CMD_HEADER* CmdHeader = (HBA_CMD_HEADER*)(usize)Port->clb;
    for (i32 slot = 0; slot < AHCI_MAX_CMD_SLOTS; slot++) {
        CmdHeader[slot].flags = sizeof(FIS_REG_H2D) / sizeof(u32);
        CmdHeader[slot].prdtl = 1;

        u64 tbl_addr = (u64)(usize)&ahci_cmd_tables[PortNo][slot];
        CmdHeader[slot].ctba = (u32)tbl_addr;
        CmdHeader[slot].ctbau = (u32)(tbl_addr >> 32);
        memset((void*)(usize)tbl_addr, 0, 256);
    }

    ahci_start_cmd(Port);
    return NSTATUS_OK;
}

Nstatus ahci_read(
    HBA_PORT* Port,
    u64 StartLba,
    u32 SectorCount,
    void* Buffer
) {
    if (!Port || !Buffer || SectorCount == 0) {
        return NinvalidArg;
    }

    i32 slot = ahci_find_cmdslot(Port);
    if (slot < 0) {
        return Nbusy;
    }

    HBA_CMD_HEADER* CmdHeader =
        (HBA_CMD_HEADER*)(usize)(((u64)Port->clbu << 32) | Port->clb);
    CmdHeader += slot;
    CmdHeader->prdtl = 1;
    CmdHeader->prdbc = 0;
    CmdHeader->flags = sizeof(FIS_REG_H2D) / sizeof(u32);

    u64 table_addr = ((u64)CmdHeader->ctbau << 32) | CmdHeader->ctba;
    HBA_CMD_TBL* CmdTbl = (HBA_CMD_TBL*)(usize)table_addr;
    memset(CmdTbl, 0, 256);

    HBA_PRDT_ENTRY* Prdt = (HBA_PRDT_ENTRY*)((u8*)CmdTbl + 0x80);
    u64 buffer_addr = (u64)(usize)Buffer;
    Prdt[0].dba = (u32)buffer_addr;
    Prdt[0].dbau = (u32)(buffer_addr >> 32);
    Prdt[0].rsv0 = 0;
    Prdt[0].dbc = (SectorCount << 9) - 1;

    FIS_REG_H2D* Fis = (FIS_REG_H2D*)(&CmdTbl->cfis);
    memset(Fis, 0, sizeof(FIS_REG_H2D));
    Fis->fis_type = FIS_TYPE_REG_H2D;
    Fis->c = 1;
    Fis->command = ATA_CMD_READ_DMA_EX;
    Fis->lba0 = (u8)StartLba;
    Fis->lba1 = (u8)(StartLba >> 8);
    Fis->lba2 = (u8)(StartLba >> 16);
    Fis->lba3 = (u8)(StartLba >> 24);
    Fis->lba4 = (u8)(StartLba >> 32);
    Fis->lba5 = (u8)(StartLba >> 40);
    Fis->device = 1 << 6;
    Fis->countl = SectorCount & 0xFF;
    Fis->counth = (SectorCount >> 8) & 0xFF;

    Port->is = (u32)-1;

    while (Port->tfd & (0x80 | 0x08)) {
        ;
    }

    Port->ci = 1u << slot;

    u32 timeout = 0;
    while (Port->ci & (1u << slot)) {
        if (++timeout > 1000000u) {
            return Ntimeout;
        }
        if (Port->is & (1u << 30)) {
            return Nio;
        }
    }

    if (Port->is & (1u << 30)) {
        return Nio;
    }

    return NSTATUS_OK;
}

Nstatus ahci_init(void) {
    Nstatus status = ahci_find_controller();
    if (NSTATUS_IS_ERR(status)) {
        printk("AHCI controller not found\n");
        return status;
    }

    printk("AHCI controller found\n");

    ahci_base->ghc |= HBA_GHC_AE;

    HBA_PORT* Port = ahci_find_port();
    if (!Port) {
        printk("No SATA device found\n");
        return NdeviceMissing;
    }

    i32 port_index = (i32)(Port - &ahci_base->ports[0]);
    status = ahci_port_rebase(Port, port_index);
    if (NSTATUS_IS_ERR(status)) {
        printk("AHCI port rebase failed\n");
        return status;
    }

    static u8 Buffer[512];
    status = ahci_read(Port, 0, 1, Buffer);
    if (NSTATUS_IS_ERR(status)) {
        printk("AHCI read failed: %x\n", status);
        return status;
    }

    printk("AHCI read success\n");
    for (i32 i = 0; i < 16; i++) {
        printk("%x ", Buffer[i]);
    }
    printk("\n");

    return NSTATUS_OK;
}
