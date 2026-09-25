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
