//

#include "vfs.h"
#include "memory.h"
#include "string.h"

#define VFS_MAX_DENTRIES 64
#define VFS_MAX_HANDLES  64
#define VFS_NAME_MAX     255

typedef struct vnode {
    u64 inode_id;
    u32 type;

    vfs_ops_t* ops;
    superblock_t* sb;

    atomic_t refcnt;
    void* fs_private;
} vnode_t;

struct dentry {
    char name[VFS_NAME_MAX + 1];
    vnode_t* node;
    dentry_t* parent;
    dentry_t* first_child;
    dentry_t* next_sibling;
    atomic_t refcnt;
};

typedef struct namespace {
    dentry_t* root;
    dentry_t* cwd;
    spinlock_t lock;
} namespace_t;

static dentry_t g_dentries[VFS_MAX_DENTRIES];
static handle_t g_handles[VFS_MAX_HANDLES];
static namespace_t g_namespace;
static spinlock_t g_vfs_lock;

static dentry_t* alloc_dentry(const char* name, vnode_t* node, dentry_t* parent) {
    for (u32 i = 0; i < VFS_MAX_DENTRIES; i++) {
        if (g_dentries[i].refcnt.value != 0) {
            continue;
        }

        memset(&g_dentries[i], 0, sizeof(dentry_t));
        strncpy(g_dentries[i].name, name, VFS_NAME_MAX);
        g_dentries[i].name[VFS_NAME_MAX] = '\0';
        g_dentries[i].node = node;
        g_dentries[i].parent = parent;
        atomic_set(&g_dentries[i].refcnt, 1);
        return &g_dentries[i];
    }

    return nNULL;
}

static void free_dentry(dentry_t* dentry) {
    if (!dentry) {
        return;
    }

    atomic_set(&dentry->refcnt, 0);
    dentry->node = nNULL;
    dentry->parent = nNULL;
    dentry->first_child = nNULL;
    dentry->next_sibling = nNULL;
}

