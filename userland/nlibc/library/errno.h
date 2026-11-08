/*
 * errno.h - Nstatus -> errno 변환 (nlibc 소유, 커널 ABI 와는 별개)
 *
 * [결정 사항 (C89 libc 필요 커널 기능 요청서 2-2)]
 * 커널은 POSIX errno 를 전혀 모른다 — 시스템 콜은 항상 Nstatus(include/nyx_abi.h 가 아니라
 * include/types.h 의 Nerror) 를 부호 확장해서 돌려준다. Nstatus 는 이 프로젝트가 자랑하는
 * 기능 중 하나이므로(더 세분화된 오류, 커널 내부에서도 동일하게 쓰임), 커널의 공개 ABI
 * 헤더(nyx_abi.h)에 errno 개념을 섞지 않기로 했다. 그래서 이 변환표는 커널이 아니라
 * "libc 를 자처하는 쪽"(nlibc)이 소유한다 — 다른 libc 프로파일이 다른 매핑을 쓰고 싶을 수도
 * 있고, "raw"(libc 없이 Nstatus 를 직접 다루는) 프로그램은 이 표가 아예 필요 없기도 하다.
 *
 * C89 가 요구하는 것은 <errno.h> 에 EDOM, ERANGE, 그리고 매크로로 확장되는 errno 뿐이다
 * (POSIX 의 수십 개 코드는 C89 표준 자체의 요구사항이 아니다). 여기서는 C89 최소 요구사항에
 * 더해, 커널이 실제로 돌려주는 Nstatus 코드들과 자연스럽게 대응되는 흔한 POSIX 이름들도
 * 함께 제공한다 — 나중에 어떤 nlibc 함수가 필요로 하든 즉시 쓸 수 있게 하기 위해서다.
 *
 * 변환은 nlibc 의 시스템 콜 계층(src/nxsys.c 의 __nl_errno_from_status)이 맡는다:
 * 시스템 콜이 음수 Nstatus 를 돌려주면 가장 가까운 errno 로 바꿔 errno 에 넣고 -1 을 돌려준다.
 * 대응이 애매한 Nyxis 고유 코드는 EIO 로 뭉뚱그린다 — 정확한 원인이 필요하면 nlibc 를 거치지
 * 않고 Nstatus 를 직접 확인한다.
 */
#ifndef _NL_ERRNO_H
#define _NL_ERRNO_H

/* C89 이 요구하는 최소 집합 */
#define EDOM   1    /* 수학 함수의 정의역 오류 */
#define ERANGE 2    /* 결과가 표현 범위를 벗어남 */

/* 흔히 쓰이는 POSIX 이름들 (C89 필수는 아니지만, 대부분의 C89 코드가 암묵적으로 기대한다) */
#define EPERM        3    /* 허용되지 않는 연산 (Nstatus: Npermission, Nunauthorized) */
#define ENOENT       4    /* 파일/경로 없음 (NnotFound, NfileNotFound) */
#define EIO          5    /* 입출력 오류 (Nio) */
#define EBADF        6    /* 잘못된 핸들 (NinvalidArg, 핸들 관련) */
#define EAGAIN       7    /* 다시 시도 (Nbusy, Ntimeout) */
#define ENOMEM       8    /* 메모리 부족 (NoutOfMemory) */
#define EACCES       9    /* 접근 거부 (Npermission) */
#define EFAULT       10   /* 잘못된 포인터 (NinvalidPointer, NnullPointer) */
#define EEXIST       11   /* 이미 존재함 (NalreadyExists) */
#define ENOTDIR      12   /* 디렉터리가 아님 (Nunsupported, vfs_stat_t.type 불일치) */
#define EISDIR       13   /* 디렉터리임 (파일 연산을 디렉터리에 시도) */
#define EINVAL       14   /* 잘못된 인자 (NinvalidArg, NinvalidFormat) */
#define ENOSPC       15   /* 저장 공간 부족 (NdiskFull) */
#define EROFS        16   /* 읽기 전용 파일시스템 (NreadOnly) */
#define ENOSYS       17   /* 구현되지 않은 시스템 콜 (NsyscallFailed) */
#define ETIMEDOUT    18   /* 시간 초과 (Ntimeout) */
#define ENOTSUP      19   /* 지원하지 않음 (Nunsupported) */
#define EMFILE       20   /* 열 수 있는 스트림/핸들이 모자람 (FOPEN_MAX, NX_MAX_HANDLES) */
#define ESRCH        21   /* 그런 프로세스 없음 (kill: 이 프로세스의 자식이 아님) */
#define ECHILD       22   /* 기다릴 자식 없음 */
#define ENAMETOOLONG 23   /* 경로가 너무 김 (NpathTooLong, NX_PATH_MAX) */
#define ENOTEMPTY    24   /* 디렉터리가 비어 있지 않음 */
#define ESPIPE       25   /* 위치를 옮길 수 없는 핸들 (콘솔 등) */
#define E2BIG        26   /* 인자/환경이 너무 큼 (NX_ARG_MAX) */
#define EINTR        27   /* 신호가 잠든 호출을 깨웠다 (signal.h) */
#define EBUSY        28   /* 사용 중 (장치) */
#define EPIPE        29   /* 읽는 쪽이 없는 파이프에 쓰기 (Ndisconnected). 신호(SIGPIPE)는 없다 */
#define ENOTTY       30   /* 터미널이 아님 (isatty, tcgetattr, ioctl) */

/* C89 7.1.4: errno 는 int 형의 수정 가능한 lvalue. 성공한 호출은 errno 를 0 으로 되돌리지 않는다. */
extern int errno;
#define errno errno

#endif /* _NL_ERRNO_H */
