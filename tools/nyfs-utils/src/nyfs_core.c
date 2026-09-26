/*
 * nyfs_core.c - NyFS 온디스크 로직의 리눅스 호스트 이식.
 *
 * nyxis-os 커널 drivers/filesystem/nyfs/nyfs.c 를 원본으로 삼아, 비트맵
 * 할당자/inode 테이블/extent 체인/디렉터리 해시/권한 검사 로직을 "같은
 * 알고리즘, 같은 바이트 배치"로 그대로 옮겼다. 함수 이름과 구조를 최대한
 * 원본과 1:1 로 맞춰서, 커널 쪽이 바뀌면 이 파일에서 어디를 같이 고쳐야
 * 하는지 바로 알 수 있게 했다.
 *
 * I/O 계층만 다르다: 커널은 diskno + 콜백(ATA/AHCI/램디스크)을 쓰고, 여기는
 * 열린 파일 서술자 하나에 대한 pread(2)/pwrite(2) 를 쓴다.
 *
 * 빌드: -std=c89 -pedantic -Wall -Wextra -Werror -Wstrict-prototypes
 *       -Wmissing-prototypes -Wshadow -Wdeclaration-after-statement
 * (프로젝트 최상위 빌드 규칙과 동일 - Makefile 참고)
 *
 * pread/pwrite/ftruncate 는 POSIX 함수라 <unistd.h> 가 -std=c89 아래서도
 * 선언을 노출하도록 _POSIX_C_SOURCE 를 먼저 정의한다(글리백 특성:
 * -std=c89 는 __STRICT_ANSI__ 를 켜서 기본적으로 POSIX 선언을 감춘다).
 */
#define _POSIX_C_SOURCE 200809L

#include "nyfs_core.h"

#include <unistd.h>   /* pread, pwrite, ftruncate */
#include <string.h>   /* memcpy, memset, memcmp, strlen */
#include <sys/stat.h> /* fstat */
#include <errno.h>

/* extent 블록 하나에 들어갈 수 있는 extent 개수의 상한. 커널과 동일한 값을
 * 써야 하는 건 아니지만(이건 순전히 "한 번에 스택에 얼마나 올려두고
 * 처리하는가"의 문제라 온디스크 포맷과 무관), 코드를 비교하기 쉽게 커널과
 * 같은 값을 쓴다. */
#define NYFS_MAX_EXTENTS_PER_BLOCK    128U
#define NYFS_MAX_TOTAL_ACES           64U
#define NYFS_DIRHASH_DEFAULT_BUCKETS  16U

/* ------------------------------------------------------------------ */
/* 원시 I/O                                                            */
/* ------------------------------------------------------------------ */

static nyfs_status_t nyfs_raw_read(nyfs_volume_t *vol, u64 offset, void *buffer, usize size)
{
    usize done;
    u8 *dst;

    if (!vol || !buffer) {
        return NinvalidArg;
    }

    done = 0;
    dst = (u8 *)buffer;
    while (done < size) {
        ssize_t n = pread(vol->fd, dst + done, size - done, (off_t)(offset + done));

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Nio;
        }
        if (n == 0) {
            /* 파일 끝(스파스 이미지의 아직 안 써진 뒷부분 등) - 남은 부분을
             * 0 으로 채운다. mkfs 가 비트맵/메타데이터 영역을 실제로 0 으로
             * 써 두므로 정상 경로에서는 거의 발생하지 않지만, 정규 파일을
             * ftruncate 로만 늘려둔 데이터 영역 끝부분 등에서 나올 수 있다. */
            memset(dst + done, 0, size - done);
            return Nok;
        }
        done += (usize)n;
    }
    return Nok;
}

static nyfs_status_t nyfs_raw_write(nyfs_volume_t *vol, u64 offset, const void *buffer, usize size)
{
    usize done;
    const u8 *src;

    if (!vol || !buffer) {
        return NinvalidArg;
    }
    if (!vol->writable) {
        return NreadOnly;
    }

    done = 0;
    src = (const u8 *)buffer;
    while (done < size) {
        ssize_t n = pwrite(vol->fd, src + done, size - done, (off_t)(offset + done));

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Nio;
        }
        done += (usize)n;
    }
    return Nok;
}

