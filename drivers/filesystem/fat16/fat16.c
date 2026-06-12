#include "drivers/filesystem/fat16/fat16.h"

#include "drivers/filesystem/vfs.h"
#include "drivers/filesystem/core.h"
#include "drivers/ramdisk/ramdisk.h"
#include "string.h"
#include "memory.h"
#include "nyxis.h"

/*
 * FAT16 filesystem implementation.
 *
 * This file contains the internal implementation of the FAT16 driver:
 * - parsing the boot sector
 * - reading FAT entries and clusters
 * - traversing the root and subdirectories
 * - opening and reading files
 *
 * Public API declarations are provided in drivers/filesystem/fat16/fat16.h.
 * Internal helpers and structures are kept private to this C file.
 */

#define FAT16_ENTRY_SIZE       32
#define FAT16_NAME_LENGTH      11
#define FAT16_MAX_DEVICES      8
#define FAT16_MAX_VNODE_DATA   128
#define FAT16_MAX_HANDLES      64

#define FAT16_ATTR_READ_ONLY   0x01
#define FAT16_ATTR_HIDDEN      0x02
#define FAT16_ATTR_SYSTEM      0x04
#define FAT16_ATTR_VOLUME_ID   0x08
#define FAT16_ATTR_DIRECTORY   0x10
#define FAT16_ATTR_ARCHIVE     0x20
#define FAT16_ATTR_LONG_NAME   0x0F

#define FAT16_CLUSTER_EOC      0xFFF8

typedef struct __attribute__((packed)) fat16_boot_sector {
    u8   JumpInstruction[3];
    utf8 OEMName[8];
    u16  BytesPerSector;
    u8   SectorsPerCluster;
    u16  ReservedSectorCount;
    u8   FATCount;
    u16  RootEntryCount;
    u16  TotalSectors16;
    u8   MediaType;
    u16  FATSize16;
    u16  SectorsPerTrack;
    u16  NumberOfHeads;
    u32  HiddenSectors;
    u32  TotalSectors32;
    u8   DriveNumber;
    u8   Reserved1;
    u8   BootSignature;
    u32  VolumeID;
    utf8 VolumeLabel[11];
    utf8 FileSystemType[8];
    u8   Reserved2[448];
    u16  Signature;
} fat16_boot_sector_t;

typedef struct __attribute__((packed)) fat16_dir_entry {
    utf8 Name[11];
    u8   Attr;
    u8   NTRes;
    u8   CreateTimeTenth;
    u16  CreateTime;
    u16  CreateDate;
    u16  LastAccessDate;
    u16  FirstClusterHigh;
    u16  WriteTime;
    u16  WriteDate;
    u16  FirstClusterLow;
    u32  FileSize;
} fat16_dir_entry_t;

typedef struct {
    fat16_mount_params_t params;
    bool used;
    u32  diskno;
    fat16_boot_sector_t boot;
    u32  root_dir_sectors;
    u32  first_fat_sector;
    u32  first_root_dir_sector;
    u32  first_data_sector;
    u32  bytes_per_cluster;
    u32  total_sectors;
    u32  total_clusters;
    char mount_path[256];
    vnode_t* root_vnode;
    vfs_filesystem_t fs;
    fat16_disk_read_fn read;
    fat16_disk_write_fn write;
} fat16_device_t;

typedef struct {
    fat16_device_t* dev;
    bool is_directory;
    u16  first_cluster;
    u32  size;
    u64  inode_id;
} fat16_vnode_data_t;

typedef struct {
    bool in_use;
    fat16_vnode_data_t entry;
} fat16_vnode_slot_t;

typedef struct {
    bool in_use;
    fat16_vnode_data_t handle;
} fat16_handle_slot_t;

static fat16_device_t g_devices[FAT16_MAX_DEVICES];
static fat16_vnode_slot_t g_vnode_data[FAT16_MAX_VNODE_DATA];
static fat16_handle_slot_t g_handle_slots[FAT16_MAX_HANDLES];

static vfs_ops_t g_fat16_file_ops;
static vfs_ops_t g_fat16_dir_ops;

