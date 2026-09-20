/*
 * ramdisk.h - 메모리 영역을 블록 장치처럼 다루는 램디스크
 */
#ifndef RAMDISK_H
#define RAMDISK_H

#include "types.h"
#include "nyxis.h"
#include "memory.h"

typedef struct {
    u32 diskno;
    u64 offset;      /* 램디스크가 시작하는 물리 주소 */
    u64 size;
    bool is_used;
} ramdisk_t;

#define RAMDISK_MAX 16

Nstatus ramdisk_init(u32 diskno, usize ramdisk_start, usize ramdisk_size, NTBLI *info);
Nstatus ramdisk_read(u32 diskno, usize offset, void *buffer, usize size, NTBLI *info);
Nstatus ramdisk_write(u32 diskno, usize offset, const void *buffer, usize size, NTBLI *info);
Nstatus ramdisk_deinit(u32 diskno);
Nstatus ramdisk_format(u32 diskno, NTBLI *info);

#endif /* RAMDISK_H */
