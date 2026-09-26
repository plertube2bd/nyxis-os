/*
 * test_nyfs_core.c - nyfs_core 라이브러리 회귀 테스트.
 *
 * 실제 FUSE 마운트 없이 라이브러리 API 를 직접 호출해서 포맷->생성->쓰기
 * ->읽기->mkdir->readdir->삭제->권한거부까지 커널 nyfs.c 의 test_nyfs() 가
 * 실기에서 검증한 것과 같은 흐름을 호스트에서 빠르게 재확인한다.
 *
 * -std=c89 -pedantic -Wall -Wextra -Werror 로 빌드된다(Makefile 참고).
 * 문자열 리터럴의 한글은 UTF-8 원문 그대로 쓴다(\u 이스케이프는 C99부터라
 * -std=c89 에서 컴파일 에러가 난다 - mkfs_nyfs.c 상단 주석 참고).
 */
#define _POSIX_C_SOURCE 200809L

#include "nyfs_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++; \
        } else { \
            printf("ok: %s\n", msg); \
        } \
    } while (0)

int main(void)
{
    const char *image = "/tmp/nyfs_core_test.img";
    int fd;
    nyfs_mkfs_params_t params;
    nyfs_volume_t vol;
    nyfs_cred_t root_cred;
    nyfs_cred_t other_cred;
    nyfs_status_t status;
    nyfs_ino_t file_ino;
    nyfs_ino_t dir_ino;
    nyfs_ino_t looked_up;
    char writebuf[64];
    char readbuf[64];
    usize n;
    nyfs_dirent_view_t view;

    unlink(image);
    fd = open(image, O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0, "이미지 파일 생성");

    memset(&params, 0, sizeof(params));
    params.block_size_shift = 12;   /* 4096 */
    params.total_blocks = 4096;     /* 16 MiB */
    params.label = "test";

    status = nyfs_format(fd, &params);
    CHECK(status == Nok, "nyfs_format");
    close(fd);

    fd = open(image, O_RDWR);
    status = nyfs_volume_open(&vol, fd, ntrue);
    CHECK(status == Nok, "nyfs_volume_open");

    root_cred.uid = 0;
    root_cred.gid = 0;
    other_cred.uid = 1000;
    other_cred.gid = 1000;

    status = nyfs_op_create(&vol, NYFS_ROOT_INO, "hello.txt", 0644, &root_cred, &file_ino);
    CHECK(status == Nok, "파일 생성(create)");

    status = nyfs_op_create(&vol, NYFS_ROOT_INO, "hello.txt", 0644, &root_cred, &file_ino);
    CHECK(status == NalreadyExists, "중복 생성 거부");

    strcpy(writebuf, "hello nyfs core");
    status = nyfs_op_write(&vol, file_ino, &root_cred, 0, writebuf, strlen(writebuf), &n);
    CHECK(status == Nok && n == strlen(writebuf), "파일 쓰기(write)");

    memset(readbuf, 0, sizeof(readbuf));
    status = nyfs_op_read(&vol, file_ino, &root_cred, 0, readbuf, sizeof(readbuf), &n);
    CHECK(status == Nok && n == strlen(writebuf) && memcmp(readbuf, writebuf, n) == 0,
          "파일 읽기(read) 내용 일치하는지");

    status = nyfs_op_mkdir(&vol, NYFS_ROOT_INO, "subdir", 0755, &root_cred, &dir_ino);
    CHECK(status == Nok, "디렉토리 생성(mkdir)");

    status = nyfs_path_lookup(&vol, "/subdir", &root_cred, &looked_up);
    CHECK(status == Nok && looked_up == dir_ino, "경로 조회(path_lookup)");

    status = nyfs_op_readdir_at(&vol, NYFS_ROOT_INO, &root_cred, 0, &view);
    CHECK(status == Nok, "readdir 인덱스 0");

    status = nyfs_op_readdir_at(&vol, NYFS_ROOT_INO, &root_cred, 1, &view);
    CHECK(status == Nok, "readdir 인덱스 1");

    status = nyfs_op_readdir_at(&vol, NYFS_ROOT_INO, &root_cred, 2, &view);
    CHECK(status == NnotFound, "readdir 끝남(둘밖에 없다 - 확인)");

    /* 권한 검사: uid=0 이 아닌 사용자가 0600 파일을 읽거나 쓰려고 하면
     * 거부되어야 한다 */
    {
        nyfs_ino_t priv_ino;
        usize wn;

        status = nyfs_op_create(&vol, NYFS_ROOT_INO, "private.txt", 0600, &root_cred, &priv_ino);
        CHECK(status == Nok, "관리자 전용 파일 생성");

        status = nyfs_op_write(&vol, priv_ino, &other_cred, 0, "x", 1, &wn);
        CHECK(status == Npermission, "다른 uid 쓰기 거부(0600)");

        status = nyfs_op_read(&vol, priv_ino, &other_cred, 0, readbuf, sizeof(readbuf), &wn);
        CHECK(status == Npermission, "다른 uid 읽기 거부(0600)");
    }

    /* rename: 같은 디렉터리 안에서 이름 바꾸기, 다른 디렉터리로 옮기기,
     * 이미 있는 이름으로는 거부, 디렉터리를 자기 자신 위로 옮기는 순환
     * 거부까지 확인한다. */
    {
        nyfs_ino_t moved_ino;
        nyfs_ino_t moved2_ino;
        nyfs_ino_t dir2_ino;

        status = nyfs_op_rename(&vol, NYFS_ROOT_INO, "hello.txt", NYFS_ROOT_INO, "renamed.txt", &root_cred);
        CHECK(status == Nok, "같은 디렉터리 안에서 rename");

        status = nyfs_path_lookup(&vol, "/hello.txt", &root_cred, &looked_up);
        CHECK(status == NnotFound, "옛 이름은 더 이상 없음");

        status = nyfs_path_lookup(&vol, "/renamed.txt", &root_cred, &moved_ino);
        CHECK(status == Nok, "새 이름으로 찾을 수 있음");

        status = nyfs_op_write(&vol, moved_ino, &root_cred, 0, "x", 1, &n);
        CHECK(status == Nok, "rename 후에도 같은 inode 라 쓰기 가능(내용 보존 확인용)");

        status = nyfs_op_rename(&vol, NYFS_ROOT_INO, "renamed.txt", dir_ino, "moved.txt", &root_cred);
        CHECK(status == Nok, "다른 디렉터리로 rename");

        status = nyfs_path_lookup(&vol, "/renamed.txt", &root_cred, &looked_up);
        CHECK(status == NnotFound, "루트에는 더 이상 없음");

        status = nyfs_path_lookup(&vol, "/subdir/moved.txt", &root_cred, &moved2_ino);
        CHECK(status == Nok && moved2_ino == moved_ino, "subdir 안에서 같은 inode 로 찾음");

        status = nyfs_op_create(&vol, NYFS_ROOT_INO, "already.txt", 0644, &root_cred, &looked_up);
        CHECK(status == Nok, "already.txt 준비");
        status = nyfs_op_rename(&vol, dir_ino, "moved.txt", NYFS_ROOT_INO, "already.txt", &root_cred);
        CHECK(status == NalreadyExists, "이미 있는 이름으로 rename 하면 거부(덮어쓰기 미지원)");

        status = nyfs_op_mkdir(&vol, dir_ino, "childdir", 0755, &root_cred, &dir2_ino);
        CHECK(status == Nok, "순환 테스트용 하위 디렉터리 생성");
        status = nyfs_op_rename(&vol, NYFS_ROOT_INO, "subdir", dir_ino, "self", &root_cred);
        CHECK(status == NinvalidArg, "디렉터리를 자기 자신 위로 옮기는 순환 거부");

        /* 원상 복구(이후 unlink/rmdir 검증이 기대하는 이름으로) */
        status = nyfs_op_remove(&vol, NYFS_ROOT_INO, "already.txt", &root_cred, nfalse);
        CHECK(status == Nok, "already.txt 정리");
        status = nyfs_op_remove(&vol, dir_ino, "childdir", &root_cred, ntrue);
        CHECK(status == Nok, "childdir 정리");
        status = nyfs_op_rename(&vol, dir_ino, "moved.txt", NYFS_ROOT_INO, "hello.txt", &root_cred);
        CHECK(status == Nok, "이후 검증을 위해 hello.txt 로 다시 되돌림");
    }

    /* ACL 상속: 부모 디렉터리에 INHERIT_FILE ACE 를 하나 넣고 파일을
     * 만들면 자식이 그 ACE 를 상속받는지 확인한다(NTFS 관례). */
    {
        nyfs_inode_t parent_inode;
        nyfs_inode_t child_inode;
        nyfs_ino_t acl_dir_ino;
        nyfs_ino_t child_ino;
        usize wn;

        status = nyfs_op_mkdir(&vol, NYFS_ROOT_INO, "acltest", 0755, &root_cred, &acl_dir_ino);
        CHECK(status == Nok, "ACL 상속 테스트용 디렉터리 생성");

        status = nyfs_read_inode(&vol, acl_dir_ino, &parent_inode);
        CHECK(status == Nok, "부모 inode 읽기");

        parent_inode.ace_count = 1;
        parent_inode.ace[0].id = other_cred.uid;
        parent_inode.ace[0].access_mask = NYFS_ACE_READ_DATA;
        parent_inode.ace[0].type = NYFS_ACE_TYPE_ALLOW;
        parent_inode.ace[0].flags = NYFS_ACE_FLAG_INHERIT_FILE;
        parent_inode.ace[0].reserved = 0;
        status = nyfs_write_inode(&vol, acl_dir_ino, &parent_inode);
        CHECK(status == Nok, "부모에 상속형 ACE 직접 기록(테스트 전용 - 실제 ACL 설정 API 는 아직 없음)");

        status = nyfs_op_create(&vol, acl_dir_ino, "inherited.txt", 0600, &root_cred, &child_ino);
        CHECK(status == Nok, "ACL 있는 디렉터리 밑에 파일 생성");

        status = nyfs_read_inode(&vol, child_ino, &child_inode);
        CHECK(status == Nok && child_inode.ace_count == 1
              && child_inode.ace[0].id == other_cred.uid
              && child_inode.ace[0].access_mask == NYFS_ACE_READ_DATA
              && !(child_inode.ace[0].flags & NYFS_ACE_FLAG_INHERIT_ONLY),
              "자식이 부모의 INHERIT_FILE ACE 를 상속받음(0600 UNIX 모드는 무시되고 ACL 로만 판정)");

        /* other_cred 는 0600 의 UNIX 소유자가 아니지만, 상속받은 ACE 가
         * READ_DATA 를 허용하므로 읽기는 되고 쓰기는 여전히 막혀야 한다. */
        status = nyfs_op_read(&vol, child_ino, &other_cred, 0, readbuf, sizeof(readbuf), &wn);
        CHECK(status == Nok, "상속된 ACE 덕분에 다른 uid 도 읽기 허용됨");

        status = nyfs_op_write(&vol, child_ino, &other_cred, 0, "y", 1, &wn);
        CHECK(status == Npermission, "READ_DATA 만 상속했으므로 쓰기는 여전히 거부");

        status = nyfs_op_remove(&vol, acl_dir_ino, "inherited.txt", &root_cred, nfalse);
        CHECK(status == Nok, "ACL 테스트 파일 정리");
        status = nyfs_op_remove(&vol, NYFS_ROOT_INO, "acltest", &root_cred, ntrue);
        CHECK(status == Nok, "ACL 테스트 디렉터리 정리");
    }

    /* unlink/rmdir 타입 검사 */
    status = nyfs_op_remove(&vol, NYFS_ROOT_INO, "hello.txt", &root_cred, ntrue);
    CHECK(status == Nunsupported, "파일을 rmdir 대상으로 요청하면 거부");

    status = nyfs_op_remove(&vol, NYFS_ROOT_INO, "subdir", &root_cred, nfalse);
    CHECK(status == Nunsupported, "디렉토리를 unlink 대상으로 요청하면 거부");

    status = nyfs_op_remove(&vol, NYFS_ROOT_INO, "hello.txt", &root_cred, nfalse);
    CHECK(status == Nok, "파일 삭제(unlink)");

    status = nyfs_op_remove(&vol, NYFS_ROOT_INO, "subdir", &root_cred, ntrue);
    CHECK(status == Nok, "빈 디렉토리 삭제(rmdir)");

    status = nyfs_path_lookup(&vol, "/hello.txt", &root_cred, &looked_up);
    CHECK(status == NnotFound, "삭제된 파일은 더 이상 찾을 수 없음");

    /* 큰 파일: extent 체인이 extent 블록 하나를 넘어가는지(여러 블록에
     * 걸친 쓰기/읽기) 확인 - 4096 바이트 블록에서 1MiB 쓰기 */
    {
        nyfs_ino_t big_ino;
        char *bigbuf;
        char *rbuf;
        usize i;
        usize written, gotn;
        const usize bigsize = 1024U * 1024U;

        bigbuf = malloc(bigsize);
        rbuf = malloc(bigsize);
        CHECK(bigbuf != nNULL && rbuf != nNULL, "헙 할당");

        for (i = 0; i < bigsize; i++) {
            bigbuf[i] = (char)(i % 251U);
        }

        status = nyfs_op_create(&vol, NYFS_ROOT_INO, "big.bin", 0644, &root_cred, &big_ino);
        CHECK(status == Nok, "큰 파일 생성");

        status = nyfs_op_write(&vol, big_ino, &root_cred, 0, bigbuf, bigsize, &written);
        CHECK(status == Nok && written == bigsize, "큰 파일 쓰기(1MiB)");

        memset(rbuf, 0, bigsize);
        status = nyfs_op_read(&vol, big_ino, &root_cred, 0, rbuf, bigsize, &gotn);
        CHECK(status == Nok && gotn == bigsize && memcmp(rbuf, bigbuf, bigsize) == 0,
              "큰 파일 읽기 내용 일치");

        free(bigbuf);
        free(rbuf);
    }

    nyfs_volume_close(&vol);
    close(fd);

    /* 다시 열어서 마운트 카운트/체크섬이 정상적으로 디스크에 남아있는지도
     * 확인한다(재마운트 시나리오) */
    fd = open(image, O_RDWR);
    status = nyfs_volume_open(&vol, fd, ntrue);
    CHECK(status == Nok, "재마운트");
    CHECK(vol.sb.mount_count == 2, "mount_count 증가 확인");
    nyfs_volume_close(&vol);
    close(fd);

    printf("\n%s (%d 실패)\n", g_failures == 0 ? "전체 통과" : "실패 있음", g_failures);
    return g_failures == 0 ? 0 : 1;
}
