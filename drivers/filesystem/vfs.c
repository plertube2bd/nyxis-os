//

#include "vfs.h"
#include "memory.h"
#include "string.h"

#define VFS_MAX_DENTRIES    64
#define VFS_MAX_VNODES      128
#define VFS_MAX_HANDLES     64
#define VFS_MAX_NAMESPACES  16
#define VFS_NAME_MAX        255

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

typedef struct vfs_namespace {
    char name[VFS_NAME_MAX + 1];
    dentry_t* root;
    dentry_t* cwd;
    spinlock_t lock;
    bool in_use;
} vfs_namespace_t;

static dentry_t g_dentries[VFS_MAX_DENTRIES];
static handle_t g_handles[VFS_MAX_HANDLES];
static vfs_namespace_t g_namespaces[VFS_MAX_NAMESPACES];
static vfs_namespace_t* g_default_namespace = nNULL;
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

static vnode_t g_vnodes[VFS_MAX_VNODES];

static vnode_t* alloc_vnode(void) {
    for (u32 i = 0; i < VFS_MAX_VNODES; i++) {
        if (g_vnodes[i].refcnt.value != 0) {
            continue;
        }

        memset(&g_vnodes[i], 0, sizeof(vnode_t));
        atomic_set(&g_vnodes[i].refcnt, 1);
        return &g_vnodes[i];
    }

    return nNULL;
}

static void free_vnode(vnode_t* vnode) {
    if (!vnode) {
        return;
    }

    atomic_set(&vnode->refcnt, 0);
    vnode->ops = nNULL;
    vnode->sb = nNULL;
    vnode->fs_private = nNULL;
}

vnode_t* vfs_alloc_vnode(void) {
    return alloc_vnode();
}

Nstatus vfs_init_vnode(
    vnode_t* vnode,
    u64 inode_id,
    u32 type,
    vfs_ops_t* ops,
    superblock_t* sb,
    void* fs_private
) {
    if (!vnode || !ops) {
        return NinvalidArg;
    }

    vnode->inode_id = inode_id;
    vnode->type = type;
    vnode->ops = ops;
    vnode->sb = sb;
    vnode->fs_private = fs_private;
    return Nok;
}

void vfs_free_vnode(vnode_t* vnode) {
    free_vnode(vnode);
}

