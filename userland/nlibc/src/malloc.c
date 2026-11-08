/*
 * malloc.c - C89 7.10.3 메모리 관리 (NxVirtualAlloc = brk 위의 할당자)
 *
 * 구조:
 *   블록 = 16바이트 머리 + 사용자 영역. 모든 블록 크기와 주소는 16의 배수 (max_align).
 *   빈 블록은 주소 순으로 정렬된 단방향 목록에 있고, free 할 때 앞뒤 이웃과 합친다.
 *   할당은 first-fit, 남는 부분이 최소 블록 이상이면 쪼갠다.
 *   힙이 모자라면 brk 를 최소 NL_GROW_MIN 씩 늘린다 (brk 는 연속이므로 새 영역은 마지막 빈 블록과
 *   합쳐질 수 있다). 끝에 붙은 큰 빈 블록은 brk 를 줄여 커널에 돌려준다.
 *
 * 사용 중인 블록의 머리 next 자리에는 표식(NL_MAGIC)을 둔다: free/realloc 에 엉뚱한 포인터나
 * 이미 해제한 포인터가 들어오면 조용히 힙을 망가뜨리는 대신 abort 한다.
 *
 * 스레드가 없으므로 잠금은 없다.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nl_internal.h"

typedef struct nl_block {
    size_t size;                /* 머리 포함 블록 전체 크기 */
    struct nl_block *next;      /* 빈 블록: 다음 빈 블록 / 사용 중: NL_MAGIC */
} nl_block;

#define NL_ALIGN     16UL
#define NL_HDR       sizeof(nl_block)            /* 16 */
#define NL_MIN_BLOCK (NL_HDR + NL_ALIGN)         /* 32 */
#define NL_GROW_MIN  (64UL * 1024UL)
#define NL_TRIM_MIN  (256UL * 1024UL)            /* 끝의 빈 블록이 이보다 크면 brk 를 줄인다 */
#define NL_MAGIC     ((nl_block *)0x6E6C6962616C6CUL)   /* "nlliball" 비슷한 값: 정렬되지 않은 주소 */
#define NL_MAX_REQ   (0x7FFFFFFFFFFFFFFFUL - NL_GROW_MIN)

static nl_block *free_list;
static char *heap_start;        /* 첫 블록의 주소 */
static char *heap_end;          /* 현재 brk (우리가 아는 힙의 끝) */

static void heap_corrupt(const char *what)
{
    static const char prefix[] = "nlibc: ";
    static const char suffix[] = ": invalid pointer or heap corruption\n";

    (void)__nl_write(2, prefix, sizeof(prefix) - 1);
    (void)__nl_write(2, what, strlen(what));
    (void)__nl_write(2, suffix, sizeof(suffix) - 1);
    abort();
}

/* 요청 크기 -> 블록 크기 (머리 포함, 16의 배수). 넘치면 0 */
static size_t block_size(size_t n)
{
    if (n > NL_MAX_REQ)
        return 0;
    n = (n + NL_HDR + NL_ALIGN - 1) & ~(NL_ALIGN - 1);
    return n < NL_MIN_BLOCK ? NL_MIN_BLOCK : n;
}

/* 빈 블록 b 를 주소 순 목록에 넣고 이웃과 합친다 */
static void insert_free(nl_block *b)
{
    nl_block *prev = NULL;
    nl_block *cur = free_list;

    while (cur && cur < b) {
        prev = cur;
        cur = cur->next;
    }
    if ((cur && (char *)b + b->size > (char *)cur)
        || (prev && (char *)prev + prev->size > (char *)b))
        heap_corrupt("free");                       /* 이미 빈 영역과 겹친다 = 이중 해제 */

    b->next = cur;
    if (cur && (char *)b + b->size == (char *)cur) {
        b->size += cur->size;
        b->next = cur->next;
    }
    if (prev) {
        prev->next = b;
        if ((char *)prev + prev->size == (char *)b) {
            prev->size += b->size;
            prev->next = b->next;
        }
    } else {
        free_list = b;
    }
}

/* 힙 끝에 붙은 큰 빈 블록을 커널에 돌려준다 */
static void trim_top(void)
{
    nl_block *b = free_list;
    size_t give;

    if (!b)
        return;
    while (b->next)
        b = b->next;
    if ((char *)b + b->size != heap_end || b->size < NL_TRIM_MIN)
        return;
    give = (b->size - NL_GROW_MIN) & ~(NL_ALIGN - 1);   /* 다음 할당을 위해 조금은 남긴다 */
    if (__nl_sbrk(-(long)give) == NULL)
        return;
    heap_end -= give;
    b->size -= give;
}

