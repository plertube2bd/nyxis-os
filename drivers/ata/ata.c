#include "ata.h"
#include "lowlevel.h"
#include "nyxis.h"

// Wait for BSY to clear and RDY to set
static void ata_wait_ready() {
    while (inb(ATA_STATUS) & ATA_STATUS_BSY);
    while (!(inb(ATA_STATUS) & ATA_STATUS_RDY));
}

// Wait for DRQ to set
static void ata_wait_drq() {
    while (!(inb(ATA_STATUS) & ATA_STATUS_DRQ));
}

// Select drive and set LBA
static void ata_select_drive(u32 lba) {
    u8 drive = 0xE0 | ((lba >> 24) & 0x0F);
    outb(ATA_DRIVE_HEAD, drive);
    io_wait();
}

Nstatus ata_read_sector(u32 lba, void* buffer) {
    u16* buf = (u16*)buffer;

    // Select drive
    ata_select_drive(lba);

    // Set sector count to 1
    outb(ATA_SECTOR_COUNT, 1);

    // Set LBA
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);

    // Send read command
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);

    // Wait for ready
    ata_wait_ready();

    // Check for error
    if (inb(ATA_STATUS) & ATA_STATUS_ERR) {
        return NSTATUS_ERR_FLAG | 1; // Error code 1
    }

    // Wait for data
    ata_wait_drq();

    // Read 256 words (512 bytes)
    for (int i = 0; i < 256; i++) {
        buf[i] = inw(ATA_DATA);
    }

    return NSTATUS_OK;
}

Nstatus ata_write_sector(u32 lba, void* buffer) {
    u16* buf = (u16*)buffer;

    // Select drive
    ata_select_drive(lba);

    // Set sector count to 1
    outb(ATA_SECTOR_COUNT, 1);

    // Set LBA
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);

    // Send write command
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

    // Wait for ready
    ata_wait_ready();

    // Check for error
    if (inb(ATA_STATUS) & ATA_STATUS_ERR) {
        return NSTATUS_ERR_FLAG | 2; // Error code 2
    }

    // Wait for data request
    ata_wait_drq();

    // Write 256 words (512 bytes)
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, buf[i]);
    }

    // Wait for write to complete
    ata_wait_ready();

    return NSTATUS_OK;
}