/*
 * mkfs_nyfs.c - NyFS 볼륨을 이미지 파일이나 블록 장치에 만드는 CLI 도구.
 *
 * 사용법:
 *   mkfs.nyfs [옵션...] <이미지파일 또는 블록장치>
 *
 * 옵션:
 *   -s, --size <크기>       새로 만들 이미지 파일의 크기(예: 64M, 1G).
 *                           블록 장치를 지정했을 때는 무시하고 장치 크기를
 *                           그대로 쓴다(장치보다 크게 지정하면 오류).
 *   -b, --block-size <바이트>  블록 크기(512~65536, 기본 4096)
 *   -N, --inodes <개수>     inode 테이블 크기(0=자동, 기본 자동)
 *   -L, --label <이름>      볼륨 라벨(최대 31바이트, UTF-8)
 *   -f, --force             대상에 이미 NyFS/다른 매직이 있어도 확인 없이 덮어씀
 *   -h, --help
 *
 * 빌드: -std=c89 -pedantic -Wall -Wextra -Werror ... (Makefile 참고).
 * 문자열 리터럴 안의 한글은 유니버설 문자명(\u escape)이 아니라 UTF-8
 * 바이트를 그대로 소스에 넣는다 - \uXXXX 표기는 C99부터라 -std=c89
 * 에서는 컴파일 에러지만, 리터럴 안에 UTF-8 바이트가 그대로 들어있는
 * 것은 C89 에서도 아무 문제가 없다(컴파일러는 그냥 바이트 배열로
 * 취급한다). 소스 파일은 UTF-8 로 저장되어 있어야 한다.
 * pread/open 등 POSIX 함수를 쓰므로 nyfs_core.c 와 같은 이유로
 * _POSIX_C_SOURCE 를 정의한다.
 */
#define _POSIX_C_SOURCE 200809L

#include "nyfs_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h> /* BLKGETSIZE64 */
#include <errno.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "사용법: %s [옵션...] <이미지파일 또는 블록장치>\n"
        "  -s, --size <크기>       새 이미지 파일 크기(예: 64M, 1G). 블록장치를 지정하면 무시된다.\n"
        "  -b, --block-size <바이트>  블록 크기(512~65536, 기본 4096)\n"
        "  -N, --inodes <개수>     inode 테이블 크기(0=자동, 기본 자동)\n"
        "  -L, --label <이름>      볼륨 라벨(최대 31바이트)\n"
        "  -f, --force             대상에 이미 다른 파일시스템이 있어도 확인 없이 덮어씀\n"
        "  -h, --help\n",
        prog);
}

/* "64M", "1G", "512K", "1048576" 같은 크기 문자열을 바이트로 바꾼다.
 * 접미사는 2 진 단위(K=1024, M=1024^2, G=1024^3, T=1024^4)로 해석한다.
 * 실패하면 0 을 돌려준다(크기 0 은 어차피 무효하므로 실패 표시로 쓴다). */
static u64 parse_size(const char *s)
{
    char *end;
    double value;
    u64 mul;

    errno = 0;
    value = strtod(s, &end);
    if (errno != 0 || end == s || value < 0.0) {
        return 0;
    }

    mul = 1;
    if (*end != '\0') {
        switch (end[0]) {
            case 'k': case 'K': mul = (u64)1024; break;
            case 'm': case 'M': mul = (u64)1024 * 1024; break;
            case 'g': case 'G': mul = (u64)1024 * 1024 * 1024; break;
            case 't': case 'T': mul = (u64)1024 * 1024 * 1024 * 1024; break;
            default: return 0;
        }
        if (end[1] != '\0' && strcmp(end + 1, "iB") != 0 && strcmp(end + 1, "B") != 0
            && strcmp(end + 1, "i") != 0) {
            return 0;
        }
    }

    return (u64)(value * (double)mul);
}