/* ------------------------------------------------------------------ */
/* CRC32 (표준 IEEE 802.3 다항식) - 커널 nyfs_crc32() 와 완전히 동일        */
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
    u32 h = 2166136261U;
    usize i;

    for (i = 0; i < len; i++) {
        h ^= (u32)(u8)name[i];
        h *= 16777619U;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* 비트맵 할당자                                                        */
/* ------------------------------------------------------------------ */

static nyfs_status_t nyfs_bitmap_get(nyfs_volume_t *vol, u64 region_start_block, u64 bit_index, nbool *out_set)
{
    u64 byte_offset;
    u8 byte;
    nyfs_status_t status;

    byte_offset = region_start_block * (u64)vol->block_size + bit_index / 8U;
    status = nyfs_raw_read(vol, byte_offset, &byte, 1);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    *out_set = (nbool)((byte & (u8)(1U << (bit_index % 8U))) != 0);
    return Nok;
}

static nyfs_status_t nyfs_bitmap_put(nyfs_volume_t *vol, u64 region_start_block, u64 bit_index, nbool value)
{
    u64 byte_offset;
    u8 byte;
    nyfs_status_t status;

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

static nyfs_status_t nyfs_flush_superblock(nyfs_volume_t *vol)
{
    vol->sb.checksum = 0;
    vol->sb.checksum = nyfs_crc32(&vol->sb, sizeof(vol->sb));
    return nyfs_raw_write(vol, 0, &vol->sb, sizeof(vol->sb));
}

static nyfs_status_t nyfs_alloc_block(nyfs_volume_t *vol, nyfs_blkno_t *out_blkno)
{
    u64 i;
    nyfs_status_t status;

    if (!vol || !out_blkno) {
        return NinvalidArg;
    }

    for (i = 0; i < vol->sb.total_blocks; i++) {
        nbool taken;

        status = nyfs_bitmap_get(vol, vol->sb.alloc_meta_start, i, &taken);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        if (!taken) {
            status = nyfs_bitmap_put(vol, vol->sb.alloc_meta_start, i, ntrue);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            vol->sb.free_blocks--;
            status = nyfs_flush_superblock(vol);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            *out_blkno = (nyfs_blkno_t)i;
            return Nok;
        }
    }

    return NdiskFull;
}

static nyfs_status_t nyfs_free_block(nyfs_volume_t *vol, nyfs_blkno_t blkno)
{
    nyfs_status_t status;

    if (!vol) {
        return NinvalidArg;
    }

    status = nyfs_bitmap_put(vol, vol->sb.alloc_meta_start, (u64)blkno, nfalse);
    if (!NSTATUS_IS_ERR(status)) {
        vol->sb.free_blocks++;
        status = nyfs_flush_superblock(vol);
    }
    return status;
}

static nyfs_status_t nyfs_alloc_inode(nyfs_volume_t *vol, nyfs_ino_t *out_ino)
{
    u64 i;
    nyfs_status_t status;

    if (!vol || !out_ino) {
        return NinvalidArg;
    }

    for (i = 0; i < vol->sb.inode_count; i++) {
        nbool taken;

        status = nyfs_bitmap_get(vol, vol->sb.inode_bitmap_start, i, &taken);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        if (!taken) {
            status = nyfs_bitmap_put(vol, vol->sb.inode_bitmap_start, i, ntrue);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            vol->sb.free_inode_count--;
            status = nyfs_flush_superblock(vol);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }

            *out_ino = (nyfs_ino_t)i;
            return Nok;
        }
    }

    return NoutOfMemory;
}

static nyfs_status_t nyfs_free_inode(nyfs_volume_t *vol, nyfs_ino_t ino)
{
    nyfs_status_t status;

    if (!vol || ino == NYFS_INO_NONE || ino >= vol->sb.inode_count) {
        return NinvalidArg;
    }

    status = nyfs_bitmap_put(vol, vol->sb.inode_bitmap_start, (u64)ino, nfalse);
    if (!NSTATUS_IS_ERR(status)) {
        vol->sb.free_inode_count++;
        status = nyfs_flush_superblock(vol);
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* inode I/O                                                           */
/* ------------------------------------------------------------------ */

nyfs_status_t nyfs_read_inode(nyfs_volume_t *vol, nyfs_ino_t ino, nyfs_inode_t *out)
{
    u64 offset;
    nyfs_status_t status;
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

nyfs_status_t nyfs_write_inode(nyfs_volume_t *vol, nyfs_ino_t ino, nyfs_inode_t *in)
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
/* extent 스트림                                                        */
/* ------------------------------------------------------------------ */

static nyfs_status_t nyfs_extent_resolve(nyfs_volume_t *vol, nyfs_blkno_t root, u64 logical_block,
                                          nyfs_blkno_t *out_block)
{
    nyfs_extent_block_header_t hdr;
    nyfs_extent_t extents[NYFS_MAX_EXTENTS_PER_BLOCK];
    nyfs_blkno_t cur;
    u64 base;
    nyfs_status_t status;

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

    return NnotFound;
}

static nyfs_status_t nyfs_extent_total_blocks(nyfs_volume_t *vol, nyfs_blkno_t root, u64 *out_total)
{
    nyfs_extent_block_header_t hdr;
    nyfs_extent_t extents[NYFS_MAX_EXTENTS_PER_BLOCK];
    nyfs_blkno_t cur;
    u64 total;
    nyfs_status_t status;

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

static nyfs_status_t nyfs_free_extent_chain(nyfs_volume_t *vol, nyfs_blkno_t root)
{
    nyfs_blkno_t cur;

    cur = root;

    while (cur != NYFS_BLKNO_NONE) {
        nyfs_extent_block_header_t hdr;
        nyfs_extent_t extents[NYFS_MAX_EXTENTS_PER_BLOCK];
        nyfs_blkno_t next;
        nyfs_status_t status;
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

static nyfs_status_t nyfs_extent_append_one(nyfs_volume_t *vol, nyfs_blkno_t *root)
{
    nyfs_blkno_t new_data_block;
    nyfs_blkno_t last_extent_blk;
    nyfs_extent_block_header_t hdr;
    nyfs_extent_t extents[NYFS_MAX_EXTENTS_PER_BLOCK];
    nyfs_status_t status;

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

static nyfs_status_t nyfs_extent_read_at(nyfs_volume_t *vol, nyfs_blkno_t root, u64 stream_offset,
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
        nyfs_status_t status;

        if (chunk > size - done) {
            chunk = size - done;
        }

        status = nyfs_extent_resolve(vol, root, logical_block, &actual);
        if (status == NnotFound) {
            break;
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

static nyfs_status_t nyfs_extent_write_at(nyfs_volume_t *vol, nyfs_blkno_t *root, u64 stream_offset,
                                           const void *buffer, usize size, usize *out_written)
{
    usize done;
    const u8 *src;
    u64 needed_blocks;
    u64 have_blocks;
    nyfs_status_t status;

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
/* 권한 검사                                                            */
/* ------------------------------------------------------------------ */

static nbool nyfs_ace_id_matches(const nyfs_ace_t *ace, const nyfs_cred_t *cred)
{
    if (ace->id == NYFS_ID_EVERYONE) {
        return ntrue;
    }
    if (ace->flags & NYFS_ACE_FLAG_GROUP) {
        return (nbool)(ace->id == cred->gid);
    }
    return (nbool)(ace->id == cred->uid);
}

static nyfs_status_t nyfs_gather_aces(nyfs_volume_t *vol, const nyfs_inode_t *inode,
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
        nyfs_status_t status;

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

static nyfs_status_t nyfs_check_access(nyfs_volume_t *vol, const nyfs_inode_t *inode,
                                        const nyfs_cred_t *cred, u32 requested_mask)
{
    if (!inode || !cred) {
        return NinvalidArg;
    }

    if (cred->uid == 0) {
        return Nok;
    }

    if (inode->ace_count == 0 && inode->acl_overflow_count == 0) {
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
        nyfs_status_t status;

        status = nyfs_gather_aces(vol, inode, aces, NYFS_MAX_TOTAL_ACES, &count);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        /* NTFS 관례: deny 를 먼저 본다. 요청한 비트 중 하나라도 막으면 즉시 거부.
         * INHERIT_ONLY ACE 는 이 항목 자신에게는 적용되지 않고 자식에게 상속만
         * 되는 것이므로 평가에서 제외한다(커널 nyfs.c 와 동일 - 2026-09-26
         * 커널 쪽에서 빠져있던 걸 고친 버그를 여기도 같이 맞춘다). */
        for (i = 0; i < count; i++) {
            if (aces[i].flags & NYFS_ACE_FLAG_INHERIT_ONLY) {
                continue;
            }
            if (aces[i].type == NYFS_ACE_TYPE_DENY && nyfs_ace_id_matches(&aces[i], cred)) {
                if (aces[i].access_mask & requested_mask) {
                    return Npermission;
                }
            }
        }

        granted = 0;
        for (i = 0; i < count; i++) {
            if (aces[i].flags & NYFS_ACE_FLAG_INHERIT_ONLY) {
                continue;
            }
            if (aces[i].type == NYFS_ACE_TYPE_ALLOW && nyfs_ace_id_matches(&aces[i], cred)) {
                granted |= aces[i].access_mask;
            }
        }

        return ((requested_mask & granted) == requested_mask) ? Nok : Npermission;
    }
}

/*
 * 부모 디렉터리의 ACL 중 상속 플래그(NYFS_ACE_FLAG_INHERIT_FILE/INHERIT_DIR)가
 * 붙은 ACE 를 새로 만드는 자식(child_is_dir 로 파일/디렉터리 구분)에게 복사한다.
 * NTFS 관례대로: 복사된 ACE 에서는 INHERIT_ONLY 를 지운다(자식에게는 실제로
 * 적용돼야 하므로). 원본에 NO_PROPAGATE 가 있었다면 복사본에서 상속 플래그
 * 자체를 지워서 그 자식 아래로는 더 이상 전파되지 않게 한다.
 *
 * 상속받은 ACE 가 하나라도 있으면 자식의 권한 판정은 그때부터 UNIX 모드가
 * 아니라 ACL 로만 이뤄진다(nyfs_check_access 의 "ACL 이 있으면 ACL 만 본다"
 * 규칙과 동일) - 즉 create()/mkdir() 에 넘긴 mode 인자는 ACL 이 상속되는
 * 순간부터 사실상 무시된다. (커널 nyfs_inherit_acl() 과 1:1 대응)
 */
static nyfs_status_t nyfs_inherit_acl(nyfs_volume_t *vol, const nyfs_inode_t *parent,
                                       nyfs_inode_t *child, nbool child_is_dir)
{
    nyfs_ace_t parent_aces[NYFS_MAX_TOTAL_ACES];
    nyfs_ace_t inherited[NYFS_MAX_TOTAL_ACES];
    u32 parent_count;
    u32 inherited_count;
    u32 need_flag;
    u32 i;
    nyfs_status_t status;

    need_flag = child_is_dir ? NYFS_ACE_FLAG_INHERIT_DIR : NYFS_ACE_FLAG_INHERIT_FILE;

    status = nyfs_gather_aces(vol, parent, parent_aces, NYFS_MAX_TOTAL_ACES, &parent_count);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    inherited_count = 0;
    for (i = 0; i < parent_count; i++) {
        nyfs_ace_t ace;

        if (!(parent_aces[i].flags & need_flag)) {
            continue;
        }
        if (inherited_count >= NYFS_MAX_TOTAL_ACES) {
            break;   /* 상속받을 ACE 가 너무 많다 - 나머지는 포기한다(매우 드문 경우) */
        }

        ace = parent_aces[i];
        ace.flags = (u8)(ace.flags & (u8)~NYFS_ACE_FLAG_INHERIT_ONLY);
        if (parent_aces[i].flags & NYFS_ACE_FLAG_NO_PROPAGATE) {
            ace.flags = (u8)(ace.flags & (u8)~(NYFS_ACE_FLAG_INHERIT_FILE
                                              | NYFS_ACE_FLAG_INHERIT_DIR
                                              | NYFS_ACE_FLAG_NO_PROPAGATE));
        }

        inherited[inherited_count] = ace;
        inherited_count++;
    }

    if (inherited_count == 0) {
        return Nok;
    }

    if (inherited_count <= NYFS_ACL_INLINE_COUNT) {
        for (i = 0; i < inherited_count; i++) {
            child->ace[i] = inherited[i];
        }
        child->ace_count = (u16)inherited_count;
        child->acl_overflow_block = NYFS_BLKNO_NONE;
        child->acl_overflow_count = 0;
        return Nok;
    }

    {
        nyfs_blkno_t overflow_block;
        u32 overflow_count = inherited_count - NYFS_ACL_INLINE_COUNT;

        if ((u64)overflow_count * sizeof(nyfs_ace_t) > vol->block_size) {
            /* 블록 하나에 다 안 들어간다 - 조용히 잘라내면 deny ACE 가 빠질 수
             * 있어 안전하지 않으므로 차라리 실패시킨다(블록 크기가 아주 작고
             * 부모에 상속형 ACE 가 매우 많을 때만 벌어지는 드문 경우다). */
            return Noverflow;
        }

        for (i = 0; i < NYFS_ACL_INLINE_COUNT; i++) {
            child->ace[i] = inherited[i];
        }
        child->ace_count = NYFS_ACL_INLINE_COUNT;

        status = nyfs_alloc_block(vol, &overflow_block);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        status = nyfs_raw_write(vol, (u64)overflow_block * vol->block_size,
                                 &inherited[NYFS_ACL_INLINE_COUNT],
                                 (usize)overflow_count * sizeof(nyfs_ace_t));
        if (NSTATUS_IS_ERR(status)) {
            (void)nyfs_free_block(vol, overflow_block);
            return status;
        }

        child->acl_overflow_block = overflow_block;
        child->acl_overflow_count = overflow_count;
        return Nok;
    }
}

/* ------------------------------------------------------------------ */
/* 디렉터리 해시테이블                                                  */
/* ------------------------------------------------------------------ */

static nyfs_status_t nyfs_dirhash_create(nyfs_volume_t *vol, nyfs_inode_t *dir)
{
    nyfs_dirhash_header_t hdr;
    u64 buckets[NYFS_DIRHASH_DEFAULT_BUCKETS];
    nyfs_blkno_t root;
    usize written;
    nyfs_status_t status;
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

static nyfs_status_t nyfs_dirhash_lookup(nyfs_volume_t *vol, const nyfs_inode_t *dir, const char *name,
                                          nyfs_ino_t *out_ino, u32 *out_type)
{
    nyfs_dirhash_header_t hdr;
    usize namelen;
    u32 hash;
    u64 bucket_off;
    u64 cur;
    usize got;
    nyfs_status_t status;

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

static nyfs_status_t nyfs_dirhash_insert(nyfs_volume_t *vol, nyfs_inode_t *dir, const char *name,
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
    nyfs_status_t status;

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
    record_size = ((usize)sizeof(de) + namelen + 7U) & ~(usize)7U;

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

static nyfs_status_t nyfs_dirhash_remove(nyfs_volume_t *vol, nyfs_inode_t *dir, const char *name)
{
    nyfs_dirhash_header_t hdr;
    usize namelen;
    u32 hash;
    u64 bucket_slot_offset;
    u64 prev_offset;
    u64 cur;
    usize got;
    usize written;
    nyfs_blkno_t root;
    nyfs_status_t status;

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
/* 경로 조회 (호스트 전용 - 커널은 VFS dentry 트리가 이 역할을 한다)         */
/* ------------------------------------------------------------------ */

/* path 에서 다음 '/' 전까지 한 구성요소를 떼어 comp 에 채우고(더 없으면
 * comp[0]='\0'), path 안에서 그 다음 구성요소가 시작할 위치를 반환한다
 * (절대 nNULL 을 반환하지 않는다 - 더 없으면 '\0' 을 가리키는 포인터를
 * 돌려주므로 호출자는 comp[0] 하나만 보고 종료를 판단하면 된다). 연속된
 * '/' 는 건너뛴다. 이름이 NYFS_NAME_MAX 보다 길면 comp 를 잘라 넣는다 -
 * 그러면 이후 dirhash_lookup 이 그 이름을 못 찾아 자연스럽게 NnotFound 로
 * 처리된다(별도 에러 분기를 만들 필요가 없다). */
static const char *nyfs_next_component(const char *path, char *comp, usize comp_max)
{
    usize n;

    while (*path == '/') {
        path++;
    }
    if (*path == '\0') {
        comp[0] = '\0';
        return path;
    }

    n = 0;
    while (path[n] != '\0' && path[n] != '/' && n + 1U < comp_max) {
        n++;
    }
    memcpy(comp, path, n);
    comp[n] = '\0';

    /* comp_max 를 넘는 나머지 글자도(잘렸더라도) 다음 '/' 까지는 그냥
     * 건너뛴다 - path 포인터가 그 이름 중간에 멈추지 않게 하기 위함 */
    while (path[n] != '\0' && path[n] != '/') {
        n++;
    }
    path += n;
    while (*path == '/') {
        path++;
    }
    return path;
}

nyfs_status_t nyfs_path_lookup(nyfs_volume_t *vol, const char *path, const nyfs_cred_t *cred,
                                nyfs_ino_t *out_ino)
{
    nyfs_ino_t cur_ino;
    const char *p;
    char comp[NYFS_NAME_MAX + 1U];

    if (!vol || !path || !cred || !out_ino) {
        return NinvalidArg;
    }

    cur_ino = NYFS_ROOT_INO;
    p = path;

    for (;;) {
        nyfs_inode_t cur_inode;
        nyfs_status_t status;
        nyfs_ino_t child_ino;
        u32 child_type;

        p = nyfs_next_component(p, comp, sizeof(comp));
        if (comp[0] == '\0') {
            break;   /* 더 이상 구성요소가 없다 - cur_ino 가 최종 결과 */
        }

        status = nyfs_read_inode(vol, cur_ino, &cur_inode);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (cur_inode.type != NYFS_INO_TYPE_DIR) {
            return Nunsupported;
        }

        status = nyfs_check_access(vol, &cur_inode, cred, NYFS_ACE_EXECUTE);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        status = nyfs_dirhash_lookup(vol, &cur_inode, comp, &child_ino, &child_type);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        cur_ino = child_ino;
    }

    *out_ino = cur_ino;
    return Nok;
}

nyfs_status_t nyfs_path_lookup_parent(nyfs_volume_t *vol, const char *path, const nyfs_cred_t *cred,
                                       nyfs_ino_t *out_parent_ino, char *out_name, usize name_max)
{
    const char *last_slash;
    usize dirlen;
    char dirbuf[4096];
    const char *name_start;
    usize namelen;
    nyfs_status_t status;

    if (!vol || !path || !cred || !out_parent_ino || !out_name) {
        return NinvalidArg;
    }

    /* 앞의 '/' 들을 건너뛴다 */
    while (*path == '/') {
        path++;
    }
    if (*path == '\0') {
        return NinvalidArg;   /* 루트 자체는 부모가 없다 */
    }

    last_slash = nNULL;
    {
        const char *q = path;
        while (*q != '\0') {
            if (*q == '/') {
                last_slash = q;
            }
            q++;
        }
    }

    if (!last_slash) {
        name_start = path;
        dirlen = 0;
    } else {
        name_start = last_slash + 1;
        dirlen = (usize)(last_slash - path);
        if (dirlen >= sizeof(dirbuf)) {
            return NnameTooLong;
        }
    }

    namelen = strlen(name_start);
    if (namelen == 0 || namelen > NYFS_NAME_MAX) {
        return NinvalidArg;
    }
    if (namelen >= name_max) {
        return NnameTooLong;
    }
    memcpy(out_name, name_start, namelen);
    out_name[namelen] = '\0';

    if (dirlen == 0) {
        *out_parent_ino = NYFS_ROOT_INO;
        /* 루트 자체에 대한 권한 확인은 nyfs_path_lookup 이 컴포넌트를 도는
         * 동안 하지만, 부모가 루트 자체인 경우엔 도는 컴포넌트가 없으므로
         * 여기서 별도로 EXECUTE 를 확인해야 한다. */
        {
            nyfs_inode_t root_inode;

            status = nyfs_read_inode(vol, NYFS_ROOT_INO, &root_inode);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
            status = nyfs_check_access(vol, &root_inode, cred, NYFS_ACE_EXECUTE);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
        }
        return Nok;
    }

    memcpy(dirbuf, path, dirlen);
    dirbuf[dirlen] = '\0';

    return nyfs_path_lookup(vol, dirbuf, cred, out_parent_ino);
}

/* ------------------------------------------------------------------ */
/* 상위 연산                                                            */
/* ------------------------------------------------------------------ */

nyfs_status_t nyfs_op_create(nyfs_volume_t *vol, nyfs_ino_t parent_ino, const char *name,
                              u16 unix_mode, const nyfs_cred_t *cred, nyfs_ino_t *out_ino)
{
    nyfs_inode_t dir_inode;
    nyfs_inode_t new_inode;
    nyfs_ino_t new_ino;
    nyfs_ino_t existing_ino;
    u32 existing_type;
    nyfs_status_t status;

    if (!vol || !name || !cred || !out_ino) {
        return NinvalidArg;
    }
    if (!vol->writable) {
        return NreadOnly;
    }

    status = nyfs_read_inode(vol, parent_ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    status = nyfs_check_access(vol, &dir_inode, cred, NYFS_ACE_WRITE_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_dirhash_lookup(vol, &dir_inode, name, &existing_ino, &existing_type);
    if (!NSTATUS_IS_ERR(status)) {
        return NalreadyExists;
    }
    if (status != NnotFound) {
        return status;
    }

    status = nyfs_alloc_inode(vol, &new_ino);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.ino = new_ino;
    new_inode.type = NYFS_INO_TYPE_FILE;
    new_inode.unix_mode = (u16)(unix_mode & 0x0FFFU);
    new_inode.uid = cred->uid;
    new_inode.gid = cred->gid;
    new_inode.link_count = 1;

    status = nyfs_inherit_acl(vol, &dir_inode, &new_inode, nfalse);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(vol, new_ino);
        return status;
    }

    status = nyfs_write_inode(vol, new_ino, &new_inode);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(vol, new_ino);
        return status;
    }

    status = nyfs_dirhash_insert(vol, &dir_inode, name, new_ino, NYFS_DIRENT_TYPE_FILE);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(vol, new_ino);
        return status;
    }

    status = nyfs_write_inode(vol, parent_ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    *out_ino = new_ino;
    return Nok;
}

nyfs_status_t nyfs_op_mkdir(nyfs_volume_t *vol, nyfs_ino_t parent_ino, const char *name,
                             u16 unix_mode, const nyfs_cred_t *cred, nyfs_ino_t *out_ino)
{
    nyfs_inode_t dir_inode;
    nyfs_inode_t new_inode;
    nyfs_ino_t new_ino;
    nyfs_ino_t existing_ino;
    u32 existing_type;
    nyfs_status_t status;

    if (!vol || !name || !cred || !out_ino) {
        return NinvalidArg;
    }
    if (!vol->writable) {
        return NreadOnly;
    }

    status = nyfs_read_inode(vol, parent_ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    status = nyfs_check_access(vol, &dir_inode, cred, NYFS_ACE_WRITE_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_dirhash_lookup(vol, &dir_inode, name, &existing_ino, &existing_type);
    if (!NSTATUS_IS_ERR(status)) {
        return NalreadyExists;
    }
    if (status != NnotFound) {
        return status;
    }

    status = nyfs_alloc_inode(vol, &new_ino);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.ino = new_ino;
    new_inode.type = NYFS_INO_TYPE_DIR;
    new_inode.unix_mode = (u16)(unix_mode & 0x0FFFU);
    new_inode.uid = cred->uid;
    new_inode.gid = cred->gid;
    new_inode.link_count = 1;

    status = nyfs_inherit_acl(vol, &dir_inode, &new_inode, ntrue);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(vol, new_ino);
        return status;
    }

    status = nyfs_dirhash_create(vol, &new_inode);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(vol, new_ino);
        return status;
    }

    status = nyfs_write_inode(vol, new_ino, &new_inode);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(vol, new_ino);
        return status;
    }

    status = nyfs_dirhash_insert(vol, &dir_inode, name, new_ino, NYFS_DIRENT_TYPE_DIR);
    if (NSTATUS_IS_ERR(status)) {
        (void)nyfs_free_inode(vol, new_ino);
        return status;
    }

    status = nyfs_write_inode(vol, parent_ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    *out_ino = new_ino;
    return Nok;
}

nyfs_status_t nyfs_op_remove(nyfs_volume_t *vol, nyfs_ino_t parent_ino, const char *name,
                              const nyfs_cred_t *cred, nbool expect_dir)
{
    nyfs_inode_t dir_inode;
    nyfs_inode_t target_inode;
    nyfs_ino_t target_ino;
    u32 target_type;
    nyfs_status_t status;

    if (!vol || !name || !cred) {
        return NinvalidArg;
    }
    if (!vol->writable) {
        return NreadOnly;
    }

    status = nyfs_read_inode(vol, parent_ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    status = nyfs_check_access(vol, &dir_inode, cred, NYFS_ACE_DELETE_CHILD);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_dirhash_lookup(vol, &dir_inode, name, &target_ino, &target_type);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_read_inode(vol, target_ino, &target_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (expect_dir && target_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;   /* rmdir 인데 디렉터리가 아님 - POSIX ENOTDIR 에 대응 */
    }
    if (!expect_dir && target_inode.type == NYFS_INO_TYPE_DIR) {
        return Nunsupported;   /* unlink 인데 디렉터리임 - POSIX EISDIR 에 대응 */
    }

    if (target_inode.type == NYFS_INO_TYPE_DIR && target_inode.dir_entry_count > 0) {
        return Nbusy;
    }

    status = nyfs_dirhash_remove(vol, &dir_inode, name);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_write_inode(vol, parent_ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (target_inode.link_count > 0) {
        target_inode.link_count--;
    }

    if (target_inode.link_count == 0) {
        if (target_inode.type == NYFS_INO_TYPE_DIR) {
            (void)nyfs_free_extent_chain(vol, target_inode.dir_hash_root);
        }
        (void)nyfs_free_extent_chain(vol, target_inode.data_extent_root);
        (void)nyfs_free_inode(vol, target_ino);
    } else {
        (void)nyfs_write_inode(vol, target_ino, &target_inode);
    }

    return Nok;
}

nyfs_status_t nyfs_op_read(nyfs_volume_t *vol, nyfs_ino_t ino, const nyfs_cred_t *cred,
                            u64 offset, void *buf, usize size, usize *out_read)
{
    nyfs_inode_t inode;
    usize avail;
    usize to_read;
    nyfs_status_t status;

    if (!vol || !cred || !buf || !out_read) {
        return NinvalidArg;
    }

    status = nyfs_read_inode(vol, ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_check_access(vol, &inode, cred, NYFS_ACE_READ_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (offset >= inode.size) {
        *out_read = 0;
        return Nok;
    }

    avail = (usize)(inode.size - offset);
    to_read = (size < avail) ? size : avail;

    return nyfs_extent_read_at(vol, inode.data_extent_root, offset, buf, to_read, out_read);
}

nyfs_status_t nyfs_op_write(nyfs_volume_t *vol, nyfs_ino_t ino, const nyfs_cred_t *cred,
                             u64 offset, const void *buf, usize size, usize *out_written)
{
    nyfs_inode_t inode;
    nyfs_blkno_t root;
    usize written;
    nyfs_status_t status;

    if (!vol || !cred || !buf || !out_written) {
        return NinvalidArg;
    }
    if (!vol->writable) {
        return NreadOnly;
    }

    status = nyfs_read_inode(vol, ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_check_access(vol, &inode, cred, NYFS_ACE_WRITE_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    root = inode.data_extent_root;
    status = nyfs_extent_write_at(vol, &root, offset, buf, size, &written);
    inode.data_extent_root = root;
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (offset + written > inode.size) {
        inode.size = offset + written;
    }

    status = nyfs_write_inode(vol, ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    *out_written = written;
    return Nok;
}

nyfs_status_t nyfs_op_readdir_at(nyfs_volume_t *vol, nyfs_ino_t dir_ino, const nyfs_cred_t *cred,
                                  u64 index, nyfs_dirent_view_t *out)
{
    nyfs_inode_t dir_inode;
    nyfs_dirhash_header_t hdr;
    usize got;
    u64 seen;
    u32 b;
    nyfs_status_t status;

    if (!vol || !cred || !out) {
        return NinvalidArg;
    }

    status = nyfs_read_inode(vol, dir_ino, &dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    status = nyfs_check_access(vol, &dir_inode, cred, NYFS_ACE_READ_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (dir_inode.dir_hash_root == NYFS_BLKNO_NONE) {
        return NnotFound;
    }

    status = nyfs_extent_read_at(vol, dir_inode.dir_hash_root, 0, &hdr, sizeof(hdr), &got);
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

        status = nyfs_extent_read_at(vol, dir_inode.dir_hash_root,
                                      (u64)sizeof(hdr) + (u64)b * sizeof(u64),
                                      &bucket_off, sizeof(bucket_off), &got);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }

        cur = bucket_off;
        while (cur != 0) {
            nyfs_dirent_t de;

            status = nyfs_extent_read_at(vol, dir_inode.dir_hash_root, cur, &de, sizeof(de), &got);
            if (NSTATUS_IS_ERR(status)) {
                return status;
            }
            if (got != sizeof(de)) {
                return Ncorrupted;
            }

            if (seen == index) {
                usize copy_len = (usize)de.name_len;

                if (copy_len > NYFS_NAME_MAX) {
                    copy_len = NYFS_NAME_MAX;
                }

                status = nyfs_extent_read_at(vol, dir_inode.dir_hash_root, cur + sizeof(de),
                                              out->name, copy_len, &got);
                if (NSTATUS_IS_ERR(status)) {
                    return status;
                }
                out->name[copy_len] = '\0';
                out->ino = de.ino;
                out->type = de.file_type;
                return Nok;
            }

            seen++;
            cur = de.next_offset;
        }
    }

    return NnotFound;
}

/* 커널 nyfs_rename() 과 1:1 대응(커널은 vnode/dentry 를 거치지만 여기서는
 * 이 라이브러리가 볼륨 하나만 다루므로 부모 inode 번호 두 개를 직접
 * 받는다 - "다른 볼륨 사이의 rename" 제약은 호출자가 볼륨을 하나만 열어
 * 쓰는 구조상 자동으로 성립한다). */
nyfs_status_t nyfs_op_rename(nyfs_volume_t *vol, nyfs_ino_t old_parent_ino, const char *old_name,
                              nyfs_ino_t new_parent_ino, const char *new_name, const nyfs_cred_t *cred)
{
    nyfs_inode_t old_dir_inode;
    nyfs_inode_t new_dir_inode_storage;
    nyfs_inode_t *new_dir_inode;
    nyfs_ino_t moved_ino;
    u32 moved_type;
    nyfs_ino_t existing_ino;
    u32 existing_type;
    nbool same_dir;
    nyfs_status_t status;

    if (!vol || !old_name || !new_name || !cred) {
        return NinvalidArg;
    }
    if (!vol->writable) {
        return NreadOnly;
    }

    same_dir = (nbool)(old_parent_ino == new_parent_ino);

    status = nyfs_read_inode(vol, old_parent_ino, &old_dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (old_dir_inode.type != NYFS_INO_TYPE_DIR) {
        return Nunsupported;
    }

    if (same_dir) {
        new_dir_inode = &old_dir_inode;   /* 같은 디렉터리면 사본 하나만 갱신한다 */
    } else {
        status = nyfs_read_inode(vol, new_parent_ino, &new_dir_inode_storage);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
        if (new_dir_inode_storage.type != NYFS_INO_TYPE_DIR) {
            return Nunsupported;
        }
        new_dir_inode = &new_dir_inode_storage;
    }

    status = nyfs_check_access(vol, &old_dir_inode, cred, NYFS_ACE_DELETE_CHILD);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    status = nyfs_check_access(vol, new_dir_inode, cred, NYFS_ACE_WRITE_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_dirhash_lookup(vol, &old_dir_inode, old_name, &moved_ino, &moved_type);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (same_dir && strcmp(old_name, new_name) == 0) {
        return Nok;   /* 이름이 같으면 할 일이 없다 */
    }

    /* 디렉터리를 자기 자신의 새 부모로 옮기는(즉 자기 자신 위로 옮기는)
     * 가장 자명한 순환만 여기서 막는다. 더 깊은 순환(자기 하위 디렉터리
     * 밑으로 옮기기)은 nyfs inode 에 부모 포인터가 없어 이 계층만으로는
     * 잡지 못한다 - 커널과 동일한 알려진 한계다. */
    if (moved_type == NYFS_DIRENT_TYPE_DIR && moved_ino == new_parent_ino) {
        return NinvalidArg;
    }

    status = nyfs_dirhash_lookup(vol, new_dir_inode, new_name, &existing_ino, &existing_type);
    if (!NSTATUS_IS_ERR(status)) {
        return NalreadyExists;   /* v1: 덮어쓰기 rename 은 지원하지 않는다 */
    }
    if (status != NnotFound) {
        return status;
    }

    status = nyfs_dirhash_remove(vol, &old_dir_inode, old_name);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_dirhash_insert(vol, new_dir_inode, new_name, moved_ino, moved_type);
    if (NSTATUS_IS_ERR(status)) {
        /* 저널이 없어 완전한 롤백은 못 한다 - 최소한 원래 이름으로 복구를
         * 시도한다(실패해도 무시한다 - 할 수 있는 최선을 다한 것이다) */
        (void)nyfs_dirhash_insert(vol, &old_dir_inode, old_name, moved_ino, moved_type);
        (void)nyfs_write_inode(vol, old_parent_ino, &old_dir_inode);
        return status;
    }

    status = nyfs_write_inode(vol, old_parent_ino, &old_dir_inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    if (!same_dir) {
        status = nyfs_write_inode(vol, new_parent_ino, new_dir_inode);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
    }

    return Nok;
}

nyfs_status_t nyfs_op_setattr(nyfs_volume_t *vol, nyfs_ino_t ino, const nyfs_cred_t *cred,
                               int set_mode, u16 unix_mode,
                               int set_owner, u32 uid, u32 gid,
                               u64 mtime_ns, u64 atime_ns)
{
    nyfs_inode_t inode;
    nyfs_status_t status;

    if (!vol || !cred) {
        return NinvalidArg;
    }
    if (!vol->writable) {
        return NreadOnly;
    }

    status = nyfs_read_inode(vol, ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    /* UNIX 관례: root 가 아니면 자기 파일의 소유주/모드만 바꿀 수 있다.
     * (커널에 아직 chmod/chown 시스템 콜이 없어 nyfs_check_access() 에 대응
     * 마스크가 없으므로, 여기서는 표준 POSIX 규칙을 직접 적용한다) */
    if (cred->uid != 0 && cred->uid != inode.uid) {
        return Npermission;
    }

    if (set_mode) {
        inode.unix_mode = (u16)(unix_mode & 0x0FFFU);
    }
    if (set_owner) {
        inode.uid = uid;
        inode.gid = gid;
    }
    if (mtime_ns != NYFS_TIME_KEEP) {
        inode.mtime = mtime_ns;
    }
    if (atime_ns != NYFS_TIME_KEEP) {
        inode.atime = atime_ns;
    }

    return nyfs_write_inode(vol, ino, &inode);
}

nyfs_status_t nyfs_op_truncate(nyfs_volume_t *vol, nyfs_ino_t ino, const nyfs_cred_t *cred, u64 new_size)
{
    nyfs_inode_t inode;
    nyfs_status_t status;

    if (!vol || !cred) {
        return NinvalidArg;
    }
    if (!vol->writable) {
        return NreadOnly;
    }

    status = nyfs_read_inode(vol, ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    status = nyfs_check_access(vol, &inode, cred, NYFS_ACE_WRITE_DATA);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (new_size > inode.size) {
        /* 헤더 주석에서 설명한 대로, 구멍(hole)을 만드는 확장 truncate 는
         * "읽으면 0 이 나와야 한다"는 보장을 extent 할당 없이 지킬 수 없어
         * 지금은 명시적으로 거부한다(자동으로 데이터를 채우는 대신 오류를
         * 돌려주는 편이 "조용히 틀린 내용을 주는 것"보다 안전하다는 판단 -
         * 보안/안정성 우선 원칙과 일치). */
        return Nunsupported;
    }

    inode.size = new_size;
    return nyfs_write_inode(vol, ino, &inode);
}

/* ------------------------------------------------------------------ */
/* mount / mkfs                                                        */
/* ------------------------------------------------------------------ */

nyfs_status_t nyfs_volume_open(nyfs_volume_t *vol, int fd, nbool writable)
{
    nyfs_status_t status;
    u32 stored_checksum;
    u32 computed_checksum;

    if (!vol || fd < 0) {
        return NinvalidArg;
    }

    memset(vol, 0, sizeof(*vol));
    vol->fd = fd;
    vol->writable = writable;
    vol->block_size = 512U; /* 슈퍼블록을 읽을 최소 단위 - 실제 값은 아래에서 갱신 */

    status = nyfs_raw_read(vol, 0, &vol->sb, sizeof(vol->sb));
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

    if (vol->sb.magic != NYFS_MAGIC) {
        return NbadFilesystem;
    }
    if (vol->sb.version != NYFS_VERSION) {
        return NbadFilesystem;
    }
    if (vol->sb.superblock_size != sizeof(nyfs_superblock_t)) {
        return NbadFilesystem;
    }
    if (vol->sb.block_size_shift < NYFS_BLOCK_SIZE_SHIFT_MIN
        || vol->sb.block_size_shift > NYFS_BLOCK_SIZE_SHIFT_MAX) {
        return NbadFilesystem;
    }

    stored_checksum = vol->sb.checksum;
    vol->sb.checksum = 0;
    computed_checksum = nyfs_crc32(&vol->sb, sizeof(vol->sb));
    vol->sb.checksum = stored_checksum;
    if (computed_checksum != stored_checksum) {
        return Ncorrupted;
    }

    vol->block_size = 1U << vol->sb.block_size_shift;

    vol->sb.mount_count++;
    if (vol->writable) {
        status = nyfs_flush_superblock(vol);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
    }

    return Nok;
}

void nyfs_volume_close(nyfs_volume_t *vol)
{
    if (!vol) {
        return;
    }
    vol->fd = -1;
    vol->writable = nfalse;
}

nyfs_status_t nyfs_format(int fd, const nyfs_mkfs_params_t *params)
{
    nyfs_volume_t vol;
    nyfs_superblock_t sb;
    u64 inode_count;
    u64 inode_bitmap_bytes, inode_bitmap_blocks;
    u64 block_bitmap_bytes, block_bitmap_blocks;
    u64 inode_table_bytes, inode_table_blocks;
    u64 first_data_block;
    u64 i;
    nyfs_inode_t root_inode;
    nyfs_status_t status;
    struct stat st;

    if (fd < 0 || !params) {
        return NinvalidArg;
    }
    if (params->block_size_shift < NYFS_BLOCK_SIZE_SHIFT_MIN
        || params->block_size_shift > NYFS_BLOCK_SIZE_SHIFT_MAX) {
        return NinvalidArg;
    }
    if (params->total_blocks < 64U) {
        return NinvalidArg;
    }

    memset(&vol, 0, sizeof(vol));
    vol.fd = fd;
    vol.writable = ntrue;
    vol.block_size = 1U << params->block_size_shift;

    /* 정규 파일이면 볼륨 전체 크기로 미리 늘려 둔다(스파스 파일이라 실제
     * 디스크 사용량은 실제로 써진 블록만큼만 늘어난다). 블록 장치는
     * ftruncate 대상이 아니므로 fstat 으로 종류를 확인한다. */
    if (fstat(fd, &st) != 0) {
        return Nio;
    }
    if (S_ISREG(st.st_mode)) {
        off_t want = (off_t)(params->total_blocks * (u64)vol.block_size);

        if (ftruncate(fd, want) != 0) {
            return Nio;
        }
    }

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

    if (params->label) {
        usize labellen = strlen(params->label);

        if (labellen >= sizeof(sb.label)) {
            labellen = sizeof(sb.label) - 1U;
        }
        memcpy(sb.label, params->label, labellen);
    }

    first_data_block = sb.inode_table_start + inode_table_blocks;
    if (first_data_block >= params->total_blocks) {
        return NinvalidArg;
    }

    sb.free_blocks = params->total_blocks - first_data_block;
    sb.free_inode_count = inode_count;
    sb.journal_version = 0;
    sb.journal_start = 0;
    sb.journal_blocks = 0;

    vol.sb = sb;

    status = nyfs_flush_superblock(&vol);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

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

    for (i = 0; i < first_data_block; i++) {
        status = nyfs_bitmap_put(&vol, sb.alloc_meta_start, i, ntrue);
        if (NSTATUS_IS_ERR(status)) {
            return status;
        }
    }

    status = nyfs_bitmap_put(&vol, sb.inode_bitmap_start, 0, ntrue);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }
    vol.sb.free_inode_count--;
    status = nyfs_flush_superblock(&vol);
    if (NSTATUS_IS_ERR(status)) {
        return status;
    }

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

    return nyfs_flush_superblock(&vol);
}
