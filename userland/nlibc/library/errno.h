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
 * 사용법: 시스템 콜 래퍼(nyxstd.h)가 음수를 돌려주면, 그 값을 다시 양의 Nstatus 코드로
 * 바꾼 뒤 nx_errno_from_status() 에 넘기고 결과를 전역 변수 errno 에 저장한다. 이 변환은
 * nlibc 의 syscall 래퍼 계층(아직 작성되지 않음)이 담당할 일이며, 이 헤더 자신은 그 계층을
 * 강제하지 않는다 (헤더만 먼저 정해 두는 것 — 아직 "libc 를 만들 때가 아니다" 라는 이전
 * 결정과 일치한다).
 */
#ifndef NX_ERRNO_H
#define NX_ERRNO_H

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

/*
 * 커널이 돌려준 "양의" Nstatus 오류 코드(즉, 시스템 콜의 음수 반환값에 -1 을 곱한 값)를
 * 가장 가까운 errno 값으로 바꾼다. 대응이 애매하거나 이 표에 없는 Nyxis 고유 코드는 뭉뚱그려
 * EIO 로 돌려준다(무엇이 잘못됐는지는 알 수 없지만 "그냥 성공은 아니다"는 알려줘야 하므로) —
 * 정확한 원인을 알아야 한다면 nlibc 를 거치지 않고 Nstatus 자체를 직접 확인하면 된다.
 *
 * 이 함수는 값 하나짜리 조회이므로 일부러 인라인 함수로 헤더에 둔다 (nlibc 정적 라이브러리가
 * 아직 없으므로, .c 파일 없이도 이 헤더 하나만 넣으면 바로 쓸 수 있게 하기 위해서다).
 */
static __inline__ int nx_errno_from_status(unsigned int nstatus_code)
{
    switch (nstatus_code) {
    case 1:  return ENOENT;      /* NnotFound */
    case 2:  return EINVAL;      /* NinvalidArg */
    case 3:  return ENOMEM;      /* NoutOfMemory */
    case 4:  return EPERM;       /* Npermission */
    case 5:  return ETIMEDOUT;   /* Ntimeout */
    case 6:  return EAGAIN;      /* Nbusy */
    case 7:  return EEXIST;      /* NalreadyExists */
    case 10: return EIO;         /* Nio */
    case 12: return ENOTSUP;     /* Nunsupported */
    case 21: return EFAULT;      /* NnullPointer */
    case 22: return EFAULT;      /* NinvalidPointer */
    case 29: return ENOENT;      /* NfileNotFound */
    case 30: return ENOENT;      /* NpathTooLong (경로 관련이므로 가장 가까운 값) */
    case 31: return EROFS;       /* NreadOnly */
    case 32: return ENOSPC;      /* NdiskFull */
    case 44: return EACCES;      /* Nunauthorized */
    case 45: return EACCES;      /* Nauthentication */
    case 47: return EINVAL;      /* NinvalidFormat */
    case 58: return ENOSYS;      /* NsyscallFailed */
    default: return EIO;
    }
}

#endif /* NX_ERRNO_H */
