/*
 * ata.c - ATA PIO 드라이버 (LBA28, 기본 채널 마스터)
 *
 * [수정 이력 요약]
 *  - 상태 레지스터를 기다리는 모든 while 루프에 시간 제한이 없었다. 드라이브가 없거나
 *    고장 나면 (0xFF 가 읽혀 BSY 가 영원히 1) 커널이 영구 정지했다. -> 타임아웃 추가.
 *  - 명령 전송 후 400ns 지연(상태 레지스터 4번 읽기) 없이 바로 상태를 읽었다.
 *  - 0xFF 상태(=드라이브 없음, 부유 버스)를 감지하지 않았다.
 *  - lba 가 28비트를 넘어도 검사하지 않고 상위 비트를 잘라서 엉뚱한 섹터를 읽고 썼다.
 *  - NULL 버퍼 검사 추가. 에러 코드를 매직 넘버 대신 표준 Nstatus 로 반환.
 *  - 쓰기 후 캐시 플러시 추가 (전원 차단 시 데이터 손실 방지).
 *  - 'int i' for 루프 선언 / 인자 없는 () 함수 정의 수정 (C89).
 */

#include "ata.h"
#include "lowlevel.h"
#include "nyxis.h"

#define ATA_SPIN_LIMIT 1000000UL

/* 400ns 이상 대기: 대체 상태 레지스터를 4번 읽는 것이 표준 방법 */
static void ata_delay_400ns(void)
{
    (void)inb(ATA_STATUS);
    (void)inb(ATA_STATUS);
    (void)inb(ATA_STATUS);
    (void)inb(ATA_STATUS);
}

/* BSY 가 꺼지고 RDY 가 켜질 때까지 (제한 시간 내) 대기 */
static Nstatus ata_wait_ready(void)
{
    u32 spin;
    u8 status;

    for (spin = 0; spin < ATA_SPIN_LIMIT; spin++) {
        status = inb(ATA_STATUS);

        if (status == 0xFF)
            return NdeviceMissing;          /* 부유 버스: 드라이브 없음 */
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_RDY))
            return NSTATUS_OK;
    }

    return Ntimeout;
}

/* DRQ 가 켜질 때까지 대기. 에러/결함 상태이면 즉시 실패 */
static Nstatus ata_wait_drq(void)
{
    u32 spin;
    u8 status;

    for (spin = 0; spin < ATA_SPIN_LIMIT; spin++) {
        status = inb(ATA_STATUS);

        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF))
            return Nio;
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ))
            return NSTATUS_OK;
    }

    return Ntimeout;
}

/* BSY 가 꺼질 때까지 대기 (쓰기 완료 확인용) */
static Nstatus ata_wait_not_busy(void)
{
    u32 spin;

    for (spin = 0; spin < ATA_SPIN_LIMIT; spin++) {
        if (!(inb(ATA_STATUS) & ATA_STATUS_BSY))
            return NSTATUS_OK;
    }

    return Ntimeout;
}

/* 드라이브 선택 + LBA 상위 4비트 설정 */
static void ata_select_drive(u32 lba)
{
    u8 drive = (u8)(0xE0U | ((lba >> 24) & 0x0FU));

    outb(ATA_DRIVE_HEAD, drive);
    ata_delay_400ns();
}

static void ata_setup_transfer(u32 lba)
{
    ata_select_drive(lba);
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, (u8)(lba & 0xFFU));
    outb(ATA_LBA_MID, (u8)((lba >> 8) & 0xFFU));
    outb(ATA_LBA_HIGH, (u8)((lba >> 16) & 0xFFU));
}

Nstatus ata_read_sector(u32 lba, void *buffer)
{
    u16 *buf = (u16 *)buffer;
    Nstatus status;
    u32 i;

    if (!buffer || ((usize)buffer & 1UL))
        return NinvalidArg;
    if (lba >= ATA_LBA28_LIMIT)
        return NinvalidArg;

    ata_setup_transfer(lba);
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);
    ata_delay_400ns();

    status = ata_wait_ready();
    if (NSTATUS_IS_ERR(status))
        return status;

    status = ata_wait_drq();
    if (NSTATUS_IS_ERR(status))
        return status;

    /* Read 256 words (512 bytes) */
    for (i = 0; i < 256; i++)
        buf[i] = inw(ATA_DATA);

    return NSTATUS_OK;
}

Nstatus ata_write_sector(u32 lba, const void *buffer)
{
    const u16 *buf = (const u16 *)buffer;
    Nstatus status;
    u32 i;

    if (!buffer || ((usize)buffer & 1UL))
        return NinvalidArg;
    if (lba >= ATA_LBA28_LIMIT)
        return NinvalidArg;

    ata_setup_transfer(lba);
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
    ata_delay_400ns();

    status = ata_wait_ready();
    if (NSTATUS_IS_ERR(status))
        return status;

    status = ata_wait_drq();
    if (NSTATUS_IS_ERR(status))
        return status;

    /* Write 256 words (512 bytes) */
    for (i = 0; i < 256; i++)
        outw(ATA_DATA, buf[i]);

    /* 쓰기 완료 후 캐시 플러시 */
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_delay_400ns();

    status = ata_wait_not_busy();
    if (NSTATUS_IS_ERR(status))
        return status;

    if (inb(ATA_STATUS) & (ATA_STATUS_ERR | ATA_STATUS_DF))
        return Nio;

    return NSTATUS_OK;
}
