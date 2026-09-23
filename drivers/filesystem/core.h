/*
 * core.h - 파일시스템 드라이버 등록 테이블
 *
 * [수정 이력] '#pragma once' (비표준) 제거, '#endif //' 라인 주석 제거 (C89).
 */
#ifndef FILESYSTEMCORE_H
#define FILESYSTEMCORE_H

#include "nyxis.h"

#define FILESYSTEM_MAX       32
#define FILESYSTEM_NAME_MAX  32

typedef struct {
    Nstatus (*mount)(u32 diskno, void *userdata);
    Nstatus (*unmount)(u32 diskno);
    /* userdata 는 mount 와 동일한 규칙: 드라이버가 자신의 전용 파라미터 구조체로
     * 캐스팅해서 쓴다. mkfs(포맷)에는 블록 장치 read/write 콜백과 총 블록 수 같은
     * 정보가 필요하므로 mount 와 마찬가지로 userdata 를 받아야 한다. */
    Nstatus (*format)(u32 diskno, void *userdata);
} filesystem_ops_t;

typedef struct {
    char name[FILESYSTEM_NAME_MAX];
    filesystem_ops_t ops;
    bool is_used;
} filesystem_t;

Nstatus filesystem_register(
    const char *name,
    filesystem_ops_t ops
);

Nstatus filesystem_unregister(
    const char *name
);

filesystem_t *filesystem_get(
    const char *name
);

Nstatus filesystem_mount(
    const char *fsname,
    u32 diskno,
    void *userdata
);

Nstatus filesystem_unmount(
    const char *fsname,
    u32 diskno
);

Nstatus filesystem_format(
    const char *fsname,
    u32 diskno,
    void *userdata
);

#endif /* FILESYSTEMCORE_H */
