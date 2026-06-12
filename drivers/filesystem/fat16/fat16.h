#ifndef FAT16_H
#define FAT16_H

#include "types.h"

typedef Nstatus (*fat16_disk_read_fn)(u32 diskno, usize offset, void* buffer, usize size, NTBLI* info);
typedef Nstatus (*fat16_disk_write_fn)(u32 diskno, usize offset, const void* buffer, usize size, NTBLI* info);

typedef struct {
    const char* mount_path;
    NTBLI* info;
    fat16_disk_read_fn read;
    fat16_disk_write_fn write;
} fat16_mount_params_t;

Nstatus fat16_register(void);
Nstatus fat16_mount(u32 diskno, void* userdata);
Nstatus fat16_unmount(u32 diskno);

#endif // FAT16_H
