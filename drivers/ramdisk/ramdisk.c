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

    /*
     * ramdisk_start 는 "커널 가상 주소" 이다 (higher-half 커널). 물리 RAM 범위 검증은 부트 정보를 받는
     * 쪽(kernel.c) 에서 물리 주소로 이미 끝냈으므로 여기서는 산술 오버플로만 막는다.
     */
    (void)info;

    if (ramdisk_size == 0 || ramdisk_start == 0)
        return NinvalidArg;

    if (!range_within(ramdisk_start, ramdisk_size, 0xFFFFFFFFFFFFFFFFUL))
        return NinvalidArg;

    if (ramdisk_find(diskno))
        return NalreadyExists;

    for (i = 0; i < RAMDISK_MAX; i++) {
        if (!g_ramdisks[i].is_used) {
            memset(&g_ramdisks[i], 0, sizeof(g_ramdisks[i]));
            g_ramdisks[i].diskno  = diskno;
            g_ramdisks[i].offset  = ramdisk_start;
            g_ramdisks[i].size    = ramdisk_size;
            g_ramdisks[i].is_used = true;

            return Nok;
        }
    }

    return NtooManyFileSystem;
}

Nstatus ramdisk_init_sparse(u32 diskno, usize ramdisk_size, void **pages,
                            ramdisk_page_alloc_fn page_alloc, ramdisk_page_free_fn page_free)
{
    u32 i;

    if (ramdisk_size == 0 || (ramdisk_size % RAMDISK_PAGE_SIZE) != 0 || !pages || !page_alloc || !page_free)
        return NinvalidArg;
    if (ramdisk_find(diskno))
        return NalreadyExists;

    for (i = 0; i < RAMDISK_MAX; i++) {
        if (!g_ramdisks[i].is_used) {
            memset(&g_ramdisks[i], 0, sizeof(g_ramdisks[i]));
            g_ramdisks[i].diskno     = diskno;
            g_ramdisks[i].size       = ramdisk_size;
            g_ramdisks[i].pages      = pages;
            g_ramdisks[i].page_alloc = page_alloc;
            g_ramdisks[i].page_free  = page_free;
            g_ramdisks[i].is_used    = true;
            return Nok;
        }
    }
    return NtooManyFileSystem;
}

static bool all_zero(const u8 *p, usize n)
{
    usize i;

    for (i = 0; i < n; i++) {
        if (p[i] != 0)
            return false;
    }
    return true;
}

/* 희소 램디스크 읽기/쓰기: 페이지 경계로 나눠 처리한다 (범위 검사는 호출자가 끝냈다) */
static void sparse_read(ramdisk_t *rd, u64 offset, u8 *dst, usize size)
{
    while (size > 0) {
        u64 idx = offset / RAMDISK_PAGE_SIZE;
        usize in = (usize)(offset % RAMDISK_PAGE_SIZE);
        usize chunk = (usize)RAMDISK_PAGE_SIZE - in;

        if (chunk > size)
            chunk = size;
        if (rd->pages[idx])
            memcpy(dst, (const u8 *)rd->pages[idx] + in, chunk);
        else
            memset(dst, 0, chunk);
        offset += chunk;
        dst += chunk;
        size -= chunk;
    }
}