static int device_size(int fd, u64 *out_size)
{
    struct stat st;

    if (fstat(fd, &st) != 0) {
        return -1;
    }
    if (S_ISREG(st.st_mode)) {
        *out_size = (u64)st.st_size;
        return 0;
    }
    if (S_ISBLK(st.st_mode)) {
        u64 sz = 0;

        if (ioctl(fd, BLKGETSIZE64, &sz) != 0) {
            return -1;
        }
        *out_size = sz;
        return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *target = nNULL;
    const char *label = nNULL;
    u64 size_bytes = 0;
    u32 block_size = 4096U;
    u64 inode_count = 0;
    int force = 0;
    int i;
    int fd;
    u64 existing_size;
    nbool creating_new_file;
    nyfs_mkfs_params_t params;
    nyfs_status_t status;
    u8 shift;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--force") == 0) {
            force = 1;
        } else if ((strcmp(arg, "-s") == 0 || strcmp(arg, "--size") == 0) && i + 1 < argc) {
            size_bytes = parse_size(argv[++i]);
            if (size_bytes == 0) {
                fprintf(stderr, "%s: 잘못된 크기: %s\n", argv[0], argv[i]);
                return 1;
            }
        } else if ((strcmp(arg, "-b") == 0 || strcmp(arg, "--block-size") == 0) && i + 1 < argc) {
            long v = strtol(argv[++i], nNULL, 10);

            if (v < 512 || v > 65536) {
                fprintf(stderr, "%s: 블록 크기는 512~65536 사이여야 함\n", argv[0]);
                return 1;
            }
            block_size = (u32)v;
        } else if ((strcmp(arg, "-N") == 0 || strcmp(arg, "--inodes") == 0) && i + 1 < argc) {
            inode_count = (u64)strtoul(argv[++i], nNULL, 10);
        } else if ((strcmp(arg, "-L") == 0 || strcmp(arg, "--label") == 0) && i + 1 < argc) {
            label = argv[++i];
        } else if (arg[0] == '-') {
            fprintf(stderr, "%s: 알 수 없는 옵션: %s\n", argv[0], arg);
            usage(argv[0]);
            return 1;
        } else if (!target) {
            target = arg;
        } else {
            fprintf(stderr, "%s: 대상은 하나만 지정할 수 있음\n", argv[0]);
            return 1;
        }
    }

    if (!target) {
        usage(argv[0]);
        return 1;
    }

    shift = 0;
    {
        u32 bs = block_size;

        while (bs > 1U) {
            if (bs & 1U) {
                fprintf(stderr, "%s: 블록 크기는 2의 거듭제곱이어야 함(4096 권장)\n", argv[0]);
                return 1;
            }
            bs >>= 1;
            shift++;
        }
    }
    if (shift < NYFS_BLOCK_SIZE_SHIFT_MIN || shift > NYFS_BLOCK_SIZE_SHIFT_MAX) {
        fprintf(stderr, "%s: 블록 크기가 범위를 벗어남\n", argv[0]);
        return 1;
    }

    creating_new_file = (nbool)(access(target, F_OK) != 0);

    if (creating_new_file && size_bytes == 0) {
        fprintf(stderr, "%s: 새 파일 생성시 크기(-s)를 지정해야 함\n", argv[0]);
        return 1;
    }

    fd = open(target, O_RDWR | (creating_new_file ? O_CREAT : 0), 0644);
    if (fd < 0) {
        fprintf(stderr, "%s: %s 열기 실패: %s\n", argv[0], target, strerror(errno));
        return 1;
    }

    if (device_size(fd, &existing_size) != 0) {
        fprintf(stderr, "%s: %s 크기 확인 실패: %s\n", argv[0], target, strerror(errno));
        close(fd);
        return 1;
    }

    if (!creating_new_file) {
        struct stat st;

        if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode)) {
            if (size_bytes != 0 && size_bytes > existing_size) {
                fprintf(stderr, "%s: 지정한 크기가 장치 크기(%lu)보다 큽니다\n",
                        argv[0], (unsigned long)existing_size);
                close(fd);
                return 1;
            }
            size_bytes = existing_size;
        } else if (size_bytes == 0) {
            size_bytes = existing_size;
        }

        if (!force) {
            nyfs_superblock_t probe;
            ssize_t n = pread(fd, &probe, sizeof(probe), 0);

            if (n == (ssize_t)sizeof(probe) && probe.magic == NYFS_MAGIC) {
                fprintf(stderr,
                    "%s: %s 에 이미 NyFS 볼륨이 있습니다. 덮어쓰려면 -f 를 주세요.\n",
                    argv[0], target);
                close(fd);
                return 1;
            }
        }
    }

    if (size_bytes < (u64)64 * (u64)block_size) {
        fprintf(stderr, "%s: 볼륨이 너무 작음(최소 %lu 바이트 필요)\n",
                argv[0], (unsigned long)((u64)64 * (u64)block_size));
        close(fd);
        return 1;
    }

    memset(&params, 0, sizeof(params));
    params.block_size_shift = shift;
    params.total_blocks = size_bytes / block_size;
    params.inode_count = inode_count;
    params.label = label;

    status = nyfs_format(fd, &params);
    if (NSTATUS_IS_ERR(status)) {
        fprintf(stderr, "%s: 포맷 실패(코드 %d)\n", argv[0], (int)status);
        close(fd);
        return 1;
    }

    if (close(fd) != 0) {
        fprintf(stderr, "%s: 닫기 실패: %s\n", argv[0], strerror(errno));
        return 1;
    }

    printf("NyFS 포맷 완료: %s\n", target);
    printf("  블록 크기: %u 바이트\n", block_size);
    printf("  총 블록 수: %lu (약 %.1f MiB)\n",
           (unsigned long)params.total_blocks,
           (double)(params.total_blocks * block_size) / (1024.0 * 1024.0));
    printf("  inode 개수: %lu\n",
           (unsigned long)(inode_count ? inode_count
               : ((params.total_blocks / 8U < 16U) ? 16U : params.total_blocks / 8U)));
    if (label) {
        printf("  라벨: %s\n", label);
    }

    return 0;
}
