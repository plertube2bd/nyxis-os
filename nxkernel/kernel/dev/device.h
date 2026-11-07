/*
 * device.h - dev:/ 이름공간의 장치 객체 (규칙은 include/nyx_abi.h 의 "장치" 참고)
 *
 * 혼합 방식 (설계 결정):
 *  - 직접 접근: 프레임버퍼(MMIO 매핑), COM1(I/O 포트를 TSS 비트맵으로 ring3 에 허가). DMA 가 없는 장치만.
 *  - 커널 경유: 블록 장치(RAM 디스크) 섹터 읽기/쓰기. 마운트된 장치는 읽기만.
 *  - 노출하지 않음: PIC, PIT, PCI 설정 공간, DMA 장치 레지스터.
 *  - 여는 것은 uid 0 만 (nspath 가 dev:/ 를 root 전용으로 해석한다).
 *
 * 소유권: 프레임버퍼/COM1 은 연 프로세스가 독점한다 (두 번째 열기는 Nbusy). 그동안 커널은 그 장치를 쓰지
 * 않는다 (printk/stdin 이 비켜 준다). 블록 장치는 쓰기 열기가 독점이고 읽기 열기는 여럿이 가능하다.
 * 핸들을 닫거나 프로세스가 끝나면(죽어도 — 핸들 정리 경로) 모든 것이 커널로 돌아온다: 매핑 해제,
 * 포트 권한 회수, printk/stdin 재개. 장치 핸들은 복제할 수 없다 (소유권을 단순하게 유지).
 */
#ifndef KERNEL_DEV_DEVICE_H
#define KERNEL_DEV_DEVICE_H

#include "nyxis.h"
#include "nyx_abi.h"

struct process;

typedef struct nx_device {
    const char *name;
    u32  kind;                  /* NX_DEV_* */
    bool mounted;               /* 블록: 마운트된 파일시스템이 쓰는 장치 (쓰기 금지) */
    u32  diskno;                /* 블록: ramdisk 번호 */
    u64  size;                  /* 블록/프레임버퍼 바이트 수 */
    u64  phys;                  /* 프레임버퍼 물리 주소 */
    u32  width, height, pitch, bpp;
    u16  io_base, io_count;     /* 스트림: I/O 포트 범위 */

    struct process *owner;      /* 독점 소유자 (없으면 NULL) */
    u32  readers;               /* 블록: 읽기 전용으로 연 수 */
    u64  mapped_va;             /* 프레임버퍼: 소유자 주소 공간에 매핑된 주소 (0 = 없음) */
} nx_device_t;

/* 부팅 때 장치들을 등록한다 (boot info 의 프레임버퍼, COM1, 루트 RAM 디스크(마운트됨), rd0) */
void device_init(const NTBLI *info, u32 rootdisk_no, u64 rootdisk_size);

/* name 의 장치를 현재 프로세스 이름으로 연다. write 이면 쓰기 열기. 독점 규칙 위반이면 Nbusy */
Nstatus device_open(const char *name, bool write, nx_device_t **out);

/* 연 것을 되돌린다. owner 는 그 핸들을 가진 프로세스 (종료 중일 수 있다) */
void device_close(nx_device_t *dev, struct process *owner, bool write);

/* 블록/스트림 I/O. *offset 은 블록 장치에서만 쓰고 갱신한다 */
Nstatus device_read(nx_device_t *dev, u64 *offset, void *buf, usize len, usize *got);
Nstatus device_write(nx_device_t *dev, struct process *p, bool write_open, u64 *offset,
                     const void *buf, usize len, usize *done);

/* 소유자 p 의 주소 공간에 프레임버퍼를 매핑하고 사용자 주소를 돌려준다 (이미 했으면 같은 주소) */
Nstatus device_map_mmio(nx_device_t *dev, struct process *p, u64 *out_va);

/* NX_DEVCMD_INFO 내용 */
void device_info(const nx_device_t *dev, struct nx_devinfo *out);

/* NX_DEVCMD_IOPERM: 소유자 p 에게 이 장치의 포트를 허가(on)하거나 회수 */
Nstatus device_ioperm(nx_device_t *dev, struct process *p, bool on);

#endif /* KERNEL_DEV_DEVICE_H */
