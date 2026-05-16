#include "drivers/ramdisk/ramdisk.h"

#include "include/nyxis.h"
#include "include/memory.h"
#include "include/interrupt.h"

static ramdisk_t g_ramdisks[RAMDISK_MAX];

static ramdisk_t* ramdisk_find(u32 diskno) {
    for (u32 i = 0; i < RAMDISK_MAX; i++) {
        if (g_ramdisks[i].is_used &&
            g_ramdisks[i].diskno == diskno) {
            return &g_ramdisks[i];
        }
    }

    return nNULL;
}

Nstatus ramdisk_init(
    u32 diskno,
    usize ramdisk_start,
    usize ramdisk_size,
    NTBLI *info
) {
    if (ramdisk_size == 0) {
        return NinvalidArg;
    }

    if (ramdisk_start + ramdisk_size > info->memory_size) {
        return NoutOfMemory;
    }

    if (ramdisk_find(diskno)) {
        return NalreadyExists;
    }

    for (u32 i = 0; i < RAMDISK_MAX; i++) {
        if (!g_ramdisks[i].is_used) {

            g_ramdisks[i].diskno  = diskno;
            g_ramdisks[i].offset  = ramdisk_start;
            g_ramdisks[i].size    = ramdisk_size;
            g_ramdisks[i].is_used = true;

            return Nok;
        }
    }

    return NtooManyFileSystem;
}

Nstatus ramdisk_read(
    u32 diskno,
    usize offset,
    void* buffer,
    usize size,
    NTBLI *info
) {
    ramdisk_t* rd = ramdisk_find(diskno);

    if (!rd) {
        return NnotFound;
    }

    if (!buffer || size == 0) {
        return NinvalidArg;
    }

    if (offset + size > rd->size) {
        return Noverflow;
    }

    usize buf = (usize)buffer;

    if (buf + size > info->memory_size) {
        return NinvalidPointer;
    }

    memcpy(
        buffer,
        (void*)(rd->offset + offset),
        size
    );

    return Nok;
}

Nstatus ramdisk_write(
    u32 diskno,
    usize offset,
    const void* buffer,
    usize size,
    NTBLI *info
) {
    ramdisk_t* rd = ramdisk_find(diskno);

    if (!rd) {
        return NnotFound;
    }

    if (!buffer || size == 0) {
        return NinvalidArg;
    }

    if (offset + size > rd->size) {
        return Noverflow;
    }

    usize buf = (usize)buffer;

    if (buf + size > info->memory_size) {
        return NinvalidPointer;
    }

    memcpy(
        (void*)(rd->offset + offset),
        buffer,
        size
    );

    return Nok;
}

Nstatus ramdisk_format(u32 diskno) {
    ramdisk_t* rd = ramdisk_find(diskno);

    if (!rd) {
        return NnotFound;
    }

    memset(
        (void*)rd->offset,
        0,
        rd->size
    );

    return Nok;
}

Nstatus ramdisk_deinit(u32 diskno) {
    ramdisk_t* rd = ramdisk_find(diskno);

    if (!rd) {
        return NnotFound;
    }

    rd->is_used = false;

    rd->diskno = 0;
    rd->offset = 0;
    rd->size   = 0;

    return Nok;
}