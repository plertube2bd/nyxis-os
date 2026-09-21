/*
 * fat16.c - FAT16 파일시스템 (읽기 전용)
 *
 * 디스크 이미지는 "신뢰할 수 없는 입력" 이다. 이 파일의 모든 파싱 코드는 이미지가
 * 악의적으로/우연히 손상되었어도 메모리를 망가뜨리거나 무한 루프에 빠지지 않도록
 * 작성해야 한다.
 *
 * [수정 이력 요약]  (심각도 순)
 *  1. 버퍼 오버플로: fat16_mount_internal 에서 memset(&dev->fs, 0, sizeof(filesystem_t)) 를 사용.
 *     dev->fs 의 실제 타입은 vfs_filesystem_t(40바이트)인데 filesystem_t(64바이트) 만큼 지워서
 *     바로 뒤의 read/write 함수 포인터(마운트 직후 read 가 NULL 이 되어 모든 파일 읽기가 실패)
 *     와 다음 배열 요소의 일부까지 덮어썼다.
 *  2. 스택 버퍼 오버플로: sector_buffer[512] 인데 BytesPerSector 를 검증하지 않아, 4096 같은 값을 가진
 *     이미지를 마운트하면 fat16_read_sector 가 스택 버퍼를 넘겨 쓴다. -> 512 만 허용.
 *  3. 파일 오프셋 이중 증가: fat16_file_read 가 handle->offset 을 올리고 vfs_read 가 또 올렸다.
 *     여러 번 나눠 읽으면 데이터를 건너뛰었다. -> 드라이버는 지역 변수만 사용.
 *  4. 클러스터 번호 미검증: 0/1/범위 밖 클러스터가 (cluster - 2) 언더플로로 임의 오프셋 읽기로 이어짐.
 *     FAT 체인에 순환이 있으면 디렉터리 탐색이 무한 루프. -> 모든 클러스터를 범위 검사하고
 *     체인 길이를 총 클러스터 수로 제한.
 *  5. 이름 매칭에서 8.3 이름이 8글자를 넘어도 잘라서 비교했다.
 *     ("HELLOWORLD.RUN" 이 "HELLOWOR.RUN" 과 같은 파일로 열림) -> 길이 초과는 불일치.
 *  6. BPB 값(FATCount, FATSize16, RootEntryCount, 총 클러스터 수 4085~65524 등) 검증 부족.
 *     산술은 u64 로 수행해 오버플로 방지.
 *  7. 자원 누수: lookup/mount 실패 경로에서 vnode/장치 슬롯 반환 누락.
 *  8. C89 호환 (for 루프 선언/혼합 선언/라인 주석), 'private' -> 'priv'.
 */

#include "drivers/filesystem/fat16/fat16.h"

#include "drivers/filesystem/vfs.h"
#include "drivers/filesystem/core.h"
#include "drivers/ramdisk/ramdisk.h"
#include "string.h"
#include "memory.h"
#include "nyxis.h"

#define FAT16_ENTRY_SIZE       32
#define FAT16_NAME_LENGTH      11
#define FAT16_MAX_DEVICES      8
#define FAT16_MAX_VNODE_DATA   128
#define FAT16_MAX_HANDLES      64

#define FAT16_SECTOR_SIZE      512U

#define FAT16_ATTR_READ_ONLY   0x01
#define FAT16_ATTR_HIDDEN      0x02
#define FAT16_ATTR_SYSTEM      0x04
#define FAT16_ATTR_VOLUME_ID   0x08
#define FAT16_ATTR_DIRECTORY   0x10
#define FAT16_ATTR_ARCHIVE     0x20
#define FAT16_ATTR_LONG_NAME   0x0F

#define FAT16_CLUSTER_BAD      0xFFF7U
#define FAT16_CLUSTER_EOC      0xFFF8U

/* FAT16 으로 인정되는 데이터 클러스터 수 범위 (Microsoft FAT 규격) */
#define FAT16_MIN_CLUSTERS     4085UL
#define FAT16_MAX_CLUSTERS     65524UL

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

NX_STATIC_ASSERT(fat16_boot_sector_size, sizeof(fat16_boot_sector_t) == 512);

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

