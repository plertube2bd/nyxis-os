#include "vfs.h"
#include "core.h"
#include "nyxis.h"

#define FAT16_ENTRY_SIZE 32 // bytes

typedef struct fat16_boot_sector {
    //   0x00 - 0x02
    u8   JumpInstruction[3]; // not use.

    //   0x03 - 0x0A
    utf8 OEMName[8]; // not use.

    //   0x0B - 0x0C
    u16  BytesPerSector;

    //   0x0D
    u8   SectorsPerCluster;

    //   0x0E - 0x0F
    u16  ReservedSectorCount;

    //   0x10
    u8   FATCount;

    //   0x11 - 0x12
    u16  RootEntryCount;

    //   0x13 - 0x14
    u16  TotalSectors16;

    //   0x15
    u8   MediaType; // not use.

    //   0x16 - 0x17
    u16  FATSize16;
} fat16_boot_sector_t;

