#pragma once

#include "include/nyxis.h"

#define FILESYSTEM_MAX       32
#define FILESYSTEM_NAME_MAX  32

typedef struct {
    Nstatus (*mount)(u32 diskno, void* userdata);
    Nstatus (*unmount)(u32 diskno);
    Nstatus (*format)(u32 diskno);
} filesystem_ops_t;

typedef struct {
    char name[FILESYSTEM_NAME_MAX];
    filesystem_ops_t ops;
    bool is_used;
} filesystem_t;

Nstatus filesystem_register(
    const char* name,
    filesystem_ops_t ops
);

Nstatus filesystem_unregister(
    const char* name
);

filesystem_t* filesystem_get(
    const char* name
);

Nstatus filesystem_mount(
    const char* fsname,
    u32 diskno,
    void* userdata
);

Nstatus filesystem_unmount(
    const char* fsname,
    u32 diskno
);

Nstatus filesystem_format(
    const char* fsname,
    u32 diskno
);