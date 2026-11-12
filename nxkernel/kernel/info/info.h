/*
 * info.h - info:/ 이름공간 (커널이 만드는 읽기 전용 가상 파일)
 *
 * 규칙 (include/nyx_abi.h 의 "이름공간"):
 *   - 파일 하나에 값 하나, 끝에 '\n' 하나. 누구나 읽을 수 있고 아무도 쓸 수 없다.
 *   - 내용은 "열 때" 한 번 만들어 핸들에 보관한다 (스냅샷). read/seek/stat 은 일반 파일과 같고,
 *     새 값이 필요하면 다시 연다.
 *   - 항목은 아래 표(info.c 의 g_nodes)로 고정되어 있다. 디렉터리는 NxReadDir 로 나열한다.
 *
 *   info:/hostname          roi:/hostname 의 값 (커널 권한으로 읽고 검사, 틀리면 "nyxis")
 *   info:/timezone          roi:/timezone 의 값 (같은 방식, 틀리면 "Europe/London"). 이름뿐이다:
 *                           시각은 늘 UTC 이고 오프셋/서머타임은 적용하지 않는다 (nlibc 의 localtime == gmtime)
 *   info:/utc_offset        UTC 오프셋 "+HH:MM". 시간대 계산이 생기기 전까지 늘 "+00:00"
 *   info:/version           운영체제 버전
 *   info:/abi               시스템 콜 ABI "주.부"
 *   info:/uptime_ms         부팅 후 밀리초
 *   info:/memory/total_kib  할당기(pfa)가 관리하는 물리 메모리 (KiB)
 *   info:/memory/free_kib   그중 남은 것 (KiB)
 *   info:/memory/ram_kib    물리 RAM 전체 (메모리 맵의 RAM 종류 합, KiB)
 *   info:/memory/reserved_kib  RAM 중 할당기 밖에 있는 것 = ram - total (커널 이미지, initrd(fs:/), tmp:/,
 *                           펌웨어 Runtime/ACPI 영역, 1MiB 아래). 쓰는 중인 RAM = ram - free
 */
#ifndef KERNEL_INFO_INFO_H
#define KERNEL_INFO_INFO_H

#include "nyxis.h"

#define NYX_OS_VERSION       "0.1"
#define INFO_DEFAULT_HOSTNAME "nyxis"
#define INFO_HOSTNAME_MAX    63U
#define INFO_DEFAULT_TIMEZONE "Europe/London"
#define INFO_TIMEZONE_MAX    63U
#define INFO_UTC_OFFSET      "+00:00"   /* 고정: 이름(timezone)과 상관없이 UTC */
#define INFO_ROI_VALUE_MAX   63U        /* roi:/ 에서 읽는 값의 최대 길이 (위 둘 중 큰 것) */
#define INFO_VALUE_MAX       256U       /* 스냅샷 하나의 최대 크기 */

#define INFO_NODE_FILE 1U
#define INFO_NODE_DIR  2U

/*
 * name: nspath 가 정규화한 info:/ 아래 경로 ("" = info:/ 자체, "memory/free_kib" 등).
 * 파일이면 *out_buf 에 kmalloc 한 내용(호출자가 kfree), *out_len 에 길이. 디렉터리면 *out_buf = NULL, 길이 0.
 * *out_type 에 INFO_NODE_*. 없는 이름이면 NnotFound.
 */
Nstatus info_open(const char *name, u32 *out_type, char **out_buf, usize *out_len);

/* 디렉터리 name 의 index 번째 항목 (없으면 NnotFound). out_name 은 NUL 종료 */
Nstatus info_readdir(const char *name, u64 index, char *out_name, usize out_max, u32 *out_type);

/* roi:/hostname 형식 검사: 길이 1..63, [a-z0-9-], '-' 로 시작하지 않음 (끝의 '\n' 은 호출자가 뗀다) */
bool info_hostname_valid(const char *s, usize n);

/* roi:/timezone 형식 검사: 길이 1..63, 영문자로 시작, [A-Za-z0-9_+-] 와 구성요소 구분 '/' (빈 구성요소 없음) */
bool info_timezone_valid(const char *s, usize n);

#endif /* KERNEL_INFO_INFO_H */
