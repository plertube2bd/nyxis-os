/*
 * nyfs.c - NyFS 커널 드라이버
 *
 * [현재 구현 범위]
 *  - mount / unmount / format(mkfs)
 *  - lookup / open / read / write / close / stat (vfs_ops_t)
 *  - 블록 할당자: 비트맵(v1, NYFS_ALLOC_BITMAP) - O(1) 은 아니고 첫 빈 비트를 선형으로
 *    찾는다. 사용자와 합의된 O(1) 요구 사항은 "경로 컴포넌트 조회"에 한정되고 블록
 *    할당까지는 아니라서, 정확성이 검증하기 쉬운 비트맵으로 먼저 구현했다.
 *    나중에 크기별 free-list 같은 걸로 바꿔도 슈퍼블록의 alloc_algorithm 필드 덕분에
 *    온디스크 포맷을 건드리지 않고 교체할 수 있다.
 *  - inode 할당자: inode 용 비트맵 하나 더(별도 영역).
 *  - 데이터 스트림/디렉터리 해시 영역 둘 다 "extent 리스트 블록의 체인"이라는 같은
 *    메커니즘 위에서 동작한다(nyfs_extent_read_at/nyfs_extent_write_at). 새 블록이
 *    필요하면 끝에 하나씩 할당해서 붙이고(연속이면 길이만 늘리고, 아니면 새 extent
 *    추가), 현재 extent 블록이 가득 차면 next_block 으로 새 extent 블록을 잇는다.
 *    depth(간접 참조 단계)는 아직 0(직접 리스트)만 지원한다 - 매우 심하게 조각난
 *    거대 파일을 위한 다단계 간접 참조는 지금 스코프 밖이다(nyfs_disk.h 의 필드는
 *    이미 예약돼 있으니 나중에 추가해도 온디스크 레이아웃은 안 깨진다).
 *
 * [아직 안 된 것 - 다음 단계에서 다룰 것들]
 *  - 파일/디렉터리 생성, 삭제, 이름변경, readdir: vfs_ops_t 자체에 아직 create/mkdir/
 *    unlink/readdir 훅이 없다(fat16 도 마찬가지 - 기존에 있던 공백). VFS 를 확장할지는
 *    별도로 결정해야 한다. 지금은 format() 이 루트 디렉터리 하나만 특별 취급으로 만든다.
 *  - "." / ".." 엔트리: readdir 이 없으니 지금은 의미가 없어서 생략했다.
 *  - 디렉터리 해시테이블의 버킷 개수는 생성 시(NYFS_DIRHASH_DEFAULT_BUCKETS) 고정이고
 *    나중에 리해싱(재분배)하지 않는다 - 엔트리가 아주 많아지면 체인이 길어진다.
 *  - 저널: 슈퍼블록/inode 의 journal_* 필드만 예약돼 있고 실제로는 안 쓴다.
 *  - 삭제된 dirent/블록 재사용: free_offset 은 단순 bump 이고 지운 공간을 재사용하지
 *    않는다(애초에 delete 자체가 아직 없다).
 *
 * [권한 검사는 한 곳에서만]
 *  nyfs_check_access() 가 유일한 권한 판정 함수다. lookup() 은 디렉터리 EXECUTE(순회)
 *  권한을, open() 은 READ_DATA/WRITE_DATA 권한을 이 함수 하나로 검사하고, 그 밖의
 *  어떤 경로도 inode 내용에 접근할 방법이 없다(핸들은 open() 을 통해서만 생긴다).
 *  호출자 신원(uid/gid)은 kernel/process/process.h 의 current_process 에서 읽는다 -
 *  ring3 프로세스/로그인이 아직 없어 지금은 사실상 항상 root(0,0)이다.
 */

#include "drivers/filesystem/nyfs/nyfs.h"
#include "drivers/filesystem/nyfs/nyfs_disk.h"

#include "drivers/filesystem/vfs.h"
#include "drivers/filesystem/core.h"
#include "kernel/process/process.h"
#include "nyx_abi.h"
#include "string.h"
#include "memory.h"
#include "nyxis.h"

#define NYFS_MAX_VOLUMES              8U
#define NYFS_MAX_VNODE_DATA           256U
#define NYFS_MAX_HANDLES              128U
#define NYFS_DIRHASH_DEFAULT_BUCKETS  16U

/* extent 블록 하나에 들어갈 수 있는 extent 개수의 상한. 블록 크기가 커도(최대 64KiB)
 * 이 상한을 넘는 만큼은 그냥 다음 extent 블록으로 넘어간다 - 이 상한 덕분에 extent
 * 배열을 항상 고정 크기 스택 버퍼(128 * 16 = 2KiB)로 다룰 수 있고, 커널 스택
 * (PROCESS_KSTACK_SIZE = 16KiB)에 64KiB짜리 블록 버퍼를 올릴 필요가 없다. */
#define NYFS_MAX_EXTENTS_PER_BLOCK    128U

/* inode 하나에서 한 번에 평가하는 ACE 총 개수(인라인 + 오버플로) 상한. 이것도
 * 스택 버퍼 크기를 고정하기 위함이다(64 * 12 = 768 B). */
#define NYFS_MAX_TOTAL_ACES           64U

typedef struct {
    bool used;
    u32 diskno;
    nyfs_disk_read_fn read;
    nyfs_disk_write_fn write;
    NTBLI *info;
    nyfs_superblock_t sb;   /* 메모리에 캐시된 슈퍼블록. 바뀔 때마다 블록 0 에 다시 쓴다 */
    u32 block_size;          /* 1 << sb.block_size_shift (매번 시프트 계산 피하려고 캐시) */
    char mount_path[256];
    vnode_t *root_vnode;
    vfs_filesystem_t fs;
    spinlock_t lock;          /* 이 볼륨의 할당(비트맵)/슈퍼블록 갱신을 보호 */
} nyfs_volume_t;

typedef struct {
    nyfs_volume_t *vol;
    nyfs_ino_t ino;
} nyfs_vnode_data_t;

typedef struct {
    bool in_use;
    nyfs_vnode_data_t entry;
} nyfs_vnode_slot_t;

typedef struct {
    bool in_use;
    nyfs_vnode_data_t entry;
} nyfs_handle_slot_t;

static nyfs_volume_t g_volumes[NYFS_MAX_VOLUMES];
static nyfs_vnode_slot_t g_vnode_data[NYFS_MAX_VNODE_DATA];
static nyfs_handle_slot_t g_handle_slots[NYFS_MAX_HANDLES];
static vfs_ops_t g_nyfs_ops;

/* ------------------------------------------------------------------ */
/* 풀 관리 (fat16 드라이버와 같은 패턴: 동적 할당자가 없어 정적 배열 사용)     */
/* ------------------------------------------------------------------ */

static nyfs_volume_t *nyfs_find_volume(u32 diskno)
{
    u32 i;

    for (i = 0; i < NYFS_MAX_VOLUMES; i++) {
        if (g_volumes[i].used && g_volumes[i].diskno == diskno) {
            return &g_volumes[i];
        }
    }
    return nNULL;
}

static nyfs_volume_t *nyfs_alloc_volume(u32 diskno)
{
    u32 i;

    for (i = 0; i < NYFS_MAX_VOLUMES; i++) {
        if (!g_volumes[i].used) {
            memset(&g_volumes[i], 0, sizeof(nyfs_volume_t));
            g_volumes[i].used = true;
            g_volumes[i].diskno = diskno;
            return &g_volumes[i];
        }
    }
    return nNULL;
}

static void nyfs_free_volume(nyfs_volume_t *vol)
{
    if (!vol) {
        return;
    }
    vol->used = false;
    vol->diskno = 0;
    vol->root_vnode = nNULL;
    vol->read = nNULL;
    vol->write = nNULL;
    memset(vol->mount_path, 0, sizeof(vol->mount_path));
}

static nyfs_vnode_data_t *nyfs_alloc_vnode_data(void)
{
    u32 i;

    for (i = 0; i < NYFS_MAX_VNODE_DATA; i++) {
        if (!g_vnode_data[i].in_use) {
            g_vnode_data[i].in_use = true;
            memset(&g_vnode_data[i].entry, 0, sizeof(nyfs_vnode_data_t));
            return &g_vnode_data[i].entry;
        }
    }
    return nNULL;
}

static void nyfs_free_vnode_data(nyfs_vnode_data_t *data)
{
    u32 i;

    if (!data) {
        return;
    }
    for (i = 0; i < NYFS_MAX_VNODE_DATA; i++) {
        if (&g_vnode_data[i].entry == data) {
            g_vnode_data[i].in_use = false;
            memset(&g_vnode_data[i].entry, 0, sizeof(nyfs_vnode_data_t));
            return;
        }
    }
}

static nyfs_vnode_data_t *nyfs_alloc_handle_data(void)
{
    u32 i;

    for (i = 0; i < NYFS_MAX_HANDLES; i++) {
        if (!g_handle_slots[i].in_use) {
            g_handle_slots[i].in_use = true;
            memset(&g_handle_slots[i].entry, 0, sizeof(nyfs_vnode_data_t));
            return &g_handle_slots[i].entry;
        }
    }
    return nNULL;
}

