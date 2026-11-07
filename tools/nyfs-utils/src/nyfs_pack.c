/*
 * nyfs_pack.c - 매니페스트로 NyFS 이미지를 만든다 (FUSE/마운트 없이, 빌드용)
 *
 *   nyfs-pack -s <크기> -o <이미지> <매니페스트>
 *
 * 매니페스트: 한 줄에 한 항목 ('#' 주석, 빈 줄 무시). 부모 디렉터리가 먼저 나와야 한다.
 *   d <이미지 경로> <모드(8진수)> <uid> <gid>
 *   f <이미지 경로> <모드(8진수)> <uid> <gid> <호스트 원본 파일>
 * "d / ..." 는 루트 디렉터리의 모드/소유자를 정한다.
 *
 * 커널 nyfs 와 같은 코어 라이브러리(nyfs_core.c)를 쓰므로 만들어진 이미지는 커널이 그대로 마운트한다
 * (NyxisOS 의 initrd = fs:/ 루트).
 */
#include "nyfs_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static const nyfs_cred_t g_root = { 0, 0 };

static u64 parse_size(const char *s)
{
    char *end = nNULL;
    unsigned long v = strtoul(s, &end, 10);   /* LP64: unsigned long 은 64비트 (C89 에 long long 없음) */

    if (!end || end == s)
        return 0;
    if (*end == 'K' || *end == 'k')
        v *= 1024UL;
    else if (*end == 'M' || *end == 'm')
        v *= 1024UL * 1024UL;
    else if (*end != '\0')
        return 0;
    return (u64)v;
}

static int copy_file(nyfs_volume_t *vol, nyfs_ino_t ino, const char *src)
{
    FILE *f = fopen(src, "rb");
    unsigned char buf[16384];
    u64 off = 0;
    size_t n;

    if (!f) {
        perror(src);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        usize done = 0;
        nyfs_status_t st = nyfs_op_write(vol, ino, &g_root, off, buf, (usize)n, &done);

        if (NSTATUS_IS_ERR(st) || done != (usize)n) {
            fprintf(stderr, "nyfs-pack: write failed for %s (status %d)\n", src, (int)st);
            fclose(f);
            return -1;
        }
        off += n;
    }
    fclose(f);
    return 0;
}

static int apply_line(nyfs_volume_t *vol, char *line, unsigned lineno)
{
    char kind[4];
    char path[1024];
    char src[1024];
    unsigned mode = 0;
    unsigned long uid = 0;
    unsigned long gid = 0;
    int fields;
    nyfs_ino_t ino = NYFS_ROOT_INO;
    nyfs_status_t st;

    src[0] = '\0';
    fields = sscanf(line, "%3s %1023s %o %lu %lu %1023s", kind, path, &mode, &uid, &gid, src);
    if (fields < 5 || (kind[0] != 'd' && kind[0] != 'f') || kind[1] != '\0' ||
        (kind[0] == 'f' && fields != 6) || mode > 07777U || path[0] != '/') {
        fprintf(stderr, "nyfs-pack: line %u: malformed entry\n", lineno);
        return -1;
    }

    if (strcmp(path, "/") != 0) {
        nyfs_ino_t parent;
        char name[NYFS_NAME_MAX + 1U];

        st = nyfs_path_lookup_parent(vol, path, &g_root, &parent, name, sizeof(name));
        if (NSTATUS_IS_ERR(st)) {
            fprintf(stderr, "nyfs-pack: line %u: parent of %s not found (list parents first)\n", lineno, path);
            return -1;
        }
        if (kind[0] == 'd')
            st = nyfs_op_mkdir(vol, parent, name, (u16)mode, &g_root, &ino);
        else
            st = nyfs_op_create(vol, parent, name, (u16)mode, &g_root, &ino);
        if (NSTATUS_IS_ERR(st)) {
            fprintf(stderr, "nyfs-pack: line %u: cannot create %s (status %d)\n", lineno, path, (int)st);
            return -1;
        }
        if (kind[0] == 'f' && copy_file(vol, ino, src) != 0)
            return -1;
    }

    st = nyfs_op_setattr(vol, ino, &g_root, 1, (u16)mode, 1, (u32)uid, (u32)gid, NYFS_TIME_KEEP, NYFS_TIME_KEEP);
    if (NSTATUS_IS_ERR(st)) {
        fprintf(stderr, "nyfs-pack: line %u: setattr failed for %s (status %d)\n", lineno, path, (int)st);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *image = nNULL;
    const char *manifest = nNULL;
    u64 size = 0;
    nyfs_mkfs_params_t mp;
    nyfs_volume_t vol;
    FILE *mf;
    char line[2600];
    unsigned lineno = 0;
    int fd;
    int i;
    int rc = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            size = parse_size(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            image = argv[++i];
        else if (!manifest && argv[i][0] != '-')
            manifest = argv[i];
        else {
            fprintf(stderr, "usage: %s -s <size[K|M]> -o <image> <manifest>\n", argv[0]);
            return 1;
        }
    }
    if (!image || !manifest || size < 64U * 1024U || (size % 4096U) != 0) {
        fprintf(stderr, "usage: %s -s <size[K|M], multiple of 4096, >= 64K> -o <image> <manifest>\n", argv[0]);
        return 1;
    }

    mf = fopen(manifest, "r");
    if (!mf) {
        perror(manifest);
        return 1;
    }

    fd = open(image, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror(image);
        fclose(mf);
        return 1;
    }

    memset(&mp, 0, sizeof(mp));
    mp.block_size_shift = 12;
    mp.total_blocks = size / 4096U;
    mp.label = "nyxis-root";
    if (NSTATUS_IS_ERR(nyfs_format(fd, &mp)) || NSTATUS_IS_ERR(nyfs_volume_open(&vol, fd, ntrue))) {
        fprintf(stderr, "nyfs-pack: format failed\n");
        close(fd);
        fclose(mf);
        return 1;
    }

    while (fgets(line, sizeof(line), mf)) {
        char *p = line;

        lineno++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        if (apply_line(&vol, p, lineno) != 0) {
            rc = 1;
            break;
        }
    }

    nyfs_volume_close(&vol);
    close(fd);
    fclose(mf);
    if (rc != 0)
        unlink(image);
    return rc;
}
