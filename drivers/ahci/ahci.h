/*
 * ahci.h - AHCI(SATA) 컨트롤러 드라이버
 *
 * [수정 이력 요약]
 *  - FIS_REG_H2D 의 u8 비트필드(GCC 확장, C89 비표준)를 일반 바이트(pmport_c)로 변경.
 *    (C 표준상 비트필드 배치는 구현 정의이므로, 하드웨어 레지스터/FIS 형식에는 비트필드를
 *     쓰지 않는 것이 안전하다)
 *  - '#endif // NAME' 형태의 라인 주석 제거 (C89).
 */
#ifndef AHCI_H
#define AHCI_H

#include "nyxis.h"

#define HBA_PORT_DET_PRESENT 3
#define HBA_PORT_IPM_ACTIVE  1

#define SATA_SIG_ATA 0x00000101U

#define ATA_CMD_READ_DMA_EX 0x25

#define HBA_PX_CMD_ST   (1U << 0)
#define HBA_PX_CMD_FRE  (1U << 4)
#define HBA_PX_CMD_FR   (1U << 14)
#define HBA_PX_CMD_CR   (1U << 15)

#define HBA_GHC_AE      (1U << 0)
#define HBA_GHC_IE      (1U << 1)
#define HBA_GHC_HR      (1U << 2)

#define HBA_CAP_S64A    (1U << 31)   /* 64비트 DMA 주소 지원 */

#define FIS_TYPE_REG_H2D 0x27
#define FIS_H2D_C        0x80        /* pmport_c 의 bit7: Command 레지스터 갱신 */

#define AHCI_MAX_PORTS 32
#define AHCI_MAX_CMD_SLOTS 32

/* PRDT 항목 하나가 전송할 수 있는 최대 크기(4MiB) 를 섹터 수로 환산한 값 */
#define AHCI_MAX_SECTORS_PER_READ 8192U

typedef volatile struct {
    u32 clb;
    u32 clbu;
    u32 fb;
    u32 fbu;
    u32 is;
    u32 ie;
    u32 cmd;
    u32 rsv0;
    u32 tfd;
    u32 sig;
    u32 ssts;
    u32 sctl;
    u32 serr;
    u32 sact;
    u32 ci;
    u32 sntf;
    u32 fbs;
    u32 rsv1[11];
    u32 vendor[4];
} HBA_PORT;

typedef volatile struct {
    u32 cap;
    u32 ghc;
    u32 is;
    u32 pi;
    u32 vs;
    u32 ccc_ctl;
    u32 ccc_pts;
    u32 em_loc;
    u32 em_ctl;
    u32 cap2;
    u32 bohc;

    u8  rsv[0xA0 - 0x2C];
    u8  vendor[0x100 - 0xA0];

    HBA_PORT ports[32];
} HBA_MEM;

typedef struct {
    u8 cfis[64];
    u8 acmd[16];
    u8 rsv[48];
} HBA_CMD_TBL;

typedef struct {
    u16 flags;
    u16 prdtl;
    u32 prdbc;
    u32 ctba;
    u32 ctbau;
    u32 rsv1[4];
} HBA_CMD_HEADER;

typedef struct {
    u32 dba;
    u32 dbau;
    u32 rsv0;
    u32 dbc;
} HBA_PRDT_ENTRY;

/* Register Host-to-Device FIS (20바이트) */
typedef struct {
    u8 fis_type;
    u8 pmport_c;    /* bit7 = C, bit3:0 = port multiplier port */

    u8 command;
    u8 featurel;

    u8 lba0;
    u8 lba1;
    u8 lba2;
    u8 device;

    u8 lba3;
    u8 lba4;
    u8 lba5;
    u8 featureh;

    u8 countl;
    u8 counth;
    u8 icc;
    u8 control;

    u8 rsv1[4];
} FIS_REG_H2D;

NX_STATIC_ASSERT(fis_h2d_size, sizeof(FIS_REG_H2D) == 20);
NX_STATIC_ASSERT(hba_port_size, sizeof(HBA_PORT) == 0x80);
NX_STATIC_ASSERT(hba_mem_size, sizeof(HBA_MEM) == 0x100 + 32 * 0x80);

Nstatus ahci_init(void);
HBA_PORT *ahci_find_port(void);
Nstatus ahci_read(
    HBA_PORT *Port,
    u64 StartLba,
    u32 SectorCount,
    void *Buffer
);

#endif /* AHCI_H */