NX_STATIC_ASSERT(fat16_dir_entry_size, sizeof(fat16_dir_entry_t) == 32);

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
    vnode_t *root_vnode;
    vfs_filesystem_t fs;
    fat16_disk_read_fn read;
    fat16_disk_write_fn write;
} fat16_device_t;

typedef struct {
    fat16_device_t *dev;
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

static u8 fat16_toupper(u8 c)
{
    if (c >= 'a' && c <= 'z') {
        return (u8)(c - 32);
    }
    return c;
}

static bool fat16_is_eoc(u16 cluster)
{
    return (cluster >= FAT16_CLUSTER_EOC) ? true : false;
}

/* 데이터 영역의 유효한 클러스터 번호인지 (2 ~ total_clusters+1) */
static bool fat16_cluster_valid(const fat16_device_t *dev, u32 cluster)
{
    return (cluster >= 2U && cluster < dev->total_clusters + 2U) ? true : false;
}

static fat16_device_t *fat16_find_device(u32 diskno)
{
    u32 i;

    for (i = 0; i < FAT16_MAX_DEVICES; i++) {
        if (g_devices[i].used && g_devices[i].diskno == diskno) {
            return &g_devices[i];
        }
    }
    return nNULL;
}

static fat16_device_t *fat16_alloc_device(u32 diskno)
{
    u32 i;

    for (i = 0; i < FAT16_MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            memset(&g_devices[i], 0, sizeof(fat16_device_t));
            g_devices[i].used = true;
            g_devices[i].diskno = diskno;
            return &g_devices[i];
        }
    }
    return nNULL;
}

