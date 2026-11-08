/* assert.h - C89 7.2 (NDEBUG 에 따라 매번 다시 정의되므로 포함 감시가 없다) */
#undef assert

/* 실패 처리기는 NDEBUG 와 관계없이 선언해 둔다 (nlibc 자신이 정의할 때 원형이 필요하다) */
#ifndef _NL_ASSERT_FAIL_DECLARED
#define _NL_ASSERT_FAIL_DECLARED
void __nl_assert_fail(const char *expr, const char *file, int line);
#endif

#ifdef NDEBUG
#define assert(ignore) ((void)0)
#else
#define assert(expr) ((expr) ? (void)0 : __nl_assert_fail(#expr, __FILE__, __LINE__))
#endif