static u8 fat16_toupper(u8 c) {
    if (c >= 'a' && c <= 'z') {
        return c - 32;
    }
    return c;
}

static bool fat16_is_eoc(u16 cluster) {
    return cluster >= FAT16_CLUSTER_EOC;
}

static fat16_device_t* fat16_find_device(u32 diskno) {
    for (u32 i = 0; i < FAT16_MAX_DEVICES; i++) {
        if (g_devices[i].used && g_devices[i].diskno == diskno) {
            return &g_devices[i];
        }
    }
    return nNULL;
}

static fat16_device_t* fat16_alloc_device(u32 diskno) {
    for (u32 i = 0; i < FAT16_MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            memset(&g_devices[i], 0, sizeof(fat16_device_t));
            g_devices[i].used = true;
            g_devices[i].diskno = diskno;
            return &g_devices[i];
        }
    }
    return nNULL;
}

static void fat16_free_device(fat16_device_t* dev) {
    if (!dev) {
        return;
    }
    dev->used = false;
    dev->diskno = 0;
    dev->root_vnode = nNULL;
    dev->read = nNULL;
    dev->write = nNULL;
    memset(dev->mount_path, 0, sizeof(dev->mount_path));
}

static fat16_vnode_data_t* fat16_alloc_vnode_data(void) {
    for (u32 i = 0; i < FAT16_MAX_VNODE_DATA; i++) {
        if (!g_vnode_data[i].in_use) {
            g_vnode_data[i].in_use = true;
            memset(&g_vnode_data[i].entry, 0, sizeof(fat16_vnode_data_t));
            return &g_vnode_data[i].entry;
        }
    }
    return nNULL;
}

static void fat16_free_vnode_data(fat16_vnode_data_t* data) {
    if (!data) {
        return;
    }

    for (u32 i = 0; i < FAT16_MAX_VNODE_DATA; i++) {
        if (&g_vnode_data[i].entry == data) {
            g_vnode_data[i].in_use = false;
            memset(&g_vnode_data[i].entry, 0, sizeof(fat16_vnode_data_t));
            return;
        }
    }
}

static fat16_vnode_data_t* fat16_alloc_handle_data(void) {
    for (u32 i = 0; i < FAT16_MAX_HANDLES; i++) {
        if (!g_handle_slots[i].in_use) {
            g_handle_slots[i].in_use = true;
            memset(&g_handle_slots[i].handle, 0, sizeof(fat16_vnode_data_t));
            return &g_handle_slots[i].handle;
        }
    }
    return nNULL;
}

static void fat16_free_handle_data(fat16_vnode_data_t* data) {
    if (!data) {
        return;
    }

    for (u32 i = 0; i < FAT16_MAX_HANDLES; i++) {
        if (&g_handle_slots[i].handle == data) {
            g_handle_slots[i].in_use = false;
            memset(&g_handle_slots[i].handle, 0, sizeof(fat16_vnode_data_t));
            return;
        }
    }
}

static Nstatus fat16_read_raw(
    fat16_device_t* dev,
    usize offset,
    void* buffer,
    usize size
) {
    if (!dev || !buffer || size == 0) {
        return NinvalidArg;
    }

    if (!dev->read || !dev->params.info) {
        return NinvalidArg;
    }

    return dev->read(dev->diskno, offset, buffer, size, dev->params.info);
}

static Nstatus fat16_read_sector(
    fat16_device_t* dev,
    u32 sector,
    void* buffer
) {
    usize offset = (usize)sector * dev->boot.BytesPerSector;
    return fat16_read_raw(dev, offset, buffer, dev->boot.BytesPerSector);
}

static Nstatus fat16_get_fat_entry(
    fat16_device_t* dev,
    u16 cluster,
    u16* out_next
) {
    if (!dev || !out_next) {
        return NinvalidArg;
    }

    usize fat_offset = (usize)dev->first_fat_sector * dev->boot.BytesPerSector + (cluster * 2);
    u16 entry;
    Nstatus status = fat16_read_raw(dev, fat_offset, &entry, sizeof(entry));
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    *out_next = entry;
    return Nok;
}