static void attach_child(dentry_t* parent, dentry_t* child) {
    if (!parent || !child) {
        return;
    }

    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

static void detach_child(dentry_t* parent, dentry_t* child) {
    if (!parent || !child) {
        return;
    }

    dentry_t* current = parent->first_child;
    dentry_t* previous = nNULL;

    while (current) {
        if (current == child) {
            if (previous) {
                previous->next_sibling = current->next_sibling;
            } else {
                parent->first_child = current->next_sibling;
            }
            current->next_sibling = nNULL;
            return;
        }

        previous = current;
        current = current->next_sibling;
    }
}

static dentry_t* find_child(dentry_t* parent, const char* name) {
    if (!parent || !name) {
        return nNULL;
    }

    dentry_t* child = parent->first_child;
    while (child) {
        if (strcmp(child->name, name) == 0) {
            return child;
        }
        child = child->next_sibling;
    }

    return nNULL;
}

static handle_t* alloc_handle(void) {
    for (u32 i = 0; i < VFS_MAX_HANDLES; i++) {
        if (g_handles[i].refcnt.value != 0) {
            continue;
        }

        memset(&g_handles[i], 0, sizeof(handle_t));
        atomic_set(&g_handles[i].refcnt, 1);
        return &g_handles[i];
    }

    return nNULL;
}

static void free_handle(handle_t* handle) {
    if (!handle) {
        return;
    }

    atomic_set(&handle->refcnt, 0);
    handle->vnode = nNULL;
    handle->fs_private = nNULL;
    handle->offset = 0;
    handle->flags = 0;
}

static Nstatus parse_segment(const char** path, char* out_name, usize max_len) {
    if (!path || !*path || !out_name || max_len == 0) {
        return NinvalidArg;
    }

    while (**path == '/') {
        (*path)++;
    }

    if (**path == '\0') {
        out_name[0] = '\0';
        return NnotFound;
    }

    usize idx = 0;
    while (**path != '/' && **path != '\0') {
        if (idx + 1 >= max_len) {
            return NpathTooLong;
        }

        out_name[idx++] = **path;
        (*path)++;
    }

    out_name[idx] = '\0';
    return Nok;
}

static Nstatus resolve_path_internal(const char* path, dentry_t** out_dentry, bool create_missing) {
    if (!path || !out_dentry) {
        return NinvalidArg;
    }

    dentry_t* current = (path[0] == '/') ? g_namespace.root : g_namespace.cwd;
    const char* cursor = path;

    if (*cursor == '/') {
        cursor++;
    }

    while (*cursor != '\0') {
        char name[VFS_NAME_MAX + 1];
        Nstatus status = parse_segment(&cursor, name, sizeof(name));
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        if (name[0] == '\0') {
            break;
        }

        dentry_t* child = find_child(current, name);
        if (!child) {
            if (!current->node || !current->node->ops || !current->node->ops->lookup) {
                return NfileNotFound;
            }

            vnode_t* vnode = nNULL;
            status = current->node->ops->lookup(current->node, name, &vnode);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            if (!vnode) {
                return NfileNotFound;
            }

            if (!create_missing) {
                return NfileNotFound;
            }

            child = alloc_dentry(name, vnode, current);
            if (!child) {
                return NoutOfMemory;
            }
            attach_child(current, child);
        }

        current = child;
    }

    *out_dentry = current;
    return Nok;
}

static Nstatus resolve_parent(const char* path, dentry_t** out_parent, char* out_name, usize max_len) {
    if (!path || !out_parent || !out_name || max_len == 0) {
        return NinvalidArg;
    }

    dentry_t* current = (path[0] == '/') ? g_namespace.root : g_namespace.cwd;
    const char* cursor = path;

    if (*cursor == '/') {
        cursor++;
    }

    while (*cursor != '\0') {
        char name[VFS_NAME_MAX + 1];
        Nstatus status = parse_segment(&cursor, name, sizeof(name));
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        if (name[0] == '\0') {
            break;
        }

        const char* next_cursor = cursor;
        while (*next_cursor == '/') {
            next_cursor++;
        }

        if (*next_cursor == '\0') {
            strncpy(out_name, name, max_len);
            out_name[max_len - 1] = '\0';
            *out_parent = current;
            return Nok;
        }

        dentry_t* child = find_child(current, name);
        if (!child) {
            if (!current->node || !current->node->ops || !current->node->ops->lookup) {
                return NnotFound;
            }

            vnode_t* vnode = nNULL;
            status = current->node->ops->lookup(current->node, name, &vnode);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            if (!vnode) {
                return NnotFound;
            }

            child = alloc_dentry(name, vnode, current);
            if (!child) {
                return NoutOfMemory;
            }
            attach_child(current, child);
        }

        current = child;
        cursor = next_cursor;
    }

    if (cursor == path || *path == '\0') {
        out_name[0] = '\0';
        *out_parent = current;
        return Nok;
    }

    return NnotFound;
}

Nstatus vfs_init(void) {
    memset(&g_namespace, 0, sizeof(namespace_t));
    memset(&g_vfs_lock, 0, sizeof(spinlock_t));

    dentry_t* root = alloc_dentry("", nNULL, nNULL);
    if (!root) {
        return NoutOfMemory;
    }

    g_namespace.root = root;
    g_namespace.cwd = root;
    spin_lock(&g_namespace.lock);
    spin_unlock(&g_namespace.lock);

    return Nok;
}

Nstatus vfs_mount(const char* path, filesystem_t* fs) {
    if (!path || !fs || !fs->root || !fs->ops) {
        return NinvalidArg;
    }

    if (strlen(path) == 0) {
        return NinvalidArg;
    }

    spin_lock(&g_vfs_lock);

    if (strcmp(path, "/") == 0) {
        if (g_namespace.root->node) {
            spin_unlock(&g_vfs_lock);
            return NalreadyExists;
        }

        g_namespace.root->node = fs->root;
        spin_unlock(&g_vfs_lock);
        return Nok;
    }

    dentry_t* parent = nNULL;
    char mount_name[VFS_NAME_MAX + 1];
    Nstatus status = resolve_parent(path, &parent, mount_name, sizeof(mount_name));
    if (NSTATUS_IS_ERR(status)) {
        spin_unlock(&g_vfs_lock);
        return status;
    }

    if (mount_name[0] == '\0') {
        spin_unlock(&g_vfs_lock);
        return NinvalidArg;
    }

    dentry_t* existing = find_child(parent, mount_name);
    if (existing && existing->node) {
        spin_unlock(&g_vfs_lock);
        return NalreadyExists;
    }

    if (!existing) {
        existing = alloc_dentry(mount_name, fs->root, parent);
        if (!existing) {
            spin_unlock(&g_vfs_lock);
            return NoutOfMemory;
        }
        attach_child(parent, existing);
    } else {
        existing->node = fs->root;
    }

    spin_unlock(&g_vfs_lock);
    return Nok;
}

Nstatus vfs_unmount(const char* path) {
    if (!path || strlen(path) == 0) {
        return NinvalidArg;
    }

    spin_lock(&g_vfs_lock);

    if (strcmp(path, "/") == 0) {
        if (!g_namespace.root->node) {
            spin_unlock(&g_vfs_lock);
            return NnotFound;
        }

        g_namespace.root->node = nNULL;
        spin_unlock(&g_vfs_lock);
        return Nok;
    }

    dentry_t* mount_point = nNULL;
    Nstatus status = resolve_path_internal(path, &mount_point, false);
    if (NSTATUS_IS_ERR(status)) {
        spin_unlock(&g_vfs_lock);
        return status;
    }

    if (!mount_point || !mount_point->node) {
        spin_unlock(&g_vfs_lock);
        return NnotFound;
    }

    dentry_t* parent = mount_point->parent;
    if (parent) {
        detach_child(parent, mount_point);
        free_dentry(mount_point);
    } else {
        mount_point->node = nNULL;
    }

    spin_unlock(&g_vfs_lock);
    return Nok;
}

Nstatus vfs_open(const char* path, u32 flags, handle_t* out_handle) {
    if (!path || !out_handle) {
        return NinvalidArg;
    }

    dentry_t* target = nNULL;
    Nstatus status = resolve_path_internal(path, &target, true);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (!target || !target->node || !target->node->ops || !target->node->ops->open) {
        return Nunsupported;
    }

    handle_t* handle = alloc_handle();
    if (!handle) {
        return NoutOfMemory;
    }

    handle->vnode = target->node;
    handle->flags = flags;
    handle->offset = 0;
    atomic_set(&handle->refcnt, 1);

    status = handle->vnode->ops->open(handle->vnode, flags, handle);
    if (NSTATUS_IS_ERR(status)) {
        free_handle(handle);
        return status;
    }

    *out_handle = *handle;
    free_handle(handle);
    return Nok;
}

Nstatus vfs_read(handle_t* handle, void* buffer, usize size, usize* bytes_read) {
    if (!handle || !buffer || !bytes_read) {
        return NinvalidArg;
    }

    if (!handle->vnode || !handle->vnode->ops || !handle->vnode->ops->read) {
        return Nunsupported;
    }

    Nstatus status = handle->vnode->ops->read(handle, buffer, size, bytes_read);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    handle->offset += *bytes_read;
    return Nok;
}

Nstatus vfs_write(handle_t* handle, const void* buffer, usize size, usize* bytes_written) {
    if (!handle || !buffer || !bytes_written) {
        return NinvalidArg;
    }

    if (!handle->vnode || !handle->vnode->ops || !handle->vnode->ops->write) {
        return Nunsupported;
    }

    Nstatus status = handle->vnode->ops->write(handle, buffer, size, bytes_written);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    handle->offset += *bytes_written;
    return Nok;
}

Nstatus vfs_close(handle_t* handle) {
    if (!handle) {
        return NinvalidArg;
    }

    if (!handle->vnode || !handle->vnode->ops || !handle->vnode->ops->close) {
        return Nunsupported;
    }

    Nstatus status = handle->vnode->ops->close(handle);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    free_handle(handle);
    return Nok;
}


