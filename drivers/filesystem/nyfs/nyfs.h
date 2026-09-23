/*
 * nyfs.h - NyFS 커널 연동 계층(공개 인터페이스)
 *
 * nyfs_disk.h 가 "디스크에 뭐가 적히는지"를 정의한다면, 이 헤더는 "커널이 그걸 어떻게
 * 쓰는지"를 정의한다: mount/unmount/format 진입점과, drivers/filesystem/core.h 의
 * filesystem_ops_t.mount/format 이 받는 void *userdata 를 실제로 어떤 구조체로
 * 캐스팅할지(nyfs_mount_params_t)를 정한다.
 *
 * 콜백 시그니처(nyfs_disk_read_fn/write_fn)는 기존 fat16_mount_params_t 와 똑같이
 * "u32 diskno + 바이트 오프셋 + 크기" 형태로 맞췄다. ramdisk_read/ramdisk_write 가
 * 이미 이 모양이라 그대로 꽂아 쓸 수 있다. ATA/AHCI 는 아직 이 시그니처의 읽기/쓰기
 * 쌍을 둘 다 제공하지 않으므로(ahci_read 만 있고 ahci_write 는 없음, ata 는 섹터
 * 단위 LBA28 API 라 모양이 다름) 지금 당장 nyfs 로 실제 쓰기가 되는 백엔드는
 * ramdisk 뿐이다 - 이건 nyfs 의 제약이 아니라 기존 드라이버 계층의 공백이고,
 * fat16 도 이미 같은 이유로 읽기 전용이다.
 */
#ifndef NYFS_H
#define NYFS_H

#include "types.h"

typedef Nstatus (*nyfs_disk_read_fn)(u32 diskno, usize offset, void *buffer, usize size, NTBLI *info);
typedef Nstatus (*nyfs_disk_write_fn)(u32 diskno, usize offset, const void *buffer, usize size, NTBLI *info);

/*
 * format()/mount() 에 넘기는 파라미터.
 *  - mount() 시점에는 read/write/info 만 있으면 된다. total_blocks/inode_count/
 *    block_size_shift 는 이미 디스크의 슈퍼블록에 있으므로 무시된다.
 *  - format() 시점에는 전부 필요하다. inode_count 를 0 으로 주면 total_blocks 로부터
 *    적당한 기본값을 자동으로 고른다(볼륨 크기의 대략 1/32, 최소 16개).
 */
typedef struct {
    const char *mount_path;     /* mount() 전용. vfs_mount() 에 그대로 넘어간다 */
    NTBLI *info;
    nyfs_disk_read_fn read;
    nyfs_disk_write_fn write;
    u64 total_blocks;           /* format() 전용 */
    u64 inode_count;            /* format() 전용. 0 = 자동 */
    u8  block_size_shift;       /* format() 전용. NYFS_BLOCK_SIZE_SHIFT_MIN..MAX */
} nyfs_mount_params_t;

/*
 * 호출자 신원. 모든 권한 검사(UNIX 모드 + ACL)는 이 구조체 하나만 보고 이뤄진다.
 * nyfs 내부에서는 kernel/process/process.h 의 current_process->uid/gid 를 읽어
 * 이 구조체를 채운다(호출자가 인자로 직접 넘길 수 있는 경로는 없다 - 그래야
 * 프로세스가 자기 uid 를 속여서 권한 검사를 우회할 수 없다).
 * ring3 프로세스/로그인 개념이 아직 없어 현재는 사실상 항상 root(0,0)이다.
 */
typedef struct {
    u32 uid;
    u32 gid;
} nyfs_cred_t;

/* "nyfs" 라는 이름으로 filesystem_register() 에 등록한다. 커널 초기화 시 한 번 호출한다. */
Nstatus nyfs_register(void);

/* filesystem_ops_t 시그니처와 맞춘 진입점들. 보통 filesystem_mount()/filesystem_format()
 * 을 통해 간접 호출되지만, 직접 호출해도 된다. userdata 는 nyfs_mount_params_t* 로
 * 캐스팅해서 쓴다. */
Nstatus nyfs_mount(u32 diskno, void *userdata);
Nstatus nyfs_unmount(u32 diskno);
Nstatus nyfs_format(u32 diskno, void *userdata);

#endif /* NYFS_H */