static void nyfs_free_handle_data(nyfs_vnode_data_t *data)
{
    u32 i;

    if (!data) {
        return;
    }
    for (i = 0; i < NYFS_MAX_HANDLES; i++) {
        if (&g_handle_slots[i].entry == data) {
            g_handle_slots[i].in_use = false;
            memset(&g_handle_slots[i].entry, 0, sizeof(nyfs_vnode_data_t));
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 원시 I/O                                                            */
/* ------------------------------------------------------------------ */

static Nstatus nyfs_raw_read(nyfs_volume_t *vol, u64 offset, void *buffer, usize size)
{
    if (!vol || !vol->read || !buffer) {
        return NinvalidArg;
    }
    return vol->read(vol->diskno, (usize)offset, buffer, size, vol->info);
}

static Nstatus nyfs_raw_write(nyfs_volume_t *vol, u64 offset, const void *buffer, usize size)
{
    if (!vol || !buffer) {
        return NinvalidArg;
    }
    if (!vol->write) {
        return NreadOnly;
    }
    return vol->write(vol->diskno, (usize)offset, buffer, size, vol->info);
}

/* ------------------------------------------------------------------ */
/* CRC32 (표준 IEEE 802.3 다항식). 테이블 없이 비트 단위로 계산한다 -
 * 메타데이터 쓰기에서만 쓰이는 콜드 패스라 256워드 테이블을 위한 정적 데이터를
 * 아낀다. */
/* ------------------------------------------------------------------ */

static u32 nyfs_crc32(const void *data, usize len)
{
    const u32 poly = 0xEDB88320U;
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFU;
    usize i;
    u32 j;

    for (i = 0; i < len; i++) {
        crc ^= (u32)p[i];
        for (j = 0; j < 8U; j++) {
            u32 mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (poly & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static u32 nyfs_hash_name(const char *name, usize len)
{
    u32 h = 2166136261U;   /* FNV-1a 32비트 offset basis */
    usize i;

    for (i = 0; i < len; i++) {
        h ^= (u32)(u8)name[i];
        h *= 16777619U;    /* FNV-1a 32비트 prime */
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* 비트맵 할당자                                                        */
/* ------------------------------------------------------------------ */

static Nstatus nyfs_bitmap_get(nyfs_volume_t *vol, u64 region_start_block, u64 bit_index, bool *out_set)
{
    u64 byte_offset;
    u8 byte;
    Nstatus status;

    byte_offset = region_start_block * (u64)vol->block_size + bit_index / 8U;
    status = nyfs_raw_read(vol, byte_offset, &byte, 1);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    *out_set = (bool)((byte & (u8)(1U << (bit_index % 8U))) != 0);
    return Nok;
}

static Nstatus nyfs_bitmap_put(nyfs_volume_t *vol, u64 region_start_block, u64 bit_index, bool value)
{
    u64 byte_offset;
    u8 byte;
    Nstatus status;

    byte_offset = region_start_block * (u64)vol->block_size + bit_index / 8U;
    status = nyfs_raw_read(vol, byte_offset, &byte, 1);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (value) {
        byte = (u8)(byte | (u8)(1U << (bit_index % 8U)));
    } else {
        byte = (u8)(byte & (u8)~(1U << (bit_index % 8U)));
    }

    return nyfs_raw_write(vol, byte_offset, &byte, 1);
}

static Nstatus nyfs_flush_superblock(nyfs_volume_t *vol)
{
    vol->sb.checksum = 0;
    vol->sb.checksum = nyfs_crc32(&vol->sb, sizeof(vol->sb));
    return nyfs_raw_write(vol, 0, &vol->sb, sizeof(vol->sb));
}

static Nstatus nyfs_alloc_block(nyfs_volume_t *vol, nyfs_blkno_t *out_blkno)
{
    u64 i;
    Nstatus status;

    if (!vol || !out_blkno) {
        return NinvalidArg;
    }

    spin_lock(&vol->lock);

    for (i = 0; i < vol->sb.total_blocks; i++) {
        bool taken;

        status = nyfs_bitmap_get(vol, vol->sb.alloc_meta_start, i, &taken);
        if (NSTATUS_IS_ERR(status)) {
            spin_unlock(&vol->lock);
            return status;
        }

        if (!taken) {
            status = nyfs_bitmap_put(vol, vol->sb.alloc_meta_start, i, true);
            if (NSTATUS_IS_ERR(status)) {
                spin_unlock(&vol->lock);
                return status;
            }

            vol->sb.free_blocks--;
            status = nyfs_flush_superblock(vol);
            spin_unlock(&vol->lock);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            *out_blkno = (nyfs_blkno_t)i;
            return Nok;
        }
    }

    spin_unlock(&vol->lock);
    return NdiskFull;
}

static Nstatus nyfs_free_block(nyfs_volume_t *vol, nyfs_blkno_t blkno)
{
    Nstatus status;

    if (!vol) {
        return NinvalidArg;
    }

    spin_lock(&vol->lock);
    status = nyfs_bitmap_put(vol, vol->sb.alloc_meta_start, (u64)blkno, false);
    if (!NSTATUS_IS_ERR(status)) {
        vol->sb.free_blocks++;
        status = nyfs_flush_superblock(vol);
    }
    spin_unlock(&vol->lock);
    return status;
}

static Nstatus nyfs_alloc_inode(nyfs_volume_t *vol, nyfs_ino_t *out_ino)
{
    u64 i;
    Nstatus status;

    if (!vol || !out_ino) {
        return NinvalidArg;
    }

    spin_lock(&vol->lock);

    for (i = 0; i < vol->sb.inode_count; i++) {
        bool taken;

        status = nyfs_bitmap_get(vol, vol->sb.inode_bitmap_start, i, &taken);
        if (NSTATUS_IS_ERR(status)) {
            spin_unlock(&vol->lock);
            return status;
        }

        if (!taken) {
            status = nyfs_bitmap_put(vol, vol->sb.inode_bitmap_start, i, true);
            if (NSTATUS_IS_ERR(status)) {
                spin_unlock(&vol->lock);
                return status;
            }

            vol->sb.free_inode_count--;
            status = nyfs_flush_superblock(vol);
            spin_unlock(&vol->lock);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            *out_ino = (nyfs_ino_t)i;
            return Nok;
        }
    }

    spin_unlock(&vol->lock);
    return NoutOfMemory;
}

static Nstatus nyfs_free_inode(nyfs_volume_t *vol, nyfs_ino_t ino)
{
    Nstatus status;

    if (!vol || ino == NYFS_INO_NONE || ino >= vol->sb.inode_count) {
        return NinvalidArg;
    }

    spin_lock(&vol->lock);
    status = nyfs_bitmap_put(vol, vol->sb.inode_bitmap_start, (u64)ino, false);
    if (!NSTATUS_IS_ERR(status)) {
        vol->sb.free_inode_count++;
        status = nyfs_flush_superblock(vol);
    }
    spin_unlock(&vol->lock);
    return status;
}

/* ------------------------------------------------------------------ */
/* inode I/O (고정폭 배열이라 ino 로 바로 오프셋을 계산한다 - O(1))         */
/* ------------------------------------------------------------------ */

static Nstatus nyfs_read_inode(nyfs_volume_t *vol, nyfs_ino_t ino, nyfs_inode_t *out)
{
    u64 offset;
    Nstatus status;
    u32 stored_checksum;
    u32 computed_checksum;

    if (!vol || !out || ino == NYFS_INO_NONE || ino >= vol->sb.inode_count) {
        return NinvalidArg;
    }

    offset = vol->sb.inode_table_start * (u64)vol->block_size + ino * (u64)sizeof(nyfs_inode_t);
    status = nyfs_raw_read(vol, offset, out, sizeof(nyfs_inode_t));
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (out->ino != ino) {
        return Ncorrupted;
    }

    stored_checksum = out->checksum;
    out->checksum = 0;
    computed_checksum = nyfs_crc32(out, sizeof(nyfs_inode_t));
    out->checksum = stored_checksum;

    if (computed_checksum != stored_checksum) {
        return Ncorrupted;
    }
    return Nok;
}

static Nstatus nyfs_write_inode(nyfs_volume_t *vol, nyfs_ino_t ino, nyfs_inode_t *in)
{
    u64 offset;

    if (!vol || !in || ino == NYFS_INO_NONE || ino >= vol->sb.inode_count) {
        return NinvalidArg;
    }

    in->ino = ino;
    in->checksum = 0;
    in->checksum = nyfs_crc32(in, sizeof(nyfs_inode_t));

    offset = vol->sb.inode_table_start * (u64)vol->block_size + ino * (u64)sizeof(nyfs_inode_t);
    return nyfs_raw_write(vol, offset, in, sizeof(nyfs_inode_t));
}

/* ------------------------------------------------------------------ */
/* extent 스트림: 데이터 스트림과 디렉터리 해시 영역이 공유하는 유일한       */
/* "성장 가능한 바이트 스트림" 메커니즘.                                  */
/* ------------------------------------------------------------------ */

static Nstatus nyfs_extent_resolve(nyfs_volume_t *vol, nyfs_blkno_t root, u64 logical_block, nyfs_blkno_t *out_block)
{
    nyfs_extent_block_header_t hdr;
    nyfs_extent_t extents[NYFS_MAX_EXTENTS_PER_BLOCK];
    nyfs_blkno_t cur;
    u64 base;
    Nstatus status;

    cur = root;
    base = 0;

    while (cur != NYFS_BLKNO_NONE) {
        u32 i;

        status = nyfs_raw_read(vol, (u64)cur * vol->block_size, &hdr, sizeof(hdr));
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (hdr.magic != NYFS_EXTENT_MAGIC || hdr.extent_count > NYFS_MAX_EXTENTS_PER_BLOCK) {
            return Ncorrupted;
        }

        if (hdr.extent_count > 0) {
            status = nyfs_raw_read(vol, (u64)cur * vol->block_size + sizeof(hdr),
                                    extents, (usize)hdr.extent_count * sizeof(nyfs_extent_t));
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
        }

        for (i = 0; i < hdr.extent_count; i++) {
            if (logical_block >= base && logical_block < base + extents[i].length) {
                *out_block = extents[i].start_block + (logical_block - base);
                return Nok;
            }
            base += extents[i].length;
        }

        cur = hdr.next_block;
    }

    return NnotFound;   /* 아직 할당되지 않은 논리 블록(스트림 끝) */
}

static Nstatus nyfs_extent_total_blocks(nyfs_volume_t *vol, nyfs_blkno_t root, u64 *out_total)
{
    nyfs_extent_block_header_t hdr;
    nyfs_extent_t extents[NYFS_MAX_EXTENTS_PER_BLOCK];
    nyfs_blkno_t cur;
    u64 total;
    Nstatus status;

    cur = root;
    total = 0;

    while (cur != NYFS_BLKNO_NONE) {
        u32 i;

        status = nyfs_raw_read(vol, (u64)cur * vol->block_size, &hdr, sizeof(hdr));
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (hdr.magic != NYFS_EXTENT_MAGIC || hdr.extent_count > NYFS_MAX_EXTENTS_PER_BLOCK) {
            return Ncorrupted;
        }

        if (hdr.extent_count > 0) {
            status = nyfs_raw_read(vol, (u64)cur * vol->block_size + sizeof(hdr),
                                    extents, (usize)hdr.extent_count * sizeof(nyfs_extent_t));
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
            for (i = 0; i < hdr.extent_count; i++) {
                total += extents[i].length;
            }
        }

        cur = hdr.next_block;
    }

    *out_total = total;
    return Nok;
}

/* 스트림 전체(데이터 블록 + extent 리스트 블록 자신)를 회수한다. unlink 로
 * 파일이나 빈 디렉터리를 지울 때 쓴다. 일부만 회수하다 실패해도 이미 회수한
 * 블록은 되돌리지 않는다 - 저널이 없는 v1 에서는 "일부 회수 후 중단"이 최선이며,
 * 최소한 이미 지워진 것을 이중 회수하지는 않는다(같은 블록을 두 번 free 하지 않음). */
static Nstatus nyfs_free_extent_chain(nyfs_volume_t *vol, nyfs_blkno_t root)
{
    nyfs_blkno_t cur;

    cur = root;

    while (cur != NYFS_BLKNO_NONE) {
        nyfs_extent_block_header_t hdr;
        nyfs_extent_t extents[NYFS_MAX_EXTENTS_PER_BLOCK];
        nyfs_blkno_t next;
        Nstatus status;
        u32 i;

        status = nyfs_raw_read(vol, (u64)cur * vol->block_size, &hdr, sizeof(hdr));
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (hdr.magic != NYFS_EXTENT_MAGIC || hdr.extent_count > NYFS_MAX_EXTENTS_PER_BLOCK) {
            return Ncorrupted;
        }

        if (hdr.extent_count > 0) {
            status = nyfs_raw_read(vol, (u64)cur * vol->block_size + sizeof(hdr),
                                    extents, (usize)hdr.extent_count * sizeof(nyfs_extent_t));
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            for (i = 0; i < hdr.extent_count; i++) {
                u64 j;

                for (j = 0; j < extents[i].length; j++) {
                    status = nyfs_free_block(vol, extents[i].start_block + j);
                    if (NSTATUS_IS_ERR(status)) {
                        return status;
                    }
                }
            }
        }

        next = hdr.next_block;
        status = nyfs_free_block(vol, cur);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        cur = next;
    }

    return Nok;
}

/* 스트림 끝에 데이터 블록을 하나 할당해서 붙인다. root 가 비어 있으면(0) 첫
 * extent 리스트 블록도 함께 만든다. */
static Nstatus nyfs_extent_append_one(nyfs_volume_t *vol, nyfs_blkno_t *root)
{
    nyfs_blkno_t new_data_block;
    nyfs_blkno_t last_extent_blk;
    nyfs_extent_block_header_t hdr;
    nyfs_extent_t extents[NYFS_MAX_EXTENTS_PER_BLOCK];
    Nstatus status;

    status = nyfs_alloc_block(vol, &new_data_block);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (*root == NYFS_BLKNO_NONE) {
        nyfs_blkno_t new_extent_blk;

        status = nyfs_alloc_block(vol, &new_extent_blk);
        if (NSTATUS_IS_ERR(status)) {
            (void)nyfs_free_block(vol, new_data_block);
            return status;
        }

        hdr.magic = NYFS_EXTENT_MAGIC;
        hdr.extent_count = 1;
        hdr.next_block = NYFS_BLKNO_NONE;
        extents[0].start_block = new_data_block;
        extents[0].length = 1;

        status = nyfs_raw_write(vol, (u64)new_extent_blk * vol->block_size, &hdr, sizeof(hdr));
        if (!NSTATUS_IS_ERR(status)) {
            status = nyfs_raw_write(vol, (u64)new_extent_blk * vol->block_size + sizeof(hdr),
                                     &extents[0], sizeof(extents[0]));
        }
        if (NSTATUS_IS_ERR(status)) {
            (void)nyfs_free_block(vol, new_data_block);
            (void)nyfs_free_block(vol, new_extent_blk);
            return status;
        }

        *root = new_extent_blk;
        return Nok;
    }

    last_extent_blk = *root;
    for (;;) {
        status = nyfs_raw_read(vol, (u64)last_extent_blk * vol->block_size, &hdr, sizeof(hdr));
        if (NSTATUS_IS_ERR(status)) {
            (void)nyfs_free_block(vol, new_data_block);
            return status;
        }
        if (hdr.magic != NYFS_EXTENT_MAGIC || hdr.extent_count > NYFS_MAX_EXTENTS_PER_BLOCK) {
            (void)nyfs_free_block(vol, new_data_block);
            return Ncorrupted;
        }
        if (hdr.next_block == NYFS_BLKNO_NONE) {
            break;
        }
        last_extent_blk = hdr.next_block;
    }

    if (hdr.extent_count > 0) {
        status = nyfs_raw_read(vol, (u64)last_extent_blk * vol->block_size + sizeof(hdr),
                                extents, (usize)hdr.extent_count * sizeof(nyfs_extent_t));
        if (NSTATUS_IS_ERR(status)) {
            (void)nyfs_free_block(vol, new_data_block);
            return status;
        }
    }

    /* 물리적으로 바로 이어지면 단편화를 피하려고 길이만 늘린다 */
    if (hdr.extent_count > 0 &&
        extents[hdr.extent_count - 1].start_block + extents[hdr.extent_count - 1].length == new_data_block) {
        extents[hdr.extent_count - 1].length++;
        return nyfs_raw_write(vol,
            (u64)last_extent_blk * vol->block_size + sizeof(hdr)
                + (usize)(hdr.extent_count - 1) * sizeof(nyfs_extent_t),
            &extents[hdr.extent_count - 1], sizeof(nyfs_extent_t));
    }

    if (hdr.extent_count < NYFS_MAX_EXTENTS_PER_BLOCK &&
        (u64)(hdr.extent_count + 1U) * sizeof(nyfs_extent_t) + sizeof(hdr) <= vol->block_size) {
        nyfs_extent_t new_extent;

        new_extent.start_block = new_data_block;
        new_extent.length = 1;

        status = nyfs_raw_write(vol,
            (u64)last_extent_blk * vol->block_size + sizeof(hdr)
                + (usize)hdr.extent_count * sizeof(nyfs_extent_t),
            &new_extent, sizeof(new_extent));
        if (NSTATUS_IS_ERR(status)) {
            (void)nyfs_free_block(vol, new_data_block);
            return status;
        }

        hdr.extent_count++;
        return nyfs_raw_write(vol, (u64)last_extent_blk * vol->block_size, &hdr, sizeof(hdr));
    }

    /* 이 extent 블록이 가득 찼다 - 새 extent 블록을 만들어 체인에 잇는다 */
    {
        nyfs_blkno_t new_extent_blk;
        nyfs_extent_block_header_t new_hdr;
        nyfs_extent_t new_extent;

        status = nyfs_alloc_block(vol, &new_extent_blk);
        if (NSTATUS_IS_ERR(status)) {
            (void)nyfs_free_block(vol, new_data_block);
            return status;
        }

        new_hdr.magic = NYFS_EXTENT_MAGIC;
        new_hdr.extent_count = 1;
        new_hdr.next_block = NYFS_BLKNO_NONE;
        new_extent.start_block = new_data_block;
        new_extent.length = 1;

        status = nyfs_raw_write(vol, (u64)new_extent_blk * vol->block_size, &new_hdr, sizeof(new_hdr));
        if (!NSTATUS_IS_ERR(status)) {
            status = nyfs_raw_write(vol, (u64)new_extent_blk * vol->block_size + sizeof(new_hdr),
                                     &new_extent, sizeof(new_extent));
        }
        if (NSTATUS_IS_ERR(status)) {
            (void)nyfs_free_block(vol, new_data_block);
            (void)nyfs_free_block(vol, new_extent_blk);
            return status;
        }

        hdr.next_block = new_extent_blk;
        return nyfs_raw_write(vol, (u64)last_extent_blk * vol->block_size, &hdr, sizeof(hdr));
    }
}

static Nstatus nyfs_extent_read_at(nyfs_volume_t *vol, nyfs_blkno_t root, u64 stream_offset,
                                    void *buffer, usize size, usize *out_read)
{
    usize done;
    u8 *dst;

    if (!vol || !buffer || !out_read) {
        return NinvalidArg;
    }

    done = 0;
    dst = (u8 *)buffer;

    while (done < size) {
        u64 abs_offset = stream_offset + done;
        u64 logical_block = abs_offset / vol->block_size;
        u64 intra = abs_offset % vol->block_size;
        usize chunk = (usize)((u64)vol->block_size - intra);
        nyfs_blkno_t actual;
        Nstatus status;

        if (chunk > size - done) {
            chunk = size - done;
        }

        status = nyfs_extent_resolve(vol, root, logical_block, &actual);
        if (status == NnotFound) {
            break;   /* 스트림 끝 - 더는 못 읽는다 */
        }
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        status = nyfs_raw_read(vol, (u64)actual * vol->block_size + intra, dst + done, chunk);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        done += chunk;
    }

    *out_read = done;
    return Nok;
}

static Nstatus nyfs_extent_write_at(nyfs_volume_t *vol, nyfs_blkno_t *root, u64 stream_offset,
                                     const void *buffer, usize size, usize *out_written)
{
    usize done;
    const u8 *src;
    u64 needed_blocks;
    u64 have_blocks;
    Nstatus status;

    if (!vol || !root || !buffer || !out_written) {
        return NinvalidArg;
    }

    *out_written = 0;
    if (size == 0) {
        return Nok;
    }

    needed_blocks = (stream_offset + size + vol->block_size - 1U) / vol->block_size;

    status = nyfs_extent_total_blocks(vol, *root, &have_blocks);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    while (have_blocks < needed_blocks) {
        status = nyfs_extent_append_one(vol, root);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        have_blocks++;
    }

    done = 0;
    src = (const u8 *)buffer;

    while (done < size) {
        u64 abs_offset = stream_offset + done;
        u64 logical_block = abs_offset / vol->block_size;
        u64 intra = abs_offset % vol->block_size;
        usize chunk = (usize)((u64)vol->block_size - intra);
        nyfs_blkno_t actual;

        if (chunk > size - done) {
            chunk = size - done;
        }

        status = nyfs_extent_resolve(vol, *root, logical_block, &actual);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        status = nyfs_raw_write(vol, (u64)actual * vol->block_size + intra, src + done, chunk);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        done += chunk;
    }

    *out_written = done;
    return Nok;
}

/* ------------------------------------------------------------------ */
/* 권한 검사 - 유일한 판정 지점                                         */
/* ------------------------------------------------------------------ */

static void nyfs_current_cred(nyfs_cred_t *out)
{
    if (current_process) {
        out->uid = current_process->uid;
        out->gid = current_process->gid;
    } else {
        out->uid = 0;
        out->gid = 0;
    }
}

static bool nyfs_ace_id_matches(const nyfs_ace_t *ace, const nyfs_cred_t *cred)
{
    if (ace->id == NYFS_ID_EVERYONE) {
        return true;
    }
    if (ace->flags & NYFS_ACE_FLAG_GROUP) {
        return (bool)(ace->id == cred->gid);
    }
    return (bool)(ace->id == cred->uid);
}

static Nstatus nyfs_gather_aces(nyfs_volume_t *vol, const nyfs_inode_t *inode,
                                 nyfs_ace_t *out, u32 max_out, u32 *out_count)
{
    u32 n;

    n = inode->ace_count;
    if (n > NYFS_ACL_INLINE_COUNT) {
        n = NYFS_ACL_INLINE_COUNT;
    }
    if (n > max_out) {
        n = max_out;
    }
    memcpy(out, inode->ace, (usize)n * sizeof(nyfs_ace_t));

    if (inode->acl_overflow_block != NYFS_BLKNO_NONE && inode->acl_overflow_count > 0 && n < max_out) {
        u32 extra = inode->acl_overflow_count;
        u32 room = max_out - n;
        Nstatus status;

        if (extra > room) {
            extra = room;
        }

        status = nyfs_raw_read(vol, (u64)inode->acl_overflow_block * vol->block_size,
                                out + n, (usize)extra * sizeof(nyfs_ace_t));
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        n += extra;
    }

    *out_count = n;
    return Nok;
}

static Nstatus nyfs_check_access(nyfs_volume_t *vol, const nyfs_inode_t *inode,
                                  const nyfs_cred_t *cred, u32 requested_mask)
{
    if (!inode || !cred) {
        return NinvalidArg;
    }

    if (cred->uid == 0) {
        return Nok;   /* root 는 UNIX 관례대로 모든 검사를 우회한다 */
    }

    if (inode->ace_count == 0 && inode->acl_overflow_count == 0) {
        /* ACL 이 전혀 없으면 순수 UNIX 모드로만 판단한다 */
        u16 mode = inode->unix_mode;
        u32 bits;

        if (cred->uid == inode->uid) {
            bits = (u32)((mode & NYFS_MODE_IRUSR) ? NYFS_ACE_READ_DATA : 0U)
                 | (u32)((mode & NYFS_MODE_IWUSR) ? (NYFS_ACE_WRITE_DATA | NYFS_ACE_APPEND_DATA) : 0U)
                 | (u32)((mode & NYFS_MODE_IXUSR) ? NYFS_ACE_EXECUTE : 0U);
        } else if (cred->gid == inode->gid) {
            bits = (u32)((mode & NYFS_MODE_IRGRP) ? NYFS_ACE_READ_DATA : 0U)
                 | (u32)((mode & NYFS_MODE_IWGRP) ? (NYFS_ACE_WRITE_DATA | NYFS_ACE_APPEND_DATA) : 0U)
                 | (u32)((mode & NYFS_MODE_IXGRP) ? NYFS_ACE_EXECUTE : 0U);
        } else {
            bits = (u32)((mode & NYFS_MODE_IROTH) ? NYFS_ACE_READ_DATA : 0U)
                 | (u32)((mode & NYFS_MODE_IWOTH) ? (NYFS_ACE_WRITE_DATA | NYFS_ACE_APPEND_DATA) : 0U)
                 | (u32)((mode & NYFS_MODE_IXOTH) ? NYFS_ACE_EXECUTE : 0U);
        }

        return ((requested_mask & bits) == requested_mask) ? Nok : Npermission;
    }

    {
        nyfs_ace_t aces[NYFS_MAX_TOTAL_ACES];
        u32 count;
        u32 i;
        u32 granted;
        Nstatus status;

        status = nyfs_gather_aces(vol, inode, aces, NYFS_MAX_TOTAL_ACES, &count);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        /* NTFS 관례: deny 를 먼저 본다. 요청한 비트 중 하나라도 막으면 즉시 거부 */
        for (i = 0; i < count; i++) {
            if (aces[i].type == NYFS_ACE_TYPE_DENY && nyfs_ace_id_matches(&aces[i], cred)) {
                if (aces[i].access_mask & requested_mask) {
                    return Npermission;
                }
            }
        }

        granted = 0;
        for (i = 0; i < count; i++) {
            if (aces[i].type == NYFS_ACE_TYPE_ALLOW && nyfs_ace_id_matches(&aces[i], cred)) {
                granted |= aces[i].access_mask;
            }
        }

        return ((requested_mask & granted) == requested_mask) ? Nok : Npermission;
    }
}

/* ------------------------------------------------------------------ */
/* 디렉터리 해시테이블                                                  */
/* ------------------------------------------------------------------ */

static Nstatus nyfs_dirhash_create(nyfs_volume_t *vol, nyfs_inode_t *dir)
{
    nyfs_dirhash_header_t hdr;
    u64 buckets[NYFS_DIRHASH_DEFAULT_BUCKETS];
    nyfs_blkno_t root;   /* &dir->dir_hash_root 를 바로 넘기면 packed 멤버 주소라 경고/오류가
                          * 나므로(-Werror=address-of-packed-member) 로컬 변수를 거친다 */
    usize written;
    Nstatus status;
    u32 i;

    hdr.magic = NYFS_DIRHASH_MAGIC;
    hdr.bucket_count = NYFS_DIRHASH_DEFAULT_BUCKETS;
    hdr.entry_count = 0;
    hdr.free_offset = (u64)sizeof(hdr) + (u64)sizeof(buckets);
    hdr.checksum = 0;
    hdr.reserved0 = 0;

    for (i = 0; i < NYFS_DIRHASH_DEFAULT_BUCKETS; i++) {
        buckets[i] = 0;
    }

    root = NYFS_BLKNO_NONE;

    status = nyfs_extent_write_at(vol, &root, 0, &hdr, sizeof(hdr), &written);
    if (NSTATUS_IS_ERR(status)) {
        dir->dir_hash_root = root;
        return status;
    }

    status = nyfs_extent_write_at(vol, &root, sizeof(hdr), buckets, sizeof(buckets), &written);
    dir->dir_hash_root = root;
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    dir->dir_entry_count = 0;
    return Nok;
}

static Nstatus nyfs_dirhash_lookup(nyfs_volume_t *vol, const nyfs_inode_t *dir, const char *name,
                                    nyfs_ino_t *out_ino, u32 *out_type)
{
    nyfs_dirhash_header_t hdr;
    usize namelen;
    u32 hash;
    u64 bucket_off;
    u64 cur;
    usize got;
    Nstatus status;

    if (dir->dir_hash_root == NYFS_BLKNO_NONE) {
        return NnotFound;
    }

    namelen = strlen(name);
    if (namelen == 0 || namelen > NYFS_NAME_MAX) {
        return NinvalidArg;
    }

    status = nyfs_extent_read_at(vol, dir->dir_hash_root, 0, &hdr, sizeof(hdr), &got);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (got != sizeof(hdr) || hdr.magic != NYFS_DIRHASH_MAGIC || hdr.bucket_count == 0) {
        return Ncorrupted;
    }

    hash = nyfs_hash_name(name, namelen);

    status = nyfs_extent_read_at(vol, dir->dir_hash_root,
                                  (u64)sizeof(hdr) + (u64)(hash % hdr.bucket_count) * sizeof(u64),
                                  &bucket_off, sizeof(bucket_off), &got);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    cur = bucket_off;
    while (cur != 0) {
        nyfs_dirent_t de;

        status = nyfs_extent_read_at(vol, dir->dir_hash_root, cur, &de, sizeof(de), &got);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (got != sizeof(de)) {
            return Ncorrupted;
        }

        if (de.name_hash == hash && (usize)de.name_len == namelen) {
            char namebuf[NYFS_NAME_MAX + 1U];

            status = nyfs_extent_read_at(vol, dir->dir_hash_root, cur + sizeof(de), namebuf, namelen, &got);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
            namebuf[namelen] = '\0';

            if (memcmp(namebuf, name, namelen) == 0) {
                *out_ino = de.ino;
                *out_type = de.file_type;
                return Nok;
            }
        }

        cur = de.next_offset;
    }

    return NnotFound;
}

static Nstatus nyfs_dirhash_insert(nyfs_volume_t *vol, nyfs_inode_t *dir, const char *name,
                                    nyfs_ino_t ino, u32 file_type)
{
    nyfs_dirhash_header_t hdr;
    usize namelen;
    u32 hash;
    u64 bucket_slot_offset;
    u64 old_head;
    nyfs_dirent_t de;
    u64 record_offset;
    usize record_size;
    usize written;
    usize got;
    nyfs_blkno_t root;
    Nstatus status;

    namelen = strlen(name);
    if (namelen == 0 || namelen > NYFS_NAME_MAX) {
        return NinvalidArg;
    }

    if (dir->dir_hash_root == NYFS_BLKNO_NONE) {
        status = nyfs_dirhash_create(vol, dir);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
    }
    root = dir->dir_hash_root;

    status = nyfs_extent_read_at(vol, root, 0, &hdr, sizeof(hdr), &got);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (got != sizeof(hdr) || hdr.magic != NYFS_DIRHASH_MAGIC || hdr.bucket_count == 0) {
        return Ncorrupted;
    }

    hash = nyfs_hash_name(name, namelen);
    bucket_slot_offset = (u64)sizeof(hdr) + (u64)(hash % hdr.bucket_count) * sizeof(u64);

    status = nyfs_extent_read_at(vol, root, bucket_slot_offset, &old_head, sizeof(old_head), &got);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    de.ino = ino;
    de.next_offset = old_head;
    de.name_hash = hash;
    de.name_len = (u16)namelen;
    de.file_type = (u8)file_type;
    de.reserved0 = 0;

    record_offset = hdr.free_offset;
    record_size = ((usize)sizeof(de) + namelen + 7U) & ~(usize)7U;   /* 다음 레코드가 8바이트 경계에 오도록 */

    status = nyfs_extent_write_at(vol, &root, record_offset, &de, sizeof(de), &written);
    if (NSTATUS_IS_ERR(status)) {
        dir->dir_hash_root = root;
        return status;
    }

    status = nyfs_extent_write_at(vol, &root, record_offset + sizeof(de), name, namelen, &written);
    if (NSTATUS_IS_ERR(status)) {
        dir->dir_hash_root = root;
        return status;
    }

    hdr.free_offset = record_offset + record_size;
    hdr.entry_count++;

    status = nyfs_extent_write_at(vol, &root, bucket_slot_offset, &record_offset, sizeof(record_offset), &written);
    if (NSTATUS_IS_ERR(status)) {
        dir->dir_hash_root = root;
        return status;
    }

    status = nyfs_extent_write_at(vol, &root, 0, &hdr, sizeof(hdr), &written);
    dir->dir_hash_root = root;
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    dir->dir_entry_count = hdr.entry_count;
    return Nok;
}

/* 이름 하나를 체인에서 떼어낸다. 레코드 자체의 바이트는 회수하지 않는다(free_offset
 * 은 여전히 단순 bump 이므로 - nyfs_dirhash_insert 위의 설명과 같은 이유) - 다만
 * 체인에서 빠지고 나면 lookup/readdir 어느 쪽으로도 더는 보이지 않으므로 기능적으로는
 * 완전히 지워진 것과 같다. */
static Nstatus nyfs_dirhash_remove(nyfs_volume_t *vol, nyfs_inode_t *dir, const char *name)
{
    nyfs_dirhash_header_t hdr;
    usize namelen;
    u32 hash;
    u64 bucket_slot_offset;
    u64 prev_offset;   /* 0 = "아직 없음"(= 지금까지 본 게 버킷 슬롯 자신뿐) */
    u64 cur;
    usize got;
    usize written;
    nyfs_blkno_t root;   /* &dir->dir_hash_root 를 바로 넘기면 packed 멤버 주소라
                          * -Werror=address-of-packed-member 에 걸리므로 로컬 변수를 거친다 */
    Nstatus status;

    if (dir->dir_hash_root == NYFS_BLKNO_NONE) {
        return NnotFound;
    }
    root = dir->dir_hash_root;

    namelen = strlen(name);
    if (namelen == 0 || namelen > NYFS_NAME_MAX) {
        return NinvalidArg;
    }

    status = nyfs_extent_read_at(vol, root, 0, &hdr, sizeof(hdr), &got);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (got != sizeof(hdr) || hdr.magic != NYFS_DIRHASH_MAGIC || hdr.bucket_count == 0) {
        return Ncorrupted;
    }

    hash = nyfs_hash_name(name, namelen);
    bucket_slot_offset = (u64)sizeof(hdr) + (u64)(hash % hdr.bucket_count) * sizeof(u64);

    status = nyfs_extent_read_at(vol, root, bucket_slot_offset, &cur, sizeof(cur), &got);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    prev_offset = 0;

    while (cur != 0) {
        nyfs_dirent_t de;

        status = nyfs_extent_read_at(vol, root, cur, &de, sizeof(de), &got);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (got != sizeof(de)) {
            return Ncorrupted;
        }

        if (de.name_hash == hash && (usize)de.name_len == namelen) {
            char namebuf[NYFS_NAME_MAX + 1U];

            status = nyfs_extent_read_at(vol, root, cur + sizeof(de), namebuf, namelen, &got);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
            namebuf[namelen] = '\0';

            if (memcmp(namebuf, name, namelen) == 0) {
                u64 next = de.next_offset;

                if (prev_offset == 0) {
                    status = nyfs_extent_write_at(vol, &root, bucket_slot_offset,
                                                   &next, sizeof(next), &written);
                } else {
                    /* 이전 dirent 의 next_offset 필드(레코드 시작에서 8바이트 뒤, ino
                     * 바로 다음)만 고쳐 쓴다 - 레코드 전체를 다시 쓸 필요는 없다 */
                    status = nyfs_extent_write_at(vol, &root,
                                                   prev_offset + sizeof(u64),
                                                   &next, sizeof(next), &written);
                }
                dir->dir_hash_root = root;
                if (NSTATUS_IS_ERR(status)) {
                    return status;
                }

                hdr.entry_count--;
                status = nyfs_extent_write_at(vol, &root, 0, &hdr, sizeof(hdr), &written);
                dir->dir_hash_root = root;
                if (NSTATUS_IS_ERR(status)) {
                    return status;
                }

                dir->dir_entry_count = hdr.entry_count;
                return Nok;
            }
        }

        prev_offset = cur;
        cur = de.next_offset;
    }

    return NnotFound;
}

/* ------------------------------------------------------------------ */
/* vfs_ops_t                                                           */
/* ------------------------------------------------------------------ */

static Nstatus nyfs_lookup(vnode_t *dir, const char *name, vnode_t **out_vnode)
{
    nyfs_vnode_data_t *dirdata;
    nyfs_inode_t dir_inode;
    nyfs_inode_t child_inode;
    nyfs_cred_t cred;
    nyfs_ino_t child_ino;
    u32 child_type;
    vnode_t *child;
    nyfs_vnode_data_t *childdata;
    Nstatus status;

    if (!dir || !name || !out_vnode) {
        return NinvalidArg;
    }

    dirdata = (nyfs_vnode_data_t *)vfs_get_vnode_private(dir);
    if (!dirdata || !dirdata->vol) {
        return NinvalidArg;
    }

    status = nyfs_read_inode(dirdata->vol, dirdata->ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    nyfs_current_cred(&cred);
    status = nyfs_check_access(dirdata->vol, &dir_inode, &cred, NYFS_ACE_EXECUTE);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_dirhash_lookup(dirdata->vol, &dir_inode, name, &child_ino, &child_type);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_read_inode(dirdata->vol, child_ino, &child_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    child = vfs_alloc_vnode();
    if (!child) {
        return NoutOfMemory;
    }

    childdata = nyfs_alloc_vnode_data();
    if (!childdata) {
        vfs_free_vnode(child);
        return NoutOfMemory;
    }
    childdata->vol = dirdata->vol;
    childdata->ino = child_ino;

    status = vfs_init_vnode(child, child_ino, (child_inode.type == NYFS_INO_TYPE_DIR) ? 2U : 1U,
                             &g_nyfs_ops, nNULL, childdata);
    if (NSTATUS_IS_ERR(status)) {
        nyfs_free_vnode_data(childdata);
        vfs_free_vnode(child);
        return status;
    }

    *out_vnode = child;
    return Nok;
}

static Nstatus nyfs_open(vnode_t *vnode, u32 flags, handle_t *out_handle)
{
    nyfs_vnode_data_t *data;
    nyfs_inode_t inode;
    nyfs_cred_t cred;
    u32 requested;
    nyfs_vnode_data_t *hdata;
    Nstatus status;

    if (!vnode || !out_handle) {
        return NinvalidArg;
    }

    data = (nyfs_vnode_data_t *)vfs_get_vnode_private(vnode);
    if (!data || !data->vol) {
        return NinvalidArg;
    }

    status = nyfs_read_inode(data->vol, data->ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    requested = 0;
    if (flags & NX_O_READ) {
        requested |= NYFS_ACE_READ_DATA;
    }
    if (flags & NX_O_WRITE) {
        requested |= NYFS_ACE_WRITE_DATA;
    }

    nyfs_current_cred(&cred);
    status = nyfs_check_access(data->vol, &inode, &cred, requested);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if ((flags & NX_O_WRITE) && !data->vol->write) {
        return NreadOnly;
    }

    hdata = nyfs_alloc_handle_data();
    if (!hdata) {
        return NoutOfMemory;
    }
    hdata->vol = data->vol;
    hdata->ino = data->ino;

    out_handle->fs_private = hdata;
    return Nok;
}

static Nstatus nyfs_read(handle_t *handle, void *buffer, usize size, usize *bytes_read)
{
    nyfs_vnode_data_t *hdata;
    nyfs_inode_t inode;
    usize avail;
    usize to_read;
    Nstatus status;

    if (!handle || !buffer || !bytes_read) {
        return NinvalidArg;
    }

    hdata = (nyfs_vnode_data_t *)handle->fs_private;
    if (!hdata || !hdata->vol) {
        return NinvalidArg;
    }

    status = nyfs_read_inode(hdata->vol, hdata->ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (handle->offset >= inode.size) {
        *bytes_read = 0;
        return Nok;
    }

    avail = (usize)(inode.size - handle->offset);
    to_read = (size < avail) ? size : avail;

    return nyfs_extent_read_at(hdata->vol, inode.data_extent_root, handle->offset, buffer, to_read, bytes_read);
}

static Nstatus nyfs_write(handle_t *handle, const void *buffer, usize size, usize *bytes_written)
{
    nyfs_vnode_data_t *hdata;
    nyfs_inode_t inode;
    nyfs_blkno_t root;   /* &inode.data_extent_root 를 바로 넘기면 packed 멤버 주소라
                          * -Werror=address-of-packed-member 에 걸리므로 로컬 변수를 거친다 */
    usize written;
    Nstatus status;

    if (!handle || !buffer || !bytes_written) {
        return NinvalidArg;
    }

    hdata = (nyfs_vnode_data_t *)handle->fs_private;
    if (!hdata || !hdata->vol) {
        return NinvalidArg;
    }
    if (!hdata->vol->write) {
        return NreadOnly;
    }

    status = nyfs_read_inode(hdata->vol, hdata->ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    root = inode.data_extent_root;
    status = nyfs_extent_write_at(hdata->vol, &root, handle->offset, buffer, size, &written);
    inode.data_extent_root = root;
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (handle->offset + written > inode.size) {
        inode.size = handle->offset + written;
    }

    status = nyfs_write_inode(hdata->vol, hdata->ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    *bytes_written = written;
    return Nok;
}

static Nstatus nyfs_close(handle_t *handle)
{
    if (!handle) {
        return NinvalidArg;
    }
    nyfs_free_handle_data((nyfs_vnode_data_t *)handle->fs_private);
    handle->fs_private = nNULL;
    return Nok;
}

static Nstatus nyfs_stat(vnode_t *vnode, vfs_stat_t *out)
{
    nyfs_vnode_data_t *data;
    nyfs_inode_t inode;
    Nstatus status;

    if (!vnode || !out) {
        return NinvalidArg;
    }

    data = (nyfs_vnode_data_t *)vfs_get_vnode_private(vnode);
    if (!data || !data->vol) {
        return NinvalidArg;
    }

    status = nyfs_read_inode(data->vol, data->ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    out->size = inode.size;
    out->type = (inode.type == NYFS_INO_TYPE_DIR) ? 2U : 1U;
    out->reserved = 0;
    return Nok;
}

static Nstatus nyfs_create(vnode_t *dir, const char *name, u32 mode, vnode_t **out_vnode)
{
    nyfs_vnode_data_t *dirdata;
    nyfs_inode_t dir_inode;
    nyfs_inode_t new_inode;
    nyfs_cred_t cred;
    nyfs_ino_t new_ino;
    nyfs_ino_t existing_ino;
    u32 existing_type;
    vnode_t *child;
    nyfs_vnode_data_t *childdata;
    Nstatus status;

    if (!dir || !name || !out_vnode) {
        return NinvalidArg;
    }

    dirdata = (nyfs_vnode_data_t *)vfs_get_vnode_private(dir);
    if (!dirdata || !dirdata->vol) {
        return NinvalidArg;
    }
    if (!dirdata->vol->write) {
        return NreadOnly;
    }

    status = nyfs_read_inode(dirdata->vol, dirdata->ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    nyfs_current_cred(&cred);
    status = nyfs_check_access(dirdata->vol, &dir_inode, &cred, NYFS_ACE_WRITE_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    /* vfs_create() 에는 O_EXCL 같은 플래그가 없으니 항상 "이미 있으면 거부"로 동작한다 */
    status = nyfs_dirhash_lookup(dirdata->vol, &dir_inode, name, &existing_ino, &existing_type);
    if (!NSTATUS_IS_ERR(status)) {
        return NalreadyExists;
    }
    if (status != NnotFound) {
        return status;
    }

    status = nyfs_alloc_inode(dirdata->vol, &new_ino);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.ino = new_ino;
    new_inode.type = NYFS_INO_TYPE_FILE;
    new_inode.unix_mode = (u16)(mode & 0x0FFFU);   /* rwxrwxrwx + set-uid/set-gid/sticky (12비트) */
    new_inode.uid = cred.uid;
    new_inode.gid = cred.gid;
    new_inode.link_count = 1;

    status = nyfs_write_inode(dirdata->vol, new_ino, &new_inode);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(dirdata->vol, new_ino);
        return status;
    }

    status = nyfs_dirhash_insert(dirdata->vol, &dir_inode, name, new_ino, NYFS_DIRENT_TYPE_FILE);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(dirdata->vol, new_ino);
        return status;
    }

    /* dirhash_insert 가 dir_inode.dir_hash_root/dir_entry_count 를 바꿨을 수 있으니
     * 디렉터리 inode 를 다시 써서 반영한다 */
    status = nyfs_write_inode(dirdata->vol, dirdata->ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    child = vfs_alloc_vnode();
    if (!child) {
        return NoutOfMemory;
    }

    childdata = nyfs_alloc_vnode_data();
    if (!childdata) {
        vfs_free_vnode(child);
        return NoutOfMemory;
    }
    childdata->vol = dirdata->vol;
    childdata->ino = new_ino;

    status = vfs_init_vnode(child, new_ino, 1U, &g_nyfs_ops, nNULL, childdata);
    if (NSTATUS_IS_ERR(status)) {
        nyfs_free_vnode_data(childdata);
        vfs_free_vnode(child);
        return status;
    }

    *out_vnode = child;
    return Nok;
}

static Nstatus nyfs_mkdir(vnode_t *dir, const char *name, u32 mode, vnode_t **out_vnode)
{
    nyfs_vnode_data_t *dirdata;
    nyfs_inode_t dir_inode;
    nyfs_inode_t new_inode;
    nyfs_cred_t cred;
    nyfs_ino_t new_ino;
    nyfs_ino_t existing_ino;
    u32 existing_type;
    vnode_t *child;
    nyfs_vnode_data_t *childdata;
    Nstatus status;

    if (!dir || !name || !out_vnode) {
        return NinvalidArg;
    }

    dirdata = (nyfs_vnode_data_t *)vfs_get_vnode_private(dir);
    if (!dirdata || !dirdata->vol) {
        return NinvalidArg;
    }
    if (!dirdata->vol->write) {
        return NreadOnly;
    }

    status = nyfs_read_inode(dirdata->vol, dirdata->ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    nyfs_current_cred(&cred);
    status = nyfs_check_access(dirdata->vol, &dir_inode, &cred, NYFS_ACE_WRITE_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_dirhash_lookup(dirdata->vol, &dir_inode, name, &existing_ino, &existing_type);
    if (!NSTATUS_IS_ERR(status)) {
        return NalreadyExists;
    }
    if (status != NnotFound) {
        return status;
    }

    status = nyfs_alloc_inode(dirdata->vol, &new_ino);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.ino = new_ino;
    new_inode.type = NYFS_INO_TYPE_DIR;
    new_inode.unix_mode = (u16)(mode & 0x0FFFU);
    new_inode.uid = cred.uid;
    new_inode.gid = cred.gid;
    new_inode.link_count = 1;

    status = nyfs_dirhash_create(dirdata->vol, &new_inode);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(dirdata->vol, new_ino);
        return status;
    }

    status = nyfs_write_inode(dirdata->vol, new_ino, &new_inode);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(dirdata->vol, new_ino);
        return status;
    }

    status = nyfs_dirhash_insert(dirdata->vol, &dir_inode, name, new_ino, NYFS_DIRENT_TYPE_DIR);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(dirdata->vol, new_ino);
        return status;
    }

    status = nyfs_write_inode(dirdata->vol, dirdata->ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    child = vfs_alloc_vnode();
    if (!child) {
        return NoutOfMemory;
    }

    childdata = nyfs_alloc_vnode_data();
    if (!childdata) {
        vfs_free_vnode(child);
        return NoutOfMemory;
    }
    childdata->vol = dirdata->vol;
    childdata->ino = new_ino;

    status = vfs_init_vnode(child, new_ino, 2U, &g_nyfs_ops, nNULL, childdata);
    if (NSTATUS_IS_ERR(status)) {
        nyfs_free_vnode_data(childdata);
        vfs_free_vnode(child);
        return status;
    }

    *out_vnode = child;
    return Nok;
}

static Nstatus nyfs_unlink(vnode_t *dir, const char *name)
{
    nyfs_vnode_data_t *dirdata;
    nyfs_inode_t dir_inode;
    nyfs_inode_t target_inode;
    nyfs_cred_t cred;
    nyfs_ino_t target_ino;
    u32 target_type;
    Nstatus status;

    if (!dir || !name) {
        return NinvalidArg;
    }

    dirdata = (nyfs_vnode_data_t *)vfs_get_vnode_private(dir);
    if (!dirdata || !dirdata->vol) {
        return NinvalidArg;
    }
    if (!dirdata->vol->write) {
        return NreadOnly;
    }

    status = nyfs_read_inode(dirdata->vol, dirdata->ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    nyfs_current_cred(&cred);
    status = nyfs_check_access(dirdata->vol, &dir_inode, &cred, NYFS_ACE_DELETE_CHILD);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_dirhash_lookup(dirdata->vol, &dir_inode, name, &target_ino, &target_type);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_read_inode(dirdata->vol, target_ino, &target_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (target_inode.type == NYFS_INO_TYPE_DIR && target_inode.dir_entry_count > 0) {
        /* "디렉터리가 비어있지 않음" 전용 에러 코드가 없어 Nbusy 로 대신한다 -
         * 연산 자체는 지원하지만(Nunsupported 가 아니라) 지금 상태에서는 거부한다는 뜻 */
        return Nbusy;
    }

    status = nyfs_dirhash_remove(dirdata->vol, &dir_inode, name);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_write_inode(dirdata->vol, dirdata->ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (target_inode.link_count > 0) {
        target_inode.link_count--;
    }

    if (target_inode.link_count == 0) {
        /* 저널이 없어 아래 두 회수가 중간에 끊기면 블록이 새는(leak) 상태로 남을 수
         * 있다 - 저널을 붙일 때 반드시 같이 다뤄야 할 지점이다(파일 상단 주석 참고) */
        if (target_inode.type == NYFS_INO_TYPE_DIR) {
            (void)nyfs_free_extent_chain(dirdata->vol, target_inode.dir_hash_root);
        }
        (void)nyfs_free_extent_chain(dirdata->vol, target_inode.data_extent_root);
        (void)nyfs_free_inode(dirdata->vol, target_ino);
    } else {
        (void)nyfs_write_inode(dirdata->vol, target_ino, &target_inode);
    }

    return Nok;
}

static Nstatus nyfs_readdir(vnode_t *dir, u64 index, char *name_out, usize name_out_max, u32 *type_out)
{
    nyfs_vnode_data_t *dirdata;
    nyfs_inode_t dir_inode;
    nyfs_cred_t cred;
    nyfs_dirhash_header_t hdr;
    usize got;
    u64 seen;
    u32 b;
    Nstatus status;

    if (!dir || !name_out || name_out_max == 0 || !type_out) {
        return NinvalidArg;
    }

    dirdata = (nyfs_vnode_data_t *)vfs_get_vnode_private(dir);
    if (!dirdata || !dirdata->vol) {
        return NinvalidArg;
    }

    status = nyfs_read_inode(dirdata->vol, dirdata->ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    nyfs_current_cred(&cred);
    status = nyfs_check_access(dirdata->vol, &dir_inode, &cred, NYFS_ACE_READ_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (dir_inode.dir_hash_root == NYFS_BLKNO_NONE) {
        return NnotFound;
    }

    status = nyfs_extent_read_at(dirdata->vol, dir_inode.dir_hash_root, 0, &hdr, sizeof(hdr), &got);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (got != sizeof(hdr) || hdr.magic != NYFS_DIRHASH_MAGIC || hdr.bucket_count == 0) {
        return Ncorrupted;
    }

    seen = 0;
    for (b = 0; b < hdr.bucket_count; b++) {
        u64 bucket_off;
        u64 cur;

        status = nyfs_extent_read_at(dirdata->vol, dir_inode.dir_hash_root,
                                      (u64)sizeof(hdr) + (u64)b * sizeof(u64),
                                      &bucket_off, sizeof(bucket_off), &got);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        cur = bucket_off;
        while (cur != 0) {
            nyfs_dirent_t de;

            status = nyfs_extent_read_at(dirdata->vol, dir_inode.dir_hash_root, cur, &de, sizeof(de), &got);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
            if (got != sizeof(de)) {
                return Ncorrupted;
            }

            if (seen == index) {
                usize copy_len = (usize)de.name_len;

                if (copy_len >= name_out_max) {
                    copy_len = name_out_max - 1U;
                }

                status = nyfs_extent_read_at(dirdata->vol, dir_inode.dir_hash_root, cur + sizeof(de),
                                              name_out, copy_len, &got);
                if (NSTATUS_IS_ERR(status)) {
                    return status;
                }
                name_out[copy_len] = '\0';
                *type_out = de.file_type;
                return Nok;
            }

            seen++;
            cur = de.next_offset;
        }
    }

    return NnotFound;
}

/* ------------------------------------------------------------------ */
/* mount / unmount / format                                            */
/* ------------------------------------------------------------------ */

Nstatus nyfs_mount(u32 diskno, void *userdata)
{
    nyfs_mount_params_t *params;
    nyfs_volume_t *vol;
    vnode_t *root;
    nyfs_vnode_data_t *rootdata;
    u32 stored_checksum;
    u32 computed_checksum;
    Nstatus status;

    if (!userdata) {
        return NinvalidArg;
    }
    params = (nyfs_mount_params_t *)userdata;
    if (!params->read) {
        return NinvalidArg;
    }

    if (nyfs_find_volume(diskno)) {
        return NalreadyExists;
    }

    vol = nyfs_alloc_volume(diskno);
    if (!vol) {
        return NtooManyFileSystem;
    }

    vol->read = params->read;
    vol->write = params->write;
    vol->info = params->info;

    status = nyfs_raw_read(vol, 0, &vol->sb, sizeof(vol->sb));
    if (NSTATUS_IS_ERR(status)) {
        nyfs_free_volume(vol);
        return status;
    }

    if (vol->sb.magic != NYFS_MAGIC || vol->sb.superblock_size != sizeof(nyfs_superblock_t)
        || vol->sb.version != NYFS_VERSION) {
        nyfs_free_volume(vol);
        return NbadFilesystem;
    }
    if (vol->sb.block_size_shift < NYFS_BLOCK_SIZE_SHIFT_MIN
        || vol->sb.block_size_shift > NYFS_BLOCK_SIZE_SHIFT_MAX) {
        nyfs_free_volume(vol);
        return NbadFilesystem;
    }

    stored_checksum = vol->sb.checksum;
    vol->sb.checksum = 0;
    computed_checksum = nyfs_crc32(&vol->sb, sizeof(vol->sb));
    vol->sb.checksum = stored_checksum;
    if (computed_checksum != stored_checksum) {
        nyfs_free_volume(vol);
        return Ncorrupted;
    }

    vol->block_size = 1U << vol->sb.block_size_shift;

    if (params->mount_path) {
        if (strnlen(params->mount_path, sizeof(vol->mount_path)) >= sizeof(vol->mount_path)) {
            nyfs_free_volume(vol);
            return NpathTooLong;
        }
        strncpy(vol->mount_path, params->mount_path, sizeof(vol->mount_path) - 1U);
    } else {
        strncpy(vol->mount_path, "/", sizeof(vol->mount_path) - 1U);
    }
    vol->mount_path[sizeof(vol->mount_path) - 1U] = '\0';

    root = vfs_alloc_vnode();
    if (!root) {
        nyfs_free_volume(vol);
        return NoutOfMemory;
    }

    rootdata = nyfs_alloc_vnode_data();
    if (!rootdata) {
        vfs_free_vnode(root);
        nyfs_free_volume(vol);
        return NoutOfMemory;
    }
    rootdata->vol = vol;
    rootdata->ino = NYFS_ROOT_INO;

    status = vfs_init_vnode(root, NYFS_ROOT_INO, 2U, &g_nyfs_ops, nNULL, rootdata);
    if (NSTATUS_IS_ERR(status)) {
        nyfs_free_vnode_data(rootdata);
        vfs_free_vnode(root);
        nyfs_free_volume(vol);
        return status;
    }

    vol->root_vnode = root;

    memset(&vol->fs, 0, sizeof(vol->fs));
    vol->fs.name = "nyfs";
    vol->fs.sb = nNULL;
    vol->fs.ops = &g_nyfs_ops;
    vol->fs.root = root;
    vol->fs.priv = vol;

    status = vfs_mount(vol->mount_path, &vol->fs);
    if (NSTATUS_IS_ERR(status)) {
        nyfs_free_vnode_data(rootdata);
        vfs_free_vnode(root);
        vol->root_vnode = nNULL;
        nyfs_free_volume(vol);
        return status;
    }

    return Nok;
}

Nstatus nyfs_unmount(u32 diskno)
{
    nyfs_volume_t *vol = nyfs_find_volume(diskno);

    if (!vol) {
        return NnotFound;
    }

    if (vol->mount_path[0] != '\0') {
        (void)vfs_unmount(vol->mount_path);
    }

    if (vol->root_vnode) {
        nyfs_free_vnode_data((nyfs_vnode_data_t *)vfs_get_vnode_private(vol->root_vnode));
        vfs_free_vnode(vol->root_vnode);
        vol->root_vnode = nNULL;
    }

    nyfs_free_volume(vol);
    return Nok;
}

Nstatus nyfs_format(u32 diskno, void *userdata)
{
    nyfs_mount_params_t *params;
    nyfs_volume_t vol;   /* g_volumes 에 등록하지 않는 임시 볼륨. 위의 alloc/read/write
                          * 헬퍼들을 그대로 재사용하려고 스택에 만든다 */
    nyfs_superblock_t sb;
    u64 inode_count;
    u64 inode_bitmap_bytes, inode_bitmap_blocks;
    u64 block_bitmap_bytes, block_bitmap_blocks;
    u64 inode_table_bytes, inode_table_blocks;
    u64 first_data_block;
    u64 i;
    nyfs_inode_t root_inode;
    Nstatus status;

    (void)diskno;

    if (!userdata) {
        return NinvalidArg;
    }
    params = (nyfs_mount_params_t *)userdata;

    if (!params->read || !params->write) {
        return NinvalidArg;
    }
    if (params->block_size_shift < NYFS_BLOCK_SIZE_SHIFT_MIN
        || params->block_size_shift > NYFS_BLOCK_SIZE_SHIFT_MAX) {
        return NinvalidArg;
    }
    if (params->total_blocks < 64U) {
        return NinvalidArg;   /* 메타데이터 + 루트 디렉터리도 못 담을 크기 */
    }

    memset(&vol, 0, sizeof(vol));
    vol.diskno = diskno;
    vol.read = params->read;
    vol.write = params->write;
    vol.info = params->info;
    vol.block_size = 1U << params->block_size_shift;

    inode_count = params->inode_count;
    if (inode_count == 0) {
        inode_count = params->total_blocks / 8U;
        if (inode_count < 16U) {
            inode_count = 16U;
        }
    }

    inode_bitmap_bytes = (inode_count + 7U) / 8U;
    inode_bitmap_blocks = (inode_bitmap_bytes + vol.block_size - 1U) / vol.block_size;
    if (inode_bitmap_blocks == 0) {
        inode_bitmap_blocks = 1;
    }

    block_bitmap_bytes = (params->total_blocks + 7U) / 8U;
    block_bitmap_blocks = (block_bitmap_bytes + vol.block_size - 1U) / vol.block_size;
    if (block_bitmap_blocks == 0) {
        block_bitmap_blocks = 1;
    }

    inode_table_bytes = inode_count * (u64)sizeof(nyfs_inode_t);
    inode_table_blocks = (inode_table_bytes + vol.block_size - 1U) / vol.block_size;
    if (inode_table_blocks == 0) {
        inode_table_blocks = 1;
    }

    /* 레이아웃: [0]=슈퍼블록, 그 뒤로 inode 비트맵, 블록 비트맵, inode 테이블, 나머지=데이터 */
    memset(&sb, 0, sizeof(sb));
    sb.magic = NYFS_MAGIC;
    sb.version = NYFS_VERSION;
    sb.superblock_size = sizeof(sb);
    sb.block_size_shift = params->block_size_shift;
    sb.alloc_algorithm = NYFS_ALLOC_BITMAP;
    sb.total_blocks = params->total_blocks;
    sb.inode_count = inode_count;
    sb.inode_bitmap_start = 1;
    sb.alloc_meta_start = sb.inode_bitmap_start + inode_bitmap_blocks;
    sb.alloc_meta_blocks = block_bitmap_blocks;
    sb.inode_table_start = sb.alloc_meta_start + block_bitmap_blocks;

    first_data_block = sb.inode_table_start + inode_table_blocks;
    if (first_data_block >= params->total_blocks) {
        return NinvalidArg;   /* 볼륨이 메타데이터조차 못 담을 만큼 작다 */
    }

    sb.free_blocks = params->total_blocks - first_data_block;
    sb.free_inode_count = inode_count;   /* inode 0 예약과 루트 디렉터리 할당에서 각각 -1 됨 */
    sb.journal_version = 0;
    sb.journal_start = 0;
    sb.journal_blocks = 0;

    vol.sb = sb;

    status = nyfs_flush_superblock(&vol);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    /* 두 비트맵 영역을 전부 0(비어있음)으로 지운다. mkfs 는 한 번만 실행되므로
     * 바이트 단위로 느리게 쓰더라도 상관없다 - 단순함을 우선한다. */
    for (i = 0; i < inode_bitmap_blocks + block_bitmap_blocks; i++) {
        u64 b;

        for (b = 0; b < vol.block_size; b++) {
            u8 zero = 0;

            status = nyfs_raw_write(&vol, (sb.inode_bitmap_start + i) * vol.block_size + b, &zero, 1);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
        }
    }

    /* 메타데이터가 차지하는 블록을 전부 "사용 중"으로 표시해서 할당기가 건드리지
     * 못하게 한다(블록 비트맵의 좌표계는 블록 번호 0부터 시작한다). */
    for (i = 0; i < first_data_block; i++) {
        status = nyfs_bitmap_put(&vol, sb.alloc_meta_start, i, true);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
    }

    /* inode 0 은 NYFS_INO_NONE 전용이라 영구히 예약한다(할당자를 거치지 않고 직접
     * 표시 - free_inode_count 는 애초에 이 자리를 감안해서 아래에서 계산한다) */
    status = nyfs_bitmap_put(&vol, sb.inode_bitmap_start, 0, true);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    vol.sb.free_inode_count--;
    status = nyfs_flush_superblock(&vol);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    /* 루트 디렉터리는 실제 할당자를 거쳐 번호를 받는다(비트 0 을 방금 예약했으니
     * 반드시 1 = NYFS_ROOT_INO 가 나와야 한다 - 그렇지 않으면 포맷 로직 자체가
     * 잘못된 것이므로 방어적으로 확인한다). */
    {
        nyfs_ino_t root_ino;

        status = nyfs_alloc_inode(&vol, &root_ino);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (root_ino != NYFS_ROOT_INO) {
            return Ncorrupted;
        }
    }

    /* 루트 디렉터리(inode 1) */
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.ino = NYFS_ROOT_INO;
    root_inode.type = NYFS_INO_TYPE_DIR;
    root_inode.unix_mode = (u16)(NYFS_MODE_IRUSR | NYFS_MODE_IWUSR | NYFS_MODE_IXUSR
                                | NYFS_MODE_IRGRP | NYFS_MODE_IXGRP
                                | NYFS_MODE_IROTH | NYFS_MODE_IXOTH);
    root_inode.uid = 0;
    root_inode.gid = 0;
    root_inode.link_count = 1;

    status = nyfs_dirhash_create(&vol, &root_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_write_inode(&vol, NYFS_ROOT_INO, &root_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    /* 위 단계들에서 vol.sb.free_blocks/free_inode_count 가 바뀌었으니 최신 값으로
     * 슈퍼블록을 다시 쓴다(각 alloc 호출이 이미 썼겠지만, 안전하게 한 번 더 -
     * nyfs_flush_superblock 을 거쳐야 체크섬도 같이 갱신된다) */
    return nyfs_flush_superblock(&vol);
}

Nstatus nyfs_register(void)
{
    filesystem_ops_t ops;

    memset(&g_nyfs_ops, 0, sizeof(g_nyfs_ops));
    g_nyfs_ops.lookup = nyfs_lookup;
    g_nyfs_ops.open = nyfs_open;
    g_nyfs_ops.read = nyfs_read;
    g_nyfs_ops.write = nyfs_write;
    g_nyfs_ops.close = nyfs_close;
    g_nyfs_ops.stat = nyfs_stat;
    g_nyfs_ops.create = nyfs_create;
    g_nyfs_ops.mkdir = nyfs_mkdir;
    g_nyfs_ops.unlink = nyfs_unlink;
    g_nyfs_ops.readdir = nyfs_readdir;

    ops.mount = nyfs_mount;
    ops.unmount = nyfs_unmount;
    ops.format = nyfs_format;

    return filesystem_register("nyfs", ops);
}