static Nstatus fat16_get_nth_cluster(
    fat16_device_t* dev,
    u16 start_cluster,
    u32 index,
    u16* out_cluster
) {
    if (!dev || !out_cluster) {
        return NinvalidArg;
    }

    u16 cluster = start_cluster;
    if (index == 0) {
        *out_cluster = cluster;
        return Nok;
    }

    for (u32 i = 0; i < index; i++) {
        u16 next;
        Nstatus status = fat16_get_fat_entry(dev, cluster, &next);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (next == 0x0000 || next == 0xFFF7) {
            return NfileCorrupted;
        }
        if (fat16_is_eoc(next)) {
            return NnotFound;
        }
        cluster = next;
    }

    *out_cluster = cluster;
    return Nok;
}

static bool fat16_name_matches(const utf8 entry_name[11], const char* request_name) {
    if (!entry_name || !request_name) {
        return false;
    }

    utf8 normalized[FAT16_NAME_LENGTH];
    for (u32 i = 0; i < FAT16_NAME_LENGTH; i++) {
        normalized[i] = ' ';
    }

    const char* cursor = request_name;
    usize name_len = 0;
    usize ext_len = 0;

    while (*cursor && *cursor != '.') {
        if (name_len < 8) {
            normalized[name_len++] = fat16_toupper((u8)*cursor);
        }
        cursor++;
    }

    if (*cursor == '.') {
        cursor++;
        while (*cursor) {
            if (ext_len < 3) {
                normalized[8 + ext_len] = fat16_toupper((u8)*cursor);
            }
            ext_len++;
            cursor++;
        }
    }

    if (name_len > 8 || ext_len > 3) {
        return false;
    }

    for (u32 i = 0; i < FAT16_NAME_LENGTH; i++) {
        utf8 candidate = normalized[i];
        utf8 actual = entry_name[i];
        if (actual >= 'a' && actual <= 'z') {
            actual -= 32;
        }
        if (candidate != actual) {
            return false;
        }
    }
    return true;
}

static Nstatus fat16_fill_vnode(
    vnode_t* node,
    fat16_device_t* dev,
    bool is_directory,
    u16 first_cluster,
    u32 size,
    u64 inode_id
) {
    if (!node || !dev) {
        return NinvalidArg;
    }

    fat16_vnode_data_t* vnode_data = fat16_alloc_vnode_data();
    if (!vnode_data) {
        return NoutOfMemory;
    }

    vnode_data->dev = dev;
    vnode_data->is_directory = is_directory;
    vnode_data->first_cluster = first_cluster;
    vnode_data->size = size;
    vnode_data->inode_id = inode_id;

    vfs_ops_t* ops = is_directory ? &g_fat16_dir_ops : &g_fat16_file_ops;
    Nstatus status = vfs_init_vnode(node, inode_id, is_directory ? 2 : 1, ops, nNULL, vnode_data);
    if (NSTATUS_IS_ERR(status)) {
        fat16_free_vnode_data(vnode_data);
        return status;
    }

    return Nok;
}

static Nstatus fat16_read_root_directory_sector(
    fat16_device_t* dev,
    u32 sector_index,
    void* buffer
) {
    u32 sector = dev->first_root_dir_sector + sector_index;
    return fat16_read_sector(dev, sector, buffer);
}

static Nstatus fat16_read_directory_sector(
    fat16_device_t* dev,
    u16 cluster,
    u32 sector_index,
    void* buffer
) {
    u32 cluster_sector = dev->first_data_sector + ((cluster - 2) * dev->boot.SectorsPerCluster);
    return fat16_read_sector(dev, cluster_sector + sector_index, buffer);
}