void* vfs_get_vnode_private(vnode_t* vnode) {
    if (!vnode) {
        return nNULL;
    }
    return vnode->fs_private;
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

static bool is_empty_string(const char* string) {
    return !string || string[0] == '\0';
}

static Nstatus parse_namespace_path(const char* path, char* out_ns, usize max_ns, const char** out_local_path) {
    if (!path || !out_ns || max_ns == 0 || !out_local_path) {
        return NinvalidArg;
    }

    out_ns[0] = '\0';
    *out_local_path = path;

    if (path[0] == '/') {
        return Nok;
    }

    const char* cursor = path;
    usize ns_len = 0;

    while (*cursor) {
        const char* colon = strchr(cursor, ':');
        if (!colon) {
            break;
        }

        if (colon[1] == '/') {
            usize segment_len = (usize)(colon - cursor);
            if (ns_len + segment_len >= max_ns) {
                return NpathTooLong;
            }

            if (ns_len > 0) {
                if (ns_len + 1 >= max_ns) {
                    return NpathTooLong;
                }
                out_ns[ns_len++] = ':';
            }
            memcpy(out_ns + ns_len, cursor, segment_len);
            ns_len += segment_len;
            out_ns[ns_len] = '\0';
            *out_local_path = colon + 1;

            if (**out_local_path == '\0') {
                *out_local_path = "/";
            }
            return Nok;
        }

        usize segment_len = (usize)(colon - cursor);
        if (ns_len + segment_len >= max_ns) {
            return NpathTooLong;
        }

        if (ns_len > 0) {
            if (ns_len + 1 >= max_ns) {
                return NpathTooLong;
            }
            out_ns[ns_len++] = ':';
        }

        memcpy(out_ns + ns_len, cursor, segment_len);
        ns_len += segment_len;
        cursor = colon + 1;
    }

    return Nok;
}

static Nstatus parse_segment(const char** cursor, char* out_name, usize max_len) {
    if (!cursor || !*cursor || !out_name || max_len == 0) {
        return NinvalidArg;
    }

    const char* source = *cursor;

    while (*source == '/') {
        source++;
    }

    if (*source == '\0') {
        out_name[0] = '\0';
        *cursor = source;
        return Nok;
    }

    usize len = 0;
    while (*source != '\0' && *source != '/') {
        if (len + 1 >= max_len) {
            return NpathTooLong;
        }
        out_name[len++] = *source++;
    }

    out_name[len] = '\0';

    while (*source == '/') {
        source++;
    }

    *cursor = source;
    return Nok;
}

static vfs_namespace_t* find_namespace(const char* name) {
    for (u32 i = 0; i < VFS_MAX_NAMESPACES; i++) {
        if (!g_namespaces[i].in_use) {
            continue;
        }

        if (strcmp(g_namespaces[i].name, name) == 0) {
            return &g_namespaces[i];
        }
    }

    return nNULL;
}

static vfs_namespace_t* create_namespace(const char* name, dentry_t* root) {
    for (u32 i = 0; i < VFS_MAX_NAMESPACES; i++) {
        if (!g_namespaces[i].in_use) {
            memset(&g_namespaces[i], 0, sizeof(vfs_namespace_t));
            g_namespaces[i].in_use = true;
            if (name && name[0] != '\0') {
                strncpy(g_namespaces[i].name, name, VFS_NAME_MAX);
                g_namespaces[i].name[VFS_NAME_MAX] = '\0';
            }
            g_namespaces[i].root = root;
            g_namespaces[i].cwd = root;
            spin_lock(&g_namespaces[i].lock);
            spin_unlock(&g_namespaces[i].lock);
            return &g_namespaces[i];
        }
    }

    return nNULL;
}

static vfs_namespace_t* get_namespace(const char* name, bool create) {
    if (is_empty_string(name)) {
        return g_default_namespace;
    }

    vfs_namespace_t* ns = find_namespace(name);
    if (ns || !create) {
        return ns;
    }

    dentry_t* root = alloc_dentry("", nNULL, nNULL);
    if (!root) {
        return nNULL;
    }

    ns = create_namespace(name, root);
    if (!ns) {
        free_dentry(root);
        return nNULL;
    }

    return ns;
}

static Nstatus resolve_namespace_path(const char* path, vfs_namespace_t** out_ns, const char** out_local_path, bool create_ns) {
    if (!path || !out_ns || !out_local_path) {
        return NinvalidArg;
    }

    char namespace_name[VFS_NAME_MAX + 1];
    Nstatus status = parse_namespace_path(path, namespace_name, sizeof(namespace_name), out_local_path);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    *out_ns = get_namespace(namespace_name, create_ns);
    if (!*out_ns) {
        return NnotFound;
    }

    return Nok;
}

static Nstatus resolve_path_internal_ns(
    vfs_namespace_t* ns,
    const char* path,
    dentry_t** out_dentry,
    bool create_missing
) {
    if (!ns || !path || !out_dentry) {
        return NinvalidArg;
    }

    dentry_t* current = (path[0] == '/') ? ns->root : ns->cwd;
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

static Nstatus resolve_parent_ns(
    vfs_namespace_t* ns,
    const char* path,
    dentry_t** out_parent,
    char* out_name,
    usize max_len
) {
    if (!ns || !path || !out_parent || !out_name || max_len == 0) {
        return NinvalidArg;
    }

    dentry_t* current = (path[0] == '/') ? ns->root : ns->cwd;
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
    memset(g_namespaces, 0, sizeof(g_namespaces));
    memset(&g_vfs_lock, 0, sizeof(spinlock_t));

    dentry_t* root = alloc_dentry("", nNULL, nNULL);
    if (!root) {
        return NoutOfMemory;
    }

    g_default_namespace = create_namespace("", root);
    if (!g_default_namespace) {
        free_dentry(root);
        return NoutOfMemory;
    }

    return Nok;
}

Nstatus vfs_register_namespace(const char* namespace_name, const char* source_path) {
    if (!namespace_name || !source_path) {
        return NinvalidArg;
    }

    vfs_namespace_t* source_ns = nNULL;
    const char* local_path = nNULL;
    Nstatus status = resolve_namespace_path(source_path, &source_ns, &local_path, false);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    dentry_t* root = nNULL;
    status = resolve_path_internal_ns(source_ns, local_path, &root, false);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (!root) {
        return NnotFound;
    }

    vfs_namespace_t* target_ns = get_namespace(namespace_name, true);
    if (!target_ns) {
        return NtooManyFileSystem;
    }

    target_ns->root = root;
    target_ns->cwd = root;
    return Nok;
}

Nstatus vfs_mount(const char* path, vfs_filesystem_t* fs) {
    if (!path || !fs || !fs->root || !fs->ops) {
        return NinvalidArg;
    }

    if (strlen(path) == 0) {
        return NinvalidArg;
    }

    spin_lock(&g_vfs_lock);
    vfs_namespace_t* ns = nNULL;
    const char* local_path = nNULL;
    Nstatus status = resolve_namespace_path(path, &ns, &local_path, true);
    if (NSTATUS_IS_ERR(status)) {
        spin_unlock(&g_vfs_lock);
        return status;
    }

    if (strcmp(local_path, "/") == 0 || local_path[0] == '\0') {
        if (ns->root->node) {
            spin_unlock(&g_vfs_lock);
            return NalreadyExists;
        }

        ns->root->node = fs->root;
        spin_unlock(&g_vfs_lock);
        return Nok;
    }

    dentry_t* parent = nNULL;
    char mount_name[VFS_NAME_MAX + 1];
    status = resolve_parent_ns(ns, local_path, &parent, mount_name, sizeof(mount_name));
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
    vfs_namespace_t* ns = nNULL;
    const char* local_path = nNULL;
    Nstatus status = resolve_namespace_path(path, &ns, &local_path, false);
    if (NSTATUS_IS_ERR(status)) {
        spin_unlock(&g_vfs_lock);
        return status;
    }

    if (strcmp(local_path, "/") == 0 || local_path[0] == '\0') {
        if (!ns->root->node) {
            spin_unlock(&g_vfs_lock);
            return NnotFound;
        }

        ns->root->node = nNULL;
        spin_unlock(&g_vfs_lock);
        return Nok;
    }

    dentry_t* mount_point = nNULL;
    status = resolve_path_internal_ns(ns, local_path, &mount_point, false);
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

static Nstatus resolve_path_internal(const char* path, dentry_t** out_dentry, bool create_missing) {
    if (!path || !out_dentry) {
        return NinvalidArg;
    }

    vfs_namespace_t* ns = nNULL;
    const char* local_path = nNULL;
    Nstatus status = resolve_namespace_path(path, &ns, &local_path, false);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    return resolve_path_internal_ns(ns, local_path, out_dentry, create_missing);
}

static Nstatus resolve_parent(const char* path, dentry_t** out_parent, char* out_name, usize max_len) {
    if (!path || !out_parent || !out_name || max_len == 0) {
        return NinvalidArg;
    }

    vfs_namespace_t* ns = nNULL;
    const char* local_path = nNULL;
    Nstatus status = resolve_namespace_path(path, &ns, &local_path, false);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    return resolve_parent_ns(ns, local_path, out_parent, out_name, max_len);
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


