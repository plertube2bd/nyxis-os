// do not use in real os. it's unfinished.

#include "ahci.h"

#include "include/nyxis.h"
#include "include/memory.h"

// ======================================================
// PORT HELPERS
// ======================================================

static void ahci_stop_cmd(HBA_PORT* Port) {
    Port->cmd &= ~HBA_PX_CMD_ST;
    Port->cmd &= ~HBA_PX_CMD_FRE;

    while (1) {
        if (!(Port->cmd & HBA_PX_CMD_FR) &&
            !(Port->cmd & HBA_PX_CMD_CR)) {
            break;
        }
    }
}

static void ahci_start_cmd(HBA_PORT* Port) {
    while (Port->cmd & HBA_PX_CMD_CR);

    Port->cmd |= HBA_PX_CMD_FRE;
    Port->cmd |= HBA_PX_CMD_ST;
}

static i32 ahci_check_type(HBA_PORT* Port) {
    u32 ssts = Port->ssts;

    u8 ipm = (ssts >> 8) & 0x0F;
    u8 det = ssts & 0x0F;

    if (det != HBA_PORT_DET_PRESENT)
        return 0;

    if (ipm != HBA_PORT_IPM_ACTIVE)
        return 0;

    switch (Port->sig) {
        case SATA_SIG_ATA:
            return 1;
    }

    return 0;
}

// ======================================================
// FIND SATA PORT
// ======================================================

static HBA_PORT* ahci_find_port(void) {
    u32 pi = ahci_base->pi;

    for (i32 i = 0; i < 32; i++) {
        if (pi & 1) {
            if (ahci_check_type(&ahci_base->ports[i])) {
                return &ahci_base->ports[i];
            }
        }

        pi >>= 1;
    }

    return nNULL;
}

// ======================================================
// PORT INIT
// ======================================================

static void ahci_port_rebase(HBA_PORT* Port, i32 PortNo) {
    ahci_stop_cmd(Port);

    Port->clb  = AHCI_BASE + (PortNo << 10);
    Port->clbu = 0;

    memset((void*)(usize)Port->clb, 0, 1024);

    Port->fb  = AHCI_BASE + (32 << 10) + (PortNo << 8);
    Port->fbu = 0;

    memset((void*)(usize)Port->fb, 0, 256);

    HBA_CMD_HEADER* CmdHeader =
        (HBA_CMD_HEADER*)(usize)Port->clb;

    for (i32 i = 0; i < 32; i++) {
        CmdHeader[i].prdtl = 1;

        CmdHeader[i].ctba =
            AHCI_BASE + (40 << 10) + (PortNo << 13) + (i << 8);

        CmdHeader[i].ctbau = 0;

        memset(
            (void*)(usize)CmdHeader[i].ctba,
            0,
            256
        );
    }

    ahci_start_cmd(Port);
}

// ======================================================
// READ SECTOR
// ======================================================

i32 ahci_read(
    HBA_PORT* Port,
    u64 StartLba,
    u32 SectorCount,
    void* Buffer
) {
    Port->is = (u32)-1;

    HBA_CMD_HEADER* CmdHeader =
        (HBA_CMD_HEADER*)(usize)Port->clb;

    CmdHeader->flags =
        (sizeof(FIS_REG_H2D) / sizeof(u32)) | (1 << 6);

    CmdHeader->prdtl = 1;

    HBA_CMD_TBL* CmdTbl =
        (HBA_CMD_TBL*)(usize)CmdHeader->ctba;

    memset(CmdTbl, 0, sizeof(HBA_CMD_TBL));

    // ==================================================
    // PRDT
    // ==================================================

    u32* Prdt = (u32*)((u8*)CmdTbl + 0x80);

    Prdt[0] = (u32)(usize)Buffer;
    Prdt[1] = 0;
    Prdt[2] = (SectorCount << 9) - 1;
    Prdt[3] = 1;

    // ==================================================
    // FIS
    // ==================================================

    FIS_REG_H2D* Fis =
        (FIS_REG_H2D*)(&CmdTbl->cfis);

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

    // ==================================================
    // WAIT
    // ==================================================

    while ((Port->tfd & (0x80 | 0x08)));

    // ISSUE COMMAND

    Port->ci = 1;

    while (1) {
        if (!(Port->ci & 1))
            break;

        if (Port->is & (1 << 30))
            return 0;
    }

    if (Port->is & (1 << 30))
        return 0;

    return 1;
}

// ======================================================
// INIT
// ======================================================

void ahci_init(void) {
    HBA_PORT* Port = ahci_find_port();

    if (!Port) {
        printk("No SATA drive found\n");
        return;
    }

    printk("SATA drive detected\n");

    ahci_port_rebase(Port, 0);

    static u8 Buffer[512];

    if (ahci_read(Port, 0, 1, Buffer)) {
        printk("Read success\n");

        for (i32 i = 0; i < 16; i++) {
            printk("%x ", Buffer[i]);
        }

        printk("\n");
    } else {
        printk("Read failed\n");
    }
}