/* 힙을 최소 need 바이트 늘리고 새 영역을 빈 목록에 넣는다 */
static int grow(size_t need)
{
    size_t inc = need < NL_GROW_MIN ? NL_GROW_MIN : need;
    char *old;
    nl_block *b;

    if (heap_end == NULL) {
        /* 처음: brk 를 16바이트 경계에 맞춘다 */
        size_t pad;

        old = (char *)__nl_sbrk(0);
        if (old == NULL)
            return -1;
        pad = (NL_ALIGN - ((unsigned long)old & (NL_ALIGN - 1))) & (NL_ALIGN - 1);
        if (pad && __nl_sbrk((long)pad) == NULL)
            return -1;
        heap_end = old + pad;
        heap_start = heap_end;
    }
    old = (char *)__nl_sbrk((long)inc);
    if (old == NULL)
        return -1;
    if (old != heap_end)
        heap_corrupt("brk moved");  /* 다른 누군가가 brk 를 바꿨다: 힙의 연속성 가정이 깨진다 */
    heap_end = old + inc;
    b = (nl_block *)old;
    b->size = inc;
    insert_free(b);
    return 0;
}

/* 사용 중인 블록 b 를 want 크기로 줄이고 남는 꼬리를 빈 목록으로 */
static void split(nl_block *b, size_t want)
{
    nl_block *rest;

    if (b->size - want < NL_MIN_BLOCK)
        return;
    rest = (nl_block *)((char *)b + want);
    rest->size = b->size - want;
    b->size = want;
    insert_free(rest);
}

void *malloc(size_t size)
{
    size_t need = block_size(size);
    int tried_grow = 0;

    if (need == 0) {
        errno = ENOMEM;
        return NULL;
    }
    for (;;) {
        nl_block *prev = NULL;
        nl_block *b = free_list;

        for (; b; prev = b, b = b->next) {
            if (b->size >= need) {
                if (prev)
                    prev->next = b->next;
                else
                    free_list = b->next;
                b->next = NL_MAGIC;
                split(b, need);
                return (char *)b + NL_HDR;
            }
        }
        if (tried_grow || grow(need) < 0) {
            errno = ENOMEM;
            return NULL;
        }
        tried_grow = 1;
    }
}

static nl_block *header_of(void *ptr, const char *who)
{
    nl_block *b = (nl_block *)((char *)ptr - NL_HDR);

    if (((unsigned long)ptr & (NL_ALIGN - 1)) != 0 || heap_end == NULL
        || (char *)b < heap_start || (char *)ptr > heap_end
        || b->next != NL_MAGIC || b->size < NL_MIN_BLOCK
        || (char *)b + b->size > heap_end)
        heap_corrupt(who);
    return b;
}

void free(void *ptr)
{
    nl_block *b;

    if (ptr == NULL)
        return;
    b = header_of(ptr, "free");
    insert_free(b);
    trim_top();
}

void *calloc(size_t nmemb, size_t size)
{
    void *p;

    if (size != 0 && nmemb > (size_t)-1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    p = malloc(nmemb * size);
    if (p)
        memset(p, 0, nmemb * size);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    nl_block *b;
    size_t need;
    void *n;

    if (ptr == NULL)
        return malloc(size);
    if (size == 0) {            /* C89 7.10.3.4: 크기 0 이면 해제 */
        free(ptr);
        return NULL;
    }
    b = header_of(ptr, "realloc");
    need = block_size(size);
    if (need == 0) {
        errno = ENOMEM;
        return NULL;
    }
    if (need <= b->size) {
        split(b, need);
        return ptr;
    }

    /* 바로 뒤가 빈 블록이고 합쳐서 충분하면 제자리에서 늘린다 */
    {
        nl_block *prev = NULL;
        nl_block *f = free_list;
        char *end = (char *)b + b->size;

        while (f && (char *)f < end) {
            prev = f;
            f = f->next;
        }
        if (f && (char *)f == end && b->size + f->size >= need) {
            if (prev)
                prev->next = f->next;
            else
                free_list = f->next;
            b->size += f->size;
            split(b, need);
            return ptr;
        }
    }

    n = malloc(size);
    if (n == NULL)
        return NULL;            /* 원래 블록은 그대로 남는다 */
    memcpy(n, ptr, b->size - NL_HDR);
    free(ptr);
    return n;
}
