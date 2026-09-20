/*
 * ramdisk.c - 램디스크
 *
 * [수정 이력 요약]  (모두 보안/안정성 관련)
 *  - 모든 범위 검사가 "start + size > limit" 형태였다. start/size 가 크면 덧셈이 오버플로하여
 *    검사를 통과하고, 결과적으로 램디스크 밖(임의 커널 메모리)을 읽고 쓸 수 있었다.
 *    -> 뺄셈 형태(size > limit - start)로 바꿔 오버플로를 원천 차단.
 *  - info 가 NULL 이면 역참조로 커널 크래시 -> NULL 검사 추가.
 *  - 버퍼 검사에서 포인터를 (usize) 로 캐스팅했는데 usize 가 include 순서에 따라 32비트가
 *    되어 컴파일 경고가 나던 문제는 types.h 수정으로 해결.
 *  - 불필요한 interrupt.h include 제거, C89 호환.
 */

#include "ramdisk.h"

#include "nyxis.h"
#include "memory.h"

static ramdisk_t g_ramdisks[RAMDISK_MAX];

static ramdisk_t *ramdisk_find(u32 diskno)
{
    u32 i;

    for (i = 0; i < RAMDISK_MAX; i++) {
        if (g_ramdisks[i].is_used &&
            g_ramdisks[i].diskno == diskno) {
            return &g_ramdisks[i];
        }
    }

    return nNULL;
}

/* [start, start+size) 가 [0, limit) 안에 있는지 (오버플로 안전) */
static bool range_within(u64 start, u64 size, u64 limit)
{
    if (start > limit)
        return false;
    return (size <= limit - start) ? true : false;
}

Nstatus ramdisk_init(
    u32 diskno,
    usize ramdisk_start,
    usize ramdisk_size,
    NTBLI *info
) {
    u32 i;

    if (!info)
        return NinvalidArg;

    if (ramdisk_size == 0 || ramdisk_start == 0)
        return NinvalidArg;

    if (!range_within(ramdisk_start, ramdisk_size, info->memory_size))
        return NoutOfMemory;

    if (ramdisk_find(diskno))
        return NalreadyExists;

    for (i = 0; i < RAMDISK_MAX; i++) {
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
    void *buffer,
    usize size,
    NTBLI *info
) {
    ramdisk_t *rd = ramdisk_find(diskno);

    if (!rd)
        return NnotFound;

    if (!buffer || size == 0 || !info)
        return NinvalidArg;

    if (!range_within(offset, size, rd->size))
        return Noverflow;

    if (!range_within((u64)(usize)buffer, size, info->memory_size))
        return NinvalidPointer;

    memcpy(buffer, (void *)(usize)(rd->offset + offset), size);

    return Nok;
}

Nstatus ramdisk_write(
    u32 diskno,
    usize offset,
    const void *buffer,
    usize size,
    NTBLI *info
) {
    ramdisk_t *rd = ramdisk_find(diskno);

    if (!rd)
        return NnotFound;

    if (!buffer || size == 0 || !info)
        return NinvalidArg;

    if (!range_within(offset, size, rd->size))
        return Noverflow;

    if (!range_within((u64)(usize)buffer, size, info->memory_size))
        return NinvalidPointer;

    memcpy((void *)(usize)(rd->offset + offset), buffer, size);

    return Nok;
}

Nstatus ramdisk_format(u32 diskno, NTBLI *info)
{
    ramdisk_t *rd;

    (void)info;
    rd = ramdisk_find(diskno);

    if (!rd)
        return NnotFound;

    memset((void *)(usize)rd->offset, 0, rd->size);

    return Nok;
}

Nstatus ramdisk_deinit(u32 diskno)
{
    ramdisk_t *rd = ramdisk_find(diskno);

    if (!rd)
        return NnotFound;

    rd->is_used = false;

    rd->diskno = 0;
    rd->offset = 0;
    rd->size   = 0;

    return Nok;
}