static Nstatus fat16_find_entry_in_directory(
    fat16_device_t* dev,
    u16 start_cluster,
    bool root_directory,
    const char* name,
    fat16_dir_entry_t* out_entry,
    u64* out_inode
) {
    if (!dev || !name || !out_entry || !out_inode) {
        return NinvalidArg;
    }

    u8 sector_buffer[512];
    u32 entries_per_sector = dev->boot.BytesPerSector / FAT16_ENTRY_SIZE;

    if (root_directory) {
        for (u32 i = 0; i < dev->root_dir_sectors; i++) {
            Nstatus status = fat16_read_root_directory_sector(dev, i, sector_buffer);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            fat16_dir_entry_t* entry = (fat16_dir_entry_t*)sector_buffer;
            for (u32 j = 0; j < entries_per_sector; j++) {
                if (entry[j].Name[0] == 0x00) {
                    return NfileNotFound;
                }
                if (entry[j].Name[0] == 0xE5) {
                    continue;
                }
                if ((entry[j].Attr & FAT16_ATTR_LONG_NAME) == FAT16_ATTR_LONG_NAME) {
                    continue;
                }
                if (entry[j].Attr & FAT16_ATTR_VOLUME_ID) {
                    continue;
                }
                if (fat16_name_matches(entry[j].Name, name)) {
                    *out_entry = entry[j];
                    *out_inode = ((u64)dev->diskno << 32) | ((u64)i << 16) | j;
                    return Nok;
                }
            }
        }
        return NfileNotFound;
    }

    u16 cluster = start_cluster;
    while (!fat16_is_eoc(cluster)) {
        for (u32 sector_idx = 0; sector_idx < dev->boot.SectorsPerCluster; sector_idx++) {
            Nstatus status = fat16_read_directory_sector(dev, cluster, sector_idx, sector_buffer);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            fat16_dir_entry_t* entry = (fat16_dir_entry_t*)sector_buffer;
            for (u32 j = 0; j < entries_per_sector; j++) {
                if (entry[j].Name[0] == 0x00) {
                    return NfileNotFound;
                }
                if (entry[j].Name[0] == 0xE5) {
                    continue;
                }
                if ((entry[j].Attr & FAT16_ATTR_LONG_NAME) == FAT16_ATTR_LONG_NAME) {
                    continue;
                }
                if (entry[j].Attr & FAT16_ATTR_VOLUME_ID) {
                    continue;
                }
                if (fat16_name_matches(entry[j].Name, name)) {
                    *out_entry = entry[j];
                    *out_inode = ((u64)dev->diskno << 32) | ((u64)cluster << 16) | j;
                    return Nok;
                }
            }
        }

        u16 next;
        Nstatus status = fat16_get_fat_entry(dev, cluster, &next);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (next == 0x0000 || next == 0xFFF7) {
            return NfileCorrupted;
        }
        cluster = next;
    }

    return NfileNotFound;
}

