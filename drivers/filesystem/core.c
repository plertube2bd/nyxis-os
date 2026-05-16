#include "drivers/filesystem/core.h"

#include "include/nyxis.h"
#include "include/memory.h"
#include "include/string.h"

static filesystem_t g_filesystems[FILESYSTEM_MAX];

static filesystem_t* fs_find(const char* name) {
    if (!name) {
        return nNULL;
    }

    for (u32 i = 0; i < FILESYSTEM_MAX; i++) {
        if (!g_filesystems[i].is_used) {
            continue;
        }

        if (strcmp(g_filesystems[i].name, name) == 0) {
            return &g_filesystems[i];
        }
    }

    return nNULL;
}

Nstatus filesystem_register(
    const char* name,
    filesystem_ops_t ops
) {
    if (!name) {
        return NinvalidArg;
    }

    if (strlen(name) >= FILESYSTEM_NAME_MAX) {
        return Noverflow;
    }

    if (fs_find(name)) {
        return NalreadyExists;
    }

    for (u32 i = 0; i < FILESYSTEM_MAX; i++) {
        if (!g_filesystems[i].is_used) {

            memset(&g_filesystems[i], 0, sizeof(filesystem_t));

            strcpy(g_filesystems[i].name, name);
            g_filesystems[i].ops = ops;
            g_filesystems[i].is_used = true;

            return Nok;
        }
    }

    return NtooManyFileSystem;
}

Nstatus filesystem_unregister(const char* name) {
    filesystem_t* fs = fs_find(name);

    if (!fs) {
        return NnotFound;
    }

    memset(fs, 0, sizeof(filesystem_t));

    return Nok;
}

filesystem_t* filesystem_get(const char* name) {
    return fs_find(name);
}

Nstatus filesystem_mount(
    const char* fsname,
    u32 diskno,
    void* userdata
) {
    filesystem_t* fs = fs_find(fsname);

    if (!fs) {
        return NnotFound;
    }

    if (!fs->ops.mount) {
        return Nunsupported;
    }

    return fs->ops.mount(diskno, userdata);
}

Nstatus filesystem_unmount(
    const char* fsname,
    u32 diskno
) {
    filesystem_t* fs = fs_find(fsname);

    if (!fs) {
        return NnotFound;
    }

    if (!fs->ops.unmount) {
        return Nunsupported;
    }

    return fs->ops.unmount(diskno);
}

Nstatus filesystem_format(
    const char* fsname,
    u32 diskno
) {
    filesystem_t* fs = fs_find(fsname);

    if (!fs) {
        return NnotFound;
    }

    if (!fs->ops.format) {
        return Nunsupported;
    }

    return fs->ops.format(diskno);
}