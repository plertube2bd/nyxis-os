/*
 * ata.h - ATA PIO 드라이버 (기본 채널, 마스터 드라이브, LBA28)
 */
#ifndef ATA_H
#define ATA_H

#include "nyxis.h"

/* ATA ports for primary channel */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_FEATURES    0x1F1
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_COMMAND     0x1F7
#define ATA_STATUS      0x1F7

/* ATA commands */
#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_IDENTIFY      0xEC

/* Status bits */
#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_RDY  0x40
#define ATA_STATUS_DF   0x20
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

/* LBA28 은 28비트까지만 표현 가능 */
#define ATA_LBA28_LIMIT 0x10000000UL

/* 섹터 1개(512바이트) 읽기/쓰기. buffer 는 512바이트 이상, 2바이트 정렬이어야 한다. */
Nstatus ata_read_sector(u32 lba, void *buffer);
Nstatus ata_write_sector(u32 lba, const void *buffer);

#endif /* ATA_H */