static Nstatus fat16_lookup(vnode_t* dir, const char* name, vnode_t** out_vnode) {
    if (!dir || !name || !out_vnode) {
        return NinvalidArg;
    }

    fat16_vnode_data_t* parent = (fat16_vnode_data_t*)vfs_get_vnode_private(dir);
    if (!parent || !parent->dev) {
        return NinvalidArg;
    }

    fat16_dir_entry_t entry;
    u64 inode;
    bool root_directory = parent->is_directory && parent->first_cluster == 0;
    u16 search_cluster = parent->first_cluster;

    Nstatus status = fat16_find_entry_in_directory(parent->dev, search_cluster, root_directory, name, &entry, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    vnode_t* vnode = vfs_alloc_vnode();
    if (!vnode) {
        return NoutOfMemory;
    }

    u16 first_cluster = entry.FirstClusterLow;
    bool is_directory = (entry.Attr & FAT16_ATTR_DIRECTORY) != 0;
    u32 file_size = entry.FileSize;

    status = fat16_fill_vnode(vnode, parent->dev, is_directory, first_cluster, file_size, inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    *out_vnode = vnode;
    return Nok;
}

static Nstatus fat16_dir_open(vnode_t* vnode, u32 flags, handle_t* out_handle) {
    (void)vnode;
    (void)flags;
    (void)out_handle;
    return Nunsupported;
}

static Nstatus fat16_file_open(vnode_t* vnode, u32 flags, handle_t* handle) {
    if (!vnode || !handle) {
        return NinvalidArg;
    }

    fat16_vnode_data_t* vnode_data = (fat16_vnode_data_t*)vfs_get_vnode_private(vnode);
    if (!vnode_data || vnode_data->is_directory) {
        return NinvalidArg;
    }

    fat16_vnode_data_t* handle_data = fat16_alloc_handle_data();
    if (!handle_data) {
        return NoutOfMemory;
    }

    *handle_data = *vnode_data;
    handle->fs_private = handle_data;
    return Nok;
}

static Nstatus fat16_file_read(handle_t* handle, void* buffer, usize size, usize* bytes_read) {
    if (!handle || !buffer || !bytes_read) {
        return NinvalidArg;
    }

    fat16_vnode_data_t* file_data = (fat16_vnode_data_t*)handle->fs_private;
    if (!file_data || file_data->is_directory || !file_data->dev) {
        return NinvalidArg;
    }

    fat16_device_t* dev = file_data->dev;
    if (handle->offset >= file_data->size) {
        *bytes_read = 0;
        return Nok;
    }

    usize remaining = file_data->size - handle->offset;
    usize to_read = (size < remaining) ? size : remaining;
    usize total_read = 0;

    while (total_read < to_read) {
        u32 cluster_size = dev->bytes_per_cluster;
        u32 cluster_index = handle->offset / cluster_size;
        u32 cluster_offset = handle->offset % cluster_size;
        u16 cluster;
        Nstatus status = fat16_get_nth_cluster(dev, file_data->first_cluster, cluster_index, &cluster);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        usize cluster_byte_offset = ((usize)dev->first_data_sector * dev->boot.BytesPerSector) +
            ((cluster - 2) * cluster_size) + cluster_offset;
        usize chunk = cluster_size - cluster_offset;
        if (chunk > (to_read - total_read)) {
            chunk = to_read - total_read;
        }

        status = fat16_read_raw(dev, cluster_byte_offset, (u8*)buffer + total_read, chunk);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        total_read += chunk;
        handle->offset += chunk;
    }

    *bytes_read = total_read;
    return Nok;
}

static Nstatus fat16_file_write(handle_t* handle, const void* buffer, usize size, usize* bytes_written) {
    (void)handle;
    (void)buffer;
    (void)size;
    (void)bytes_written;
    return NreadOnly;
}

static Nstatus fat16_file_close(handle_t* handle) {
    if (!handle) {
        return NinvalidArg;
    }

    if (handle->fs_private) {
        fat16_free_handle_data((fat16_vnode_data_t*)handle->fs_private);
    }
    return Nok;
}

static Nstatus fat16_load_boot_sector(fat16_device_t* dev) {
    if (!dev || !dev->params.info) {
        return NinvalidArg;
    }

    Nstatus status = fat16_read_raw(dev, 0, &dev->boot, sizeof(fat16_boot_sector_t));
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (dev->boot.Signature != 0xAA55) {
        return NbadFilesystem;
    }

    if (dev->boot.BytesPerSector == 0 || dev->boot.SectorsPerCluster == 0) {
        return NbadFilesystem;
    }

    dev->total_sectors = dev->boot.TotalSectors16 ? dev->boot.TotalSectors16 : dev->boot.TotalSectors32;
    if (dev->total_sectors == 0) {
        return NbadFilesystem;
    }

    dev->root_dir_sectors = ((dev->boot.RootEntryCount * FAT16_ENTRY_SIZE) + dev->boot.BytesPerSector - 1) / dev->boot.BytesPerSector;
    dev->first_fat_sector = dev->boot.ReservedSectorCount;
    dev->first_root_dir_sector = dev->first_fat_sector + (dev->boot.FATCount * dev->boot.FATSize16);
    dev->first_data_sector = dev->first_root_dir_sector + dev->root_dir_sectors;
    dev->bytes_per_cluster = dev->boot.BytesPerSector * dev->boot.SectorsPerCluster;

    if (dev->total_sectors <= dev->first_data_sector) {
        return NbadFilesystem;
    }

    u32 data_sectors = dev->total_sectors - dev->first_data_sector;
    dev->total_clusters = data_sectors / dev->boot.SectorsPerCluster;
    if (dev->total_clusters < 1) {
        return NbadFilesystem;
    }

    return Nok;
}

static Nstatus fat16_mount_internal(fat16_device_t* dev) {
    if (!dev) {
        return NinvalidArg;
    }

    if (!dev->params.info) {
        return NinvalidArg;
    }

    if (!dev->read) {
        dev->read = ramdisk_read;
    }

    Nstatus status = fat16_load_boot_sector(dev);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    vnode_t* root = vfs_alloc_vnode();
    if (!root) {
        return NoutOfMemory;
    }

    status = fat16_fill_vnode(root, dev, true, 0, 0, ((u64)dev->diskno << 32) | 1);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    dev->root_vnode = root;
    memset(&dev->fs, 0, sizeof(filesystem_t));
    dev->fs.root = root;
    dev->fs.ops = &g_fat16_dir_ops;
    dev->fs.private = dev;
    dev->fs.sb = nNULL;
    dev->fs.name = "fat16";

    status = vfs_mount(dev->mount_path, &dev->fs);
    if (NSTATUS_IS_ERR(status)) {
        fat16_free_vnode_data((fat16_vnode_data_t*)vfs_get_vnode_private(root));
        return status;
    }

    return Nok;
}

Nstatus fat16_mount(u32 diskno, void* userdata) {
    if (!userdata) {
        return NinvalidArg;
    }

    fat16_mount_params_t* params = (fat16_mount_params_t*)userdata;
    if (!params->info) {
        return NinvalidArg;
    }

    fat16_device_t* existing = fat16_find_device(diskno);
    if (existing) {
        return NalreadyExists;
    }

    fat16_device_t* dev = fat16_alloc_device(diskno);
    if (!dev) {
        return NtooManyFileSystem;
    }

    dev->params = *params;
    dev->read = params->read ? params->read : ramdisk_read;
    dev->write = params->write;
    if (params->mount_path) {
        usize len = strlen((const utf8*)params->mount_path);
        if (len >= sizeof(dev->mount_path)) {
            fat16_free_device(dev);
            return NpathTooLong;
        }
        strncpy((utf8*)dev->mount_path, (const utf8*)params->mount_path, sizeof(dev->mount_path) - 1);
        dev->mount_path[sizeof(dev->mount_path) - 1] = '\0';
    } else {
        strncpy((utf8*)dev->mount_path, (const utf8*)"/", sizeof(dev->mount_path) - 1);
        dev->mount_path[sizeof(dev->mount_path) - 1] = '\0';
    }

    return fat16_mount_internal(dev);
}

Nstatus fat16_unmount(u32 diskno) {
    fat16_device_t* dev = fat16_find_device(diskno);
    if (!dev) {
        return NnotFound;
    }

    if (dev->mount_path[0] != '\0') {
        vfs_unmount(dev->mount_path);
    }

    if (dev->root_vnode) {
        fat16_free_vnode_data((fat16_vnode_data_t*)vfs_get_vnode_private(dev->root_vnode));
        vfs_free_vnode(dev->root_vnode);
        dev->root_vnode = nNULL;
    }

    fat16_free_device(dev);
    return Nok;
}

Nstatus fat16_register(void) {
    memset(&g_fat16_file_ops, 0, sizeof(vfs_ops_t));
    memset(&g_fat16_dir_ops, 0, sizeof(vfs_ops_t));

    g_fat16_dir_ops.lookup = fat16_lookup;
    g_fat16_dir_ops.open = fat16_dir_open;
    g_fat16_dir_ops.read = nNULL;
    g_fat16_dir_ops.write = nNULL;
    g_fat16_dir_ops.close = nNULL;

    g_fat16_file_ops.lookup = nNULL;
    g_fat16_file_ops.open = fat16_file_open;
    g_fat16_file_ops.read = fat16_file_read;
    g_fat16_file_ops.write = fat16_file_write;
    g_fat16_file_ops.close = fat16_file_close;

    filesystem_ops_t ops;
    ops.mount = fat16_mount;
    ops.unmount = fat16_unmount;
    ops.format = nNULL;

    return filesystem_register("fat16", ops);
}
