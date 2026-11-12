/*
 * ramdisk.h - 메모리 영역을 블록 장치처럼 다루는 램디스크
 */
#ifndef RAMDISK_H
#define RAMDISK_H

#include "types.h"
#include "nyxis.h"
#include "memory.h"

/* 희소 램디스크의 페이지 할당/반납 (커널: kpage_alloc/kpage_free). 할당은 0 으로 채운 페이지를 돌려준다 */
typedef void *(*ramdisk_page_alloc_fn)(void);
typedef void (*ramdisk_page_free_fn)(void *page);

#define RAMDISK_PAGE_SIZE 4096UL

typedef struct {
    u32 diskno;
    u64 offset;      /* 연속 램디스크가 시작하는 커널 가상 주소 (희소면 0) */
    u64 size;
    bool is_used;
    /* 희소 모드 (ramdisk_init_sparse): pages[i] 가 [i*4K, (i+1)*4K) 를 담는 페이지, NULL 이면 그 범위는 0 */
    void **pages;
    ramdisk_page_alloc_fn page_alloc;
    ramdisk_page_free_fn page_free;
} ramdisk_t;

#define RAMDISK_MAX 16

Nstatus ramdisk_init(u32 diskno, usize ramdisk_start, usize ramdisk_size, NTBLI *info);

/*
 * 희소 램디스크: 크기만 정하고, 페이지는 처음 0 이 아닌 값을 쓸 때 page_alloc 으로 할당한다.
 *   - 할당되지 않은 범위를 읽으면 0. 비어 있는 페이지에 0 만 쓰면 할당하지 않는다 (포맷의 0 채우기).
 *   - ramdisk_discard 로 범위를 버리면 그 안에 완전히 든 페이지를 반납한다 (파일시스템이 블록을 해제할 때).
 * pages: size/RAMDISK_PAGE_SIZE 개의 NULL 로 채운 배열 (호출자 소유, 램디스크가 쓰는 동안 유지).
 * size 는 RAMDISK_PAGE_SIZE 의 배수여야 한다.
 */
Nstatus ramdisk_init_sparse(u32 diskno, usize ramdisk_size, void **pages,
                            ramdisk_page_alloc_fn page_alloc, ramdisk_page_free_fn page_free);

/* [offset, offset+size) 의 내용을 버린다 (이후 읽으면 0). 희소 램디스크는 페이지를 반납, 연속 램디스크는 0 으로 */
Nstatus ramdisk_discard(u32 diskno, usize offset, usize size, NTBLI *info);

/* 희소 램디스크가 지금 가진 페이지 수 (연속 램디스크는 크기 / 4KiB) */
u64 ramdisk_pages_in_use(u32 diskno);
Nstatus ramdisk_read(u32 diskno, usize offset, void *buffer, usize size, NTBLI *info);
Nstatus ramdisk_write(u32 diskno, usize offset, const void *buffer, usize size, NTBLI *info);
Nstatus ramdisk_deinit(u32 diskno);
Nstatus ramdisk_format(u32 diskno, NTBLI *info);

#endif /* RAMDISK_H */
