/*
 * config.h - Nyxis 용 TinyCC 설정 (tcc 의 configure 가 만드는 파일을 대신한다)
 *
 * Nyxis 안의 nlcc (app:/nlcc.run) 는 TinyCC 를 이 설정으로 빌드한 것이다. 설치 위치:
 *   app:/lib/tcc/             tcc 자신의 헤더(include/: stdarg.h stddef.h tccdefs.h …)와 libtcc1.a
 *   app:/lib/nlibc/include/   nlibc 헤더
 *   app:/lib/nlibc/lib/       crt1.o (= nlcrt0.o), crti.o / crtn.o (빈 오브젝트), libc.a (= libnlibc.a)
 * tcc 는 리눅스처럼 crt1.o crti.o … -lc … crtn.o 를 붙이므로 nlibc 를 그 이름으로 둔다 (tcc 를 고치지 않는다).
 */
#define TCC_VERSION "0.9.28rc"

#define TCC_TARGET_X86_64 1
#define TARGETOS_Nyxis 1          /* 패치(tinycc-nyxis.patch)가 보는 이름: 경로 규칙, 정적 링크, -run 없음 */

#define CONFIG_TCC_STATIC 1       /* dlopen 없음 */
#define CONFIG_TCC_BACKTRACE 0    /* 실행 중 역추적(신호, ucontext) 없음 */
#define CONFIG_TCC_BCHECK 0       /* 경계 검사 런타임 없음 */
#define CONFIG_TCC_SEMLOCK 0      /* 스레드 없음 */
#define CONFIG_TCC_PREDEFS 0      /* tccdefs.h 는 실행 때 {B}/include 에서 읽는다 */

#ifndef CONFIG_TCCDIR
# define CONFIG_TCCDIR "app:/lib/tcc"
#endif
#ifndef CONFIG_NLIBC_DIR
# define CONFIG_NLIBC_DIR "app:/lib/nlibc"
#endif
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include;" CONFIG_NLIBC_DIR "/include"
#define CONFIG_TCC_LIBPATHS "{B};" CONFIG_NLIBC_DIR "/lib"
#define CONFIG_TCC_CRTPREFIX CONFIG_NLIBC_DIR "/lib"
#define CONFIG_TCC_ELFINTERP "-"