static void fat16_free_device(fat16_device_t *dev)
{
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

static fat16_vnode_data_t *fat16_alloc_vnode_data(void)
{
    u32 i;

    for (i = 0; i < FAT16_MAX_VNODE_DATA; i++) {
        if (!g_vnode_data[i].in_use) {
            g_vnode_data[i].in_use = true;
            memset(&g_vnode_data[i].entry, 0, sizeof(fat16_vnode_data_t));
            return &g_vnode_data[i].entry;
        }
    }
    return nNULL;
}

static void fat16_free_vnode_data(fat16_vnode_data_t *data)
{
    u32 i;

    if (!data) {
        return;
    }

    for (i = 0; i < FAT16_MAX_VNODE_DATA; i++) {
        if (&g_vnode_data[i].entry == data) {
            g_vnode_data[i].in_use = false;
            memset(&g_vnode_data[i].entry, 0, sizeof(fat16_vnode_data_t));
            return;
        }
    }
}

static fat16_vnode_data_t *fat16_alloc_handle_data(void)
{
    u32 i;

    for (i = 0; i < FAT16_MAX_HANDLES; i++) {
        if (!g_handle_slots[i].in_use) {
            g_handle_slots[i].in_use = true;
            memset(&g_handle_slots[i].handle, 0, sizeof(fat16_vnode_data_t));
            return &g_handle_slots[i].handle;
        }
    }
    return nNULL;
}

static void fat16_free_handle_data(fat16_vnode_data_t *data)
{
    u32 i;

    if (!data) {
        return;
    }

    for (i = 0; i < FAT16_MAX_HANDLES; i++) {
        if (&g_handle_slots[i].handle == data) {
            g_handle_slots[i].in_use = false;
            memset(&g_handle_slots[i].handle, 0, sizeof(fat16_vnode_data_t));
            return;
        }
    }
}

static Nstatus fat16_read_raw(
    fat16_device_t *dev,
    u64 offset,
    void *buffer,
    usize size
) {
    if (!dev || !buffer || size == 0) {
        return NinvalidArg;
    }

    if (!dev->read || !dev->params.info) {
        return NinvalidArg;
    }

    return dev->read(dev->diskno, (usize)offset, buffer, size, dev->params.info);
}

static Nstatus fat16_read_sector(
    fat16_device_t *dev,
    u32 sector,
    void *buffer
) {
    u64 offset = (u64)sector * FAT16_SECTOR_SIZE;
    return fat16_read_raw(dev, offset, buffer, FAT16_SECTOR_SIZE);
}

static Nstatus fat16_get_fat_entry(
    fat16_device_t *dev,
    u16 cluster,
    u16 *out_next
) {
    u64 fat_offset;
    u16 entry;
    Nstatus status;

    if (!dev || !out_next) {
        return NinvalidArg;
    }

    /* FAT 테이블 밖을 읽지 않도록 클러스터 범위 검사 */
    if (!fat16_cluster_valid(dev, cluster)) {
        return NfileCorrupted;
    }

    fat_offset = (u64)dev->first_fat_sector * FAT16_SECTOR_SIZE + (u64)cluster * 2U;
    status = fat16_read_raw(dev, fat_offset, &entry, sizeof(entry));
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    *out_next = entry;
    return Nok;
}

/*
 * 체인에서 index 번째 클러스터를 구한다.
 * 유효한 체인은 총 클러스터 수보다 길 수 없으므로 이를 넘으면 손상(순환)으로 판단한다.
 */
static Nstatus fat16_get_nth_cluster(
    fat16_device_t *dev,
    u16 start_cluster,
    u32 index,
    u16 *out_cluster
) {
    u16 cluster = start_cluster;
    u32 i;

    if (!dev || !out_cluster) {
        return NinvalidArg;
    }

    if (!fat16_cluster_valid(dev, cluster) || index >= dev->total_clusters) {
        return NfileCorrupted;
    }

    for (i = 0; i < index; i++) {
        u16 next;
        Nstatus status = fat16_get_fat_entry(dev, cluster, &next);

        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (fat16_is_eoc(next)) {
            return NnotFound;
        }
        if (next == FAT16_CLUSTER_BAD || !fat16_cluster_valid(dev, next)) {
            return NfileCorrupted;
        }
        cluster = next;
    }

    *out_cluster = cluster;
    return Nok;
}

/*
 * 요청 이름("NAME.EXT")이 디렉터리 엔트리의 8.3 이름과 정확히 같은지 비교한다.
 * 8글자/3글자를 넘는 이름, 점이 둘 이상인 이름, 이름이 비어 있는 이름은 불일치.
 */
static bool fat16_name_matches(const utf8 entry_name[11], const char *request_name)
{
    utf8 normalized[FAT16_NAME_LENGTH];
    const char *cursor;
    usize name_len = 0;
    usize ext_len = 0;
    u32 i;

    if (!entry_name || !request_name) {
        return false;
    }

    for (i = 0; i < FAT16_NAME_LENGTH; i++) {
        normalized[i] = ' ';
    }

    cursor = request_name;

    while (*cursor && *cursor != '.') {
        if (name_len >= 8) {
            return false;               /* 8글자 초과: 잘라서 비교하면 안 된다 */
        }
        normalized[name_len++] = fat16_toupper((u8)*cursor);
        cursor++;
    }

    if (name_len == 0) {
        return false;
    }

    if (*cursor == '.') {
        cursor++;
        while (*cursor) {
            if (*cursor == '.' || ext_len >= 3) {
                return false;
            }
            normalized[8 + ext_len] = fat16_toupper((u8)*cursor);
            ext_len++;
            cursor++;
        }
    }

    for (i = 0; i < FAT16_NAME_LENGTH; i++) {
        utf8 candidate = normalized[i];
        utf8 actual = entry_name[i];

        if (actual >= 'a' && actual <= 'z') {
            actual = (utf8)(actual - 32);
        }
        if (candidate != actual) {
            return false;
        }
    }
    return true;
}

static Nstatus fat16_fill_vnode(
    vnode_t *node,
    fat16_device_t *dev,
    bool is_directory,
    u16 first_cluster,
    u32 size,
    u64 inode_id
) {
    fat16_vnode_data_t *vnode_data;
    vfs_ops_t *ops;
    Nstatus status;

    if (!node || !dev) {
        return NinvalidArg;
    }

    vnode_data = fat16_alloc_vnode_data();
    if (!vnode_data) {
        return NoutOfMemory;
    }

    vnode_data->dev = dev;
    vnode_data->is_directory = is_directory;
    vnode_data->first_cluster = first_cluster;
    vnode_data->size = size;
    vnode_data->inode_id = inode_id;

    ops = is_directory ? &g_fat16_dir_ops : &g_fat16_file_ops;
    status = vfs_init_vnode(node, inode_id, is_directory ? 2 : 1, ops, nNULL, vnode_data);
    if (NSTATUS_IS_ERR(status)) {
        fat16_free_vnode_data(vnode_data);
        return status;
    }

    return Nok;
}

/* 디렉터리 엔트리 sector_buffer 를 검사하여 name 과 일치하는 항목을 찾는다 */
static Nstatus fat16_scan_sector(
    const u8 *sector_buffer,
    const char *name,
    fat16_dir_entry_t *out_entry,
    u32 *out_index,
    bool *out_end
) {
    const fat16_dir_entry_t *entry = (const fat16_dir_entry_t *)(const void *)sector_buffer;
    u32 entries_per_sector = FAT16_SECTOR_SIZE / FAT16_ENTRY_SIZE;
    u32 j;

    *out_end = false;

    for (j = 0; j < entries_per_sector; j++) {
        if (entry[j].Name[0] == 0x00) {
            *out_end = true;            /* 이 이후는 모두 미사용 */
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
            *out_index = j;
            return Nok;
        }
    }

    return NfileNotFound;
}

static Nstatus fat16_find_entry_in_directory(
    fat16_device_t *dev,
    u16 start_cluster,
    bool root_directory,
    const char *name,
    fat16_dir_entry_t *out_entry,
    u64 *out_inode
) {
    u8 sector_buffer[FAT16_SECTOR_SIZE];
    u32 index = 0;
    bool end = false;

    if (!dev || !name || !out_entry || !out_inode) {
        return NinvalidArg;
    }

    if (root_directory) {
        u32 i;

        for (i = 0; i < dev->root_dir_sectors; i++) {
            Nstatus status = fat16_read_sector(dev, dev->first_root_dir_sector + i, sector_buffer);

            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            status = fat16_scan_sector(sector_buffer, name, out_entry, &index, &end);
            if (status == Nok) {
                *out_inode = ((u64)dev->diskno << 32) | ((u64)i << 16) | index;
                return Nok;
            }
            if (end) {
                return NfileNotFound;
            }
        }
        return NfileNotFound;
    }

    {
        u16 cluster = start_cluster;
        u32 chain_len;

        /* 체인 길이를 총 클러스터 수로 제한 (FAT 순환으로 인한 무한 루프 방지) */
        for (chain_len = 0; chain_len < dev->total_clusters; chain_len++) {
            u32 sector_idx;
            u16 next;
            Nstatus status;

            if (!fat16_cluster_valid(dev, cluster)) {
                return NfileCorrupted;
            }

            for (sector_idx = 0; sector_idx < dev->boot.SectorsPerCluster; sector_idx++) {
                u32 sector = dev->first_data_sector +
                             ((u32)(cluster - 2U) * dev->boot.SectorsPerCluster) + sector_idx;

                status = fat16_read_sector(dev, sector, sector_buffer);
                if (NSTATUS_IS_ERR(status)) {
                    return status;
                }

                status = fat16_scan_sector(sector_buffer, name, out_entry, &index, &end);
                if (status == Nok) {
                    *out_inode = ((u64)dev->diskno << 32) | ((u64)cluster << 16) | index;
                    return Nok;
                }
                if (end) {
                    return NfileNotFound;
                }
            }

            status = fat16_get_fat_entry(dev, cluster, &next);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
            if (fat16_is_eoc(next)) {
                return NfileNotFound;
            }
            if (next == FAT16_CLUSTER_BAD) {
                return NfileCorrupted;
            }
            cluster = next;
        }
    }

    /* 체인이 EOC 없이 total_clusters 를 넘음 = 순환 */
    return NfileCorrupted;
}

static Nstatus fat16_lookup(vnode_t *dir, const char *name, vnode_t **out_vnode)
{
    fat16_vnode_data_t *parent;
    fat16_dir_entry_t entry;
    u64 inode;
    bool root_directory;
    vnode_t *vnode;
    Nstatus status;

    if (!dir || !name || !out_vnode) {
        return NinvalidArg;
    }

    parent = (fat16_vnode_data_t *)vfs_get_vnode_private(dir);
    if (!parent || !parent->dev || !parent->is_directory) {
        return NinvalidArg;
    }

    root_directory = (parent->first_cluster == 0) ? true : false;

    status = fat16_find_entry_in_directory(parent->dev, parent->first_cluster, root_directory,
                                           name, &entry, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    vnode = vfs_alloc_vnode();
    if (!vnode) {
        return NoutOfMemory;
    }

    status = fat16_fill_vnode(vnode, parent->dev,
                              (entry.Attr & FAT16_ATTR_DIRECTORY) ? true : false,
                              entry.FirstClusterLow, entry.FileSize, inode);
    if (NSTATUS_IS_ERR(status)) {
        vfs_free_vnode(vnode);          /* 실패 시 vnode 누수 방지 */
        return status;
    }

    *out_vnode = vnode;
    return Nok;
}

/* 파일/디렉터리 공통 stat: vnode 에 저장된 정보를 그대로 돌려준다 */
static Nstatus fat16_stat(vnode_t *vnode, vfs_stat_t *out)
{
    fat16_vnode_data_t *data;

    if (!vnode || !out) {
        return NinvalidArg;
    }

    data = (fat16_vnode_data_t *)vfs_get_vnode_private(vnode);
    if (!data) {
        return NinvalidArg;
    }

    out->size = data->is_directory ? 0 : data->size;
    out->type = data->is_directory ? 2U : 1U;
    return Nok;
}

static Nstatus fat16_dir_open(vnode_t *vnode, u32 flags, handle_t *out_handle)
{
    (void)vnode;
    (void)flags;
    (void)out_handle;
    return Nunsupported;
}

static Nstatus fat16_file_open(vnode_t *vnode, u32 flags, handle_t *handle)
{
    fat16_vnode_data_t *vnode_data;
    fat16_vnode_data_t *handle_data;

    (void)flags;

    if (!vnode || !handle) {
        return NinvalidArg;
    }

    vnode_data = (fat16_vnode_data_t *)vfs_get_vnode_private(vnode);
    if (!vnode_data || vnode_data->is_directory) {
        return NinvalidArg;
    }

    handle_data = fat16_alloc_handle_data();
    if (!handle_data) {
        return NoutOfMemory;
    }

    *handle_data = *vnode_data;
    handle->fs_private = handle_data;
    return Nok;
}

/*
 * 파일 읽기. handle->offset 은 읽기만 하고 절대 수정하지 않는다.
 * (offset 갱신은 vfs_read 의 책임)
 */
static Nstatus fat16_file_read(handle_t *handle, void *buffer, usize size, usize *bytes_read)
{
    fat16_vnode_data_t *file_data;
    fat16_device_t *dev;
    u64 offset;
    u64 remaining;
    usize to_read;
    usize total_read = 0;

    if (!handle || !buffer || !bytes_read) {
        return NinvalidArg;
    }

    *bytes_read = 0;

    file_data = (fat16_vnode_data_t *)handle->fs_private;
    if (!file_data || file_data->is_directory || !file_data->dev) {
        return NinvalidArg;
    }

    dev = file_data->dev;
    offset = handle->offset;

    if (offset >= file_data->size) {
        return Nok;                     /* EOF */
    }

    remaining = (u64)file_data->size - offset;
    to_read = (size < remaining) ? size : (usize)remaining;

    while (total_read < to_read) {
        u32 cluster_size = dev->bytes_per_cluster;
        u64 cluster_index = offset / cluster_size;
        u32 cluster_offset = (u32)(offset % cluster_size);
        u16 cluster;
        u64 cluster_byte_offset;
        usize chunk;
        Nstatus status;

        if (cluster_index >= dev->total_clusters) {
            return NfileCorrupted;
        }

        status = fat16_get_nth_cluster(dev, file_data->first_cluster, (u32)cluster_index, &cluster);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        cluster_byte_offset = ((u64)dev->first_data_sector * FAT16_SECTOR_SIZE) +
                              ((u64)(cluster - 2U) * cluster_size) + cluster_offset;
        chunk = cluster_size - cluster_offset;
        if (chunk > (to_read - total_read)) {
            chunk = to_read - total_read;
        }

        status = fat16_read_raw(dev, cluster_byte_offset, (u8 *)buffer + total_read, chunk);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        total_read += chunk;
        offset += chunk;
    }

    *bytes_read = total_read;
    return Nok;
}

static Nstatus fat16_file_write(handle_t *handle, const void *buffer, usize size, usize *bytes_written)
{
    (void)handle;
    (void)buffer;
    (void)size;
    (void)bytes_written;
    return NreadOnly;
}

static Nstatus fat16_file_close(handle_t *handle)
{
    if (!handle) {
        return NinvalidArg;
    }

    if (handle->fs_private) {
        fat16_free_handle_data((fat16_vnode_data_t *)handle->fs_private);
        handle->fs_private = nNULL;
    }
    return Nok;
}

/*
 * 부트 섹터(BPB) 를 읽고 검증한다. 검증을 통과해야만 이후 코드의 산술/버퍼 가정이 성립한다.
 */
static Nstatus fat16_load_boot_sector(fat16_device_t *dev)
{
    Nstatus status;
    u64 root_dir_sectors;
    u64 first_fat;
    u64 first_root;
    u64 first_data;
    u64 total;
    u64 data_sectors;
    u64 clusters;
    u32 spc;

    if (!dev || !dev->params.info) {
        return NinvalidArg;
    }

    status = fat16_read_raw(dev, 0, &dev->boot, sizeof(fat16_boot_sector_t));
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (dev->boot.Signature != 0xAA55) {
        return NbadFilesystem;
    }

    /* 이 드라이버의 모든 스택 버퍼는 512바이트 섹터를 전제로 한다 */
    if (dev->boot.BytesPerSector != FAT16_SECTOR_SIZE) {
        return Nunsupported;
    }

    spc = dev->boot.SectorsPerCluster;
    if (spc == 0 || spc > 128 || (spc & (spc - 1)) != 0) {
        return NbadFilesystem;
    }

    if (dev->boot.ReservedSectorCount == 0 ||
        dev->boot.FATCount == 0 || dev->boot.FATCount > 2 ||
        dev->boot.FATSize16 == 0 ||
        dev->boot.RootEntryCount == 0) {
        return NbadFilesystem;
    }

    total = dev->boot.TotalSectors16 ? dev->boot.TotalSectors16 : dev->boot.TotalSectors32;
    if (total == 0 || total > 0xFFFFFFFFUL) {
        return NbadFilesystem;
    }

    root_dir_sectors = (((u64)dev->boot.RootEntryCount * FAT16_ENTRY_SIZE) +
                        FAT16_SECTOR_SIZE - 1) / FAT16_SECTOR_SIZE;
    first_fat = dev->boot.ReservedSectorCount;
    first_root = first_fat + (u64)dev->boot.FATCount * dev->boot.FATSize16;
    first_data = first_root + root_dir_sectors;

    if (first_data >= total) {
        return NbadFilesystem;
    }

    data_sectors = total - first_data;
    clusters = data_sectors / spc;

    /* 클러스터 수로 FAT 종류를 판정한다 (범위 밖이면 FAT12/FAT32 이므로 이 드라이버 대상 아님) */
    if (clusters < FAT16_MIN_CLUSTERS || clusters > FAT16_MAX_CLUSTERS) {
        return NbadFilesystem;
    }

    /* FAT 테이블이 모든 클러스터 엔트리(+예약 2개)를 담을 수 있어야 한다 */
    if ((u64)dev->boot.FATSize16 * FAT16_SECTOR_SIZE < (clusters + 2U) * 2U) {
        return NbadFilesystem;
    }

    dev->total_sectors = (u32)total;
    dev->root_dir_sectors = (u32)root_dir_sectors;
    dev->first_fat_sector = (u32)first_fat;
    dev->first_root_dir_sector = (u32)first_root;
    dev->first_data_sector = (u32)first_data;
    dev->bytes_per_cluster = FAT16_SECTOR_SIZE * spc;
    dev->total_clusters = (u32)clusters;

    return Nok;
}

static Nstatus fat16_mount_internal(fat16_device_t *dev)
{
    vnode_t *root;
    Nstatus status;

    if (!dev || !dev->params.info) {
        return NinvalidArg;
    }

    if (!dev->read) {
        dev->read = ramdisk_read;
    }

    status = fat16_load_boot_sector(dev);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    root = vfs_alloc_vnode();
    if (!root) {
        return NoutOfMemory;
    }

    status = fat16_fill_vnode(root, dev, true, 0, 0, ((u64)dev->diskno << 32) | 1);
    if (NSTATUS_IS_ERR(status)) {
        vfs_free_vnode(root);
        return status;
    }

    dev->root_vnode = root;

    /* 타입에 맞는 크기로 초기화한다 (예전에는 filesystem_t 크기로 지워서 오버플로) */
    memset(&dev->fs, 0, sizeof(dev->fs));
    dev->fs.root = root;
    dev->fs.ops = &g_fat16_dir_ops;
    dev->fs.priv = dev;
    dev->fs.sb = nNULL;
    dev->fs.name = "fat16";

    status = vfs_mount(dev->mount_path, &dev->fs);
    if (NSTATUS_IS_ERR(status)) {
        fat16_free_vnode_data((fat16_vnode_data_t *)vfs_get_vnode_private(root));
        vfs_free_vnode(root);
        dev->root_vnode = nNULL;
        return status;
    }

    return Nok;
}

Nstatus fat16_mount(u32 diskno, void *userdata)
{
    fat16_mount_params_t *params;
    fat16_device_t *dev;
    Nstatus status;

    if (!userdata) {
        return NinvalidArg;
    }

    params = (fat16_mount_params_t *)userdata;
    if (!params->info) {
        return NinvalidArg;
    }

    if (fat16_find_device(diskno)) {
        return NalreadyExists;
    }

    dev = fat16_alloc_device(diskno);
    if (!dev) {
        return NtooManyFileSystem;
    }

    dev->params = *params;
    dev->read = params->read ? params->read : ramdisk_read;
    dev->write = params->write;

    if (params->mount_path) {
        if (strnlen(params->mount_path, sizeof(dev->mount_path)) >= sizeof(dev->mount_path)) {
            fat16_free_device(dev);
            return NpathTooLong;
        }
        strncpy(dev->mount_path, params->mount_path, sizeof(dev->mount_path) - 1);
    } else {
        strncpy(dev->mount_path, "/", sizeof(dev->mount_path) - 1);
    }
    dev->mount_path[sizeof(dev->mount_path) - 1] = '\0';

    status = fat16_mount_internal(dev);
    if (NSTATUS_IS_ERR(status)) {
        fat16_free_device(dev);         /* 실패해도 슬롯을 돌려줘야 재시도 가능 */
        return status;
    }

    return Nok;
}

Nstatus fat16_unmount(u32 diskno)
{
    fat16_device_t *dev = fat16_find_device(diskno);

    if (!dev) {
        return NnotFound;
    }

    if (dev->mount_path[0] != '\0') {
        (void)vfs_unmount(dev->mount_path);
    }

    if (dev->root_vnode) {
        fat16_free_vnode_data((fat16_vnode_data_t *)vfs_get_vnode_private(dev->root_vnode));
        vfs_free_vnode(dev->root_vnode);
        dev->root_vnode = nNULL;
    }

    fat16_free_device(dev);
    return Nok;
}

Nstatus fat16_register(void)
{
    filesystem_ops_t ops;

    memset(&g_fat16_file_ops, 0, sizeof(vfs_ops_t));
    memset(&g_fat16_dir_ops, 0, sizeof(vfs_ops_t));

    g_fat16_dir_ops.lookup = fat16_lookup;
    g_fat16_dir_ops.open = fat16_dir_open;
    g_fat16_dir_ops.read = nNULL;
    g_fat16_dir_ops.write = nNULL;
    g_fat16_dir_ops.close = nNULL;
    g_fat16_dir_ops.stat = fat16_stat;

    g_fat16_file_ops.lookup = nNULL;
    g_fat16_file_ops.open = fat16_file_open;
    g_fat16_file_ops.read = fat16_file_read;
    g_fat16_file_ops.write = fat16_file_write;
    g_fat16_file_ops.close = fat16_file_close;
    g_fat16_file_ops.stat = fat16_stat;

    ops.mount = fat16_mount;
    ops.unmount = fat16_unmount;
    ops.format = nNULL;

    return filesystem_register("fat16", ops);
}
