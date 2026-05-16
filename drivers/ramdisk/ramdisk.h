#ifndef RAMDISK_H
#define RAMDISK_H

#include "include/types.h"
#include "include/nyxis.h"
#include "include/memory.h"

typedef struct {
    u32 diskno;
    u64 offset;
    u64 size;
    bool is_used;
} ramdisk_t;

#define RAMDISK_MAX 16

Nstatus ramdisk_init(u32 diskno, usize ramdisk_start, usize ramdisk_size, NTBLI *info);
Nstatus ramdisk_read(u32 diskno, usize offset, void* buffer, usize size, NTBLI *info);
Nstatus ramdisk_write(u32 diskno, usize offset, const void* buffer, usize size, NTBLI *info);
Nstatus ramdisk_deinit(u32 diskno);
Nstatus ramdisk_format(u32 diskno, NTBLI *info);

#endif // RAMDISK_H