static Nstatus sparse_write(ramdisk_t *rd, u64 offset, const u8 *src, usize size)
{
    while (size > 0) {
        u64 idx = offset / RAMDISK_PAGE_SIZE;
        usize in = (usize)(offset % RAMDISK_PAGE_SIZE);
        usize chunk = (usize)RAMDISK_PAGE_SIZE - in;

        if (chunk > size)
            chunk = size;
        if (!rd->pages[idx]) {
            if (!all_zero(src, chunk)) {
                rd->pages[idx] = rd->page_alloc();    /* 0 으로 채워져 있다 */
                if (!rd->pages[idx])
                    return NoutOfMemory;
                memcpy((u8 *)rd->pages[idx] + in, src, chunk);
            }
        } else if (chunk == (usize)RAMDISK_PAGE_SIZE && all_zero(src, chunk)) {
            rd->page_free(rd->pages[idx]);            /* 페이지 전체를 0 으로: 반납과 같다 */
            rd->pages[idx] = nNULL;
        } else {
            memcpy((u8 *)rd->pages[idx] + in, src, chunk);
        }
        offset += chunk;
        src += chunk;
        size -= chunk;
    }
    return Nok;
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

    (void)info;

    if (!buffer || size == 0)
        return NinvalidArg;

    if (!range_within(offset, size, rd->size))
        return Noverflow;

    if (!range_within((u64)(usize)buffer, size, 0xFFFFFFFFFFFFFFFFUL))
        return NinvalidPointer;

    if (rd->pages) {
        sparse_read(rd, (u64)offset, (u8 *)buffer, size);
        return Nok;
    }
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

    (void)info;

    if (!buffer || size == 0)
        return NinvalidArg;

    if (!range_within(offset, size, rd->size))
        return Noverflow;

    if (!range_within((u64)(usize)buffer, size, 0xFFFFFFFFFFFFFFFFUL))
        return NinvalidPointer;

    if (rd->pages)
        return sparse_write(rd, (u64)offset, (const u8 *)buffer, size);
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

    if (rd->pages)
        return ramdisk_discard(diskno, 0, (usize)rd->size, info);
    memset((void *)(usize)rd->offset, 0, rd->size);

    return Nok;
}

Nstatus ramdisk_discard(u32 diskno, usize offset, usize size, NTBLI *info)
{
    ramdisk_t *rd = ramdisk_find(diskno);
    u64 first;
    u64 end;
    u64 idx;

    (void)info;
    if (!rd)
        return NnotFound;
    if (size == 0)
        return Nok;
    if (!range_within(offset, size, rd->size))
        return Noverflow;

    if (!rd->pages) {
        memset((void *)(usize)(rd->offset + offset), 0, size);
        return Nok;
    }

    /* 범위에 완전히 든 페이지만 반납하고, 걸친 가장자리는 0 으로 */
    first = ((u64)offset + RAMDISK_PAGE_SIZE - 1) / RAMDISK_PAGE_SIZE;
    end = ((u64)offset + size) / RAMDISK_PAGE_SIZE;
    for (idx = first; idx < end; idx++) {
        if (rd->pages[idx]) {
            rd->page_free(rd->pages[idx]);
            rd->pages[idx] = nNULL;
        }
    }
    if (first >= end) {
        usize n = size;
        u64 o = offset;

        while (n > 0) {
            u64 i2 = o / RAMDISK_PAGE_SIZE;
            usize in = (usize)(o % RAMDISK_PAGE_SIZE);
            usize chunk = (usize)RAMDISK_PAGE_SIZE - in;

            if (chunk > n)
                chunk = n;
            if (rd->pages[i2])
                memset((u8 *)rd->pages[i2] + in, 0, chunk);
            o += chunk;
            n -= chunk;
        }
    } else {
        u64 head = first * RAMDISK_PAGE_SIZE - (u64)offset;      /* first 페이지 앞의 걸친 부분 */
        u64 tail_start = end * RAMDISK_PAGE_SIZE;

        if (head > 0 && rd->pages[first - 1])
            memset((u8 *)rd->pages[first - 1] + (RAMDISK_PAGE_SIZE - head), 0, (usize)head);
        if ((u64)offset + size > tail_start && rd->pages[end])
            memset(rd->pages[end], 0, (usize)((u64)offset + size - tail_start));
    }
    return Nok;
}

u64 ramdisk_pages_in_use(u32 diskno)
{
    ramdisk_t *rd = ramdisk_find(diskno);
    u64 n = 0;
    u64 i;

    if (!rd)
        return 0;
    if (!rd->pages)
        return rd->size / RAMDISK_PAGE_SIZE;
    for (i = 0; i < rd->size / RAMDISK_PAGE_SIZE; i++) {
        if (rd->pages[i])
            n++;
    }
    return n;
}

Nstatus ramdisk_deinit(u32 diskno)
{
    ramdisk_t *rd = ramdisk_find(diskno);

    if (!rd)
        return NnotFound;

    if (rd->pages)
        (void)ramdisk_discard(diskno, 0, (usize)rd->size, nNULL);
    rd->is_used = false;
    rd->pages = nNULL;

    rd->diskno = 0;
    rd->offset = 0;
    rd->size   = 0;

    return Nok;
}
