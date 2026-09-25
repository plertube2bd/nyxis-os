/*
 * nyfs_fuse.c - NyFS 를 리눅스에서 FUSE3 로 마운트하는 드라이버.
 *
 * 이 파일만 예외적으로 -std=gnu99 로 빌드한다: libfuse3 헤더(fuse3/fuse.h)
 * 자체가 C99 API 라서 -std=c89 로는 헤더를 포함하는 순간부터 깨진다.
 * (nxkernel/Makefile 도 호스트 테스트 빌드에서 이미 -std=gnu99 를 쓰는
 * 선례가 있다 - 그 관례를 그대로 따른다.) 실제 온디스크 포맷을 다루는
 * 로직은 전부 nyfs_core.c(엄격한 C89) 쪽에 있고, 이 파일은 오직
 * "FUSE 콜백 <-> libnyfs 호출"을 잇는 얇은 글루 코드다.
 *
 * 사용법:
 *   nyfs-fuse <이미지파일 또는 블록장치> <마운트포인트> [FUSE 옵션...]
 *   예) ./nyfs-fuse disk.img /mnt/nyfs -f            (포그라운드로 실행)
 *       fusermount3 -u /mnt/nyfs                      (언마운트)
 *
 * 범위: 커널 drivers/filesystem/nyfs/nyfs.c 와 동일 - rename 없음, ACL
 * 상속 없음, extent depth 0 만, 저널 없음(2026-09-25 사용자와 합의).
 * chmod/chown/utimens 는 온디스크 포맷을 바꾸지 않는 범위 내에서 지원한다
 * (nyfs_core.h 상단 주석 참고).
 */
#define FUSE_USE_VERSION 31

#include "nyfs_core.h"

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

/* 전역 볼륨 핸들 하나. FUSE 는 프로세스당 마운트 하나를 다루므로 이걸로
 * 충분하다(private_data 로 넘기는 방법도 있지만, 콜백 시그니처가 전부
 * fuse_get_context() 로 이걸 다시 꺼내야 해서 오히려 코드가 늘어난다). */
static nyfs_volume_t g_vol;

/* ------------------------------------------------------------------ */
/* 자잘한 변환 도우미                                                    */
/* ------------------------------------------------------------------ */

static int nyfs_status_to_errno(nyfs_status_t status)
{
    switch (status) {
        case Nok:            return 0;
        case NnotFound:      return -ENOENT;
        case NinvalidArg:    return -EINVAL;
        case NoutOfMemory:   return -ENOSPC;   /* inode 테이블 꽉 찬 경우도 포함 */
        case Npermission:    return -EACCES;
        case Nbusy:          return -ENOTEMPTY;
        case NalreadyExists: return -EEXIST;
        case Nio:            return -EIO;
        case Nunsupported:   return -ENOTSUP;
        case Ncorrupted:     return -EIO;
        case NreadOnly:      return -EROFS;
        case NdiskFull:      return -ENOSPC;
        case NbadFilesystem: return -EIO;
        case NnameTooLong:   return -ENAMETOOLONG;
        default:             return -EIO;
    }
}

static void nyfs_cred_from_context(nyfs_cred_t *cred)
{
    const struct fuse_context *ctx = fuse_get_context();

    cred->uid = ctx->uid;
    cred->gid = ctx->gid;
}

static u64 nyfs_now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static void nyfs_fill_stat(struct stat *st, nyfs_ino_t ino, const nyfs_inode_t *inode)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = (ino_t)ino;
    st->st_nlink = inode->link_count ? inode->link_count : 1;
    st->st_uid = inode->uid;
    st->st_gid = inode->gid;
    st->st_size = (off_t)inode->size;
    st->st_blksize = (blksize_t)g_vol.block_size;
    st->st_blocks = (blkcnt_t)((inode->size + 511U) / 512U);

    st->st_mtim.tv_sec = (time_t)(inode->mtime / 1000000000ULL);
    st->st_mtim.tv_nsec = (long)(inode->mtime % 1000000000ULL);
    st->st_atim.tv_sec = (time_t)(inode->atime / 1000000000ULL);
    st->st_atim.tv_nsec = (long)(inode->atime % 1000000000ULL);
    st->st_ctim.tv_sec = (time_t)(inode->ctime / 1000000000ULL);
    st->st_ctim.tv_nsec = (long)(inode->ctime % 1000000000ULL);

    switch (inode->type) {
        case NYFS_INO_TYPE_DIR:
            st->st_mode = (mode_t)(S_IFDIR | (inode->unix_mode & 07777U));
            break;
        case NYFS_INO_TYPE_SYMLINK:
            st->st_mode = (mode_t)(S_IFLNK | (inode->unix_mode & 07777U));
            break;
        case NYFS_INO_TYPE_DEVICE:
            st->st_mode = (mode_t)(S_IFCHR | (inode->unix_mode & 07777U));
            break;
        default:
            st->st_mode = (mode_t)(S_IFREG | (inode->unix_mode & 07777U));
            break;
    }
}

/* path(FUSE 가 주는, 항상 "/" 로 시작하는 절대경로)를 inode 번호로 바꾼다 */
static nyfs_status_t nyfs_resolve(const char *path, const nyfs_cred_t *cred, nyfs_ino_t *out_ino)
{
    if (strcmp(path, "/") == 0) {
        *out_ino = NYFS_ROOT_INO;
        return Nok;
    }
    return nyfs_path_lookup(&g_vol, path, cred, out_ino);
}

/* ------------------------------------------------------------------ */
/* FUSE 콜백                                                            */
/* ------------------------------------------------------------------ */

static int op_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    nyfs_cred_t cred;
    nyfs_ino_t ino;
    nyfs_inode_t inode;
    nyfs_status_t status;

    (void)fi;
    nyfs_cred_from_context(&cred);

    status = nyfs_resolve(path, &cred, &ino);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    status = nyfs_read_inode(&g_vol, ino, &inode);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    nyfs_fill_stat(stbuf, ino, &inode);
    return 0;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    nyfs_cred_t cred;
    nyfs_ino_t dir_ino;
    nyfs_status_t status;
    u64 slot;

    (void)fi;
    (void)flags;
    nyfs_cred_from_context(&cred);

    status = nyfs_resolve(path, &cred, &dir_ino);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    /* libfuse 는 한 번의 readdir 호출 안에서 filler 에 넘기는 off 값이
     * "전부 0"(전통적 - 버퍼가 찰 때까지 이어붙이는 방식)이거나 "전부
     * 0이 아닌 값"(오프셋 기반 - 중간에서 이어 읽기가 가능한 방식) 이어야
     * 한다. 둘을 섞으면(예전에 "."/"..' 만 off=0, 실제 항목은 off=index+1
     * 로 준 것처럼) libfuse 내부 버퍼 계산이 깨져 커널에 EIO 로 보인다.
     * 그래서 여기서는 전체를 하나의 슬롯 번호로 통일한다:
     *   슬롯 0 = "."   (다음 off = 1)
     *   슬롯 1 = ".."  (다음 off = 2)
     *   슬롯 2+i = dirhash 의 i 번째 자식 (다음 off = 3+i) */
    for (slot = (u64)offset; ; slot++) {
        struct stat st;
        const char *name;
        nyfs_dirent_view_t view;

        memset(&st, 0, sizeof(st));

        if (slot == 0) {
            name = ".";
            st.st_ino = (ino_t)dir_ino;
            st.st_mode = S_IFDIR;
        } else if (slot == 1) {
            name = "..";
            /* 부모 inode 번호는 알아내려면 별도 조회가 필요하지만, 값이
             * 정확하지 않아도 경로 해석에는 영향이 없다(경로 기반으로
             * 다시 조회하므로) - 자리표시자로 dir_ino 를 넣는다. */
            st.st_ino = (ino_t)dir_ino;
            st.st_mode = S_IFDIR;
        } else {
            u64 dirhash_index = slot - 2U;

            status = nyfs_op_readdir_at(&g_vol, dir_ino, &cred, dirhash_index, &view);
            if (status == NnotFound) {
                break;
            }
            if (NSTATUS_IS_ERR(status)) {
                return nyfs_status_to_errno(status);
            }

            name = view.name;
            st.st_ino = (ino_t)view.ino;
            st.st_mode = (view.type == NYFS_DIRENT_TYPE_DIR) ? S_IFDIR : S_IFREG;
        }

        if (filler(buf, name, &st, (off_t)(slot + 1U), 0)) {
            break;
        }
    }

    return 0;
}

static int op_mkdir(const char *path, mode_t mode)
{
    nyfs_cred_t cred;
    nyfs_ino_t parent_ino;
    nyfs_ino_t new_ino;
    char name[NYFS_NAME_MAX + 1U];
    nyfs_status_t status;

    nyfs_cred_from_context(&cred);

    status = nyfs_path_lookup_parent(&g_vol, path, &cred, &parent_ino, name, sizeof(name));
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    status = nyfs_op_mkdir(&g_vol, parent_ino, name, (u16)mode, &cred, &new_ino);
    return nyfs_status_to_errno(status);
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    nyfs_cred_t cred;
    nyfs_ino_t parent_ino;
    nyfs_ino_t new_ino;
    char name[NYFS_NAME_MAX + 1U];
    nyfs_status_t status;

    (void)fi;
    nyfs_cred_from_context(&cred);

    status = nyfs_path_lookup_parent(&g_vol, path, &cred, &parent_ino, name, sizeof(name));
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    status = nyfs_op_create(&g_vol, parent_ino, name, (u16)mode, &cred, &new_ino);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }
    fi->fh = (uint64_t)new_ino;
    return 0;
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
    nyfs_cred_t cred;
    nyfs_ino_t ino;
    nyfs_status_t status;

    nyfs_cred_from_context(&cred);

    status = nyfs_resolve(path, &cred, &ino);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    /* 실제 권한 검사는 각 read/write 호출에서 nyfs_check_access() 가 하므로
     * 여기서는 파일이 있고 접근 가능한지만 확인한다. fh 에 inode 번호를
     * 담아 read/write 마다 경로를 다시 조회하지 않게 한다. */
    fi->fh = (uint64_t)ino;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    nyfs_cred_t cred;
    nyfs_ino_t ino;
    usize got;
    nyfs_status_t status;

    nyfs_cred_from_context(&cred);
    ino = (nyfs_ino_t)fi->fh;
    if (ino == NYFS_INO_NONE) {
        nyfs_status_t s2 = nyfs_resolve(path, &cred, &ino);
        if (NSTATUS_IS_ERR(s2)) {
            return nyfs_status_to_errno(s2);
        }
    }

    status = nyfs_op_read(&g_vol, ino, &cred, (u64)offset, buf, size, &got);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }
    return (int)got;
}

static int op_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    nyfs_cred_t cred;
    nyfs_ino_t ino;
    usize written;
    nyfs_status_t status;

    nyfs_cred_from_context(&cred);
    ino = (nyfs_ino_t)fi->fh;
    if (ino == NYFS_INO_NONE) {
        nyfs_status_t s2 = nyfs_resolve(path, &cred, &ino);
        if (NSTATUS_IS_ERR(s2)) {
            return nyfs_status_to_errno(s2);
        }
    }

    status = nyfs_op_write(&g_vol, ino, &cred, (u64)offset, buf, size, &written);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    (void)nyfs_op_setattr(&g_vol, ino, &cred, 0, 0, 0, 0, 0, nyfs_now_ns(), NYFS_TIME_KEEP);
    return (int)written;
}

static int op_unlink(const char *path)
{
    nyfs_cred_t cred;
    nyfs_ino_t parent_ino;
    char name[NYFS_NAME_MAX + 1U];
    nyfs_status_t status;

    nyfs_cred_from_context(&cred);

    status = nyfs_path_lookup_parent(&g_vol, path, &cred, &parent_ino, name, sizeof(name));
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    status = nyfs_op_remove(&g_vol, parent_ino, name, &cred, nfalse);
    return nyfs_status_to_errno(status);
}

static int op_rmdir(const char *path)
{
    nyfs_cred_t cred;
    nyfs_ino_t parent_ino;
    char name[NYFS_NAME_MAX + 1U];
    nyfs_status_t status;

    nyfs_cred_from_context(&cred);

    status = nyfs_path_lookup_parent(&g_vol, path, &cred, &parent_ino, name, sizeof(name));
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    status = nyfs_op_remove(&g_vol, parent_ino, name, &cred, ntrue);
    return nyfs_status_to_errno(status);
}

static int op_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    nyfs_cred_t cred;
    nyfs_ino_t ino;
    nyfs_status_t status;

    nyfs_cred_from_context(&cred);

    if (fi) {
        ino = (nyfs_ino_t)fi->fh;
    } else {
        status = nyfs_resolve(path, &cred, &ino);
        if (NSTATUS_IS_ERR(status)) {
            return nyfs_status_to_errno(status);
        }
    }

    status = nyfs_op_truncate(&g_vol, ino, &cred, (u64)size);
    return nyfs_status_to_errno(status);
}

static int op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    nyfs_cred_t cred;
    nyfs_ino_t ino;
    nyfs_status_t status;

    (void)fi;
    nyfs_cred_from_context(&cred);

    status = nyfs_resolve(path, &cred, &ino);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    status = nyfs_op_setattr(&g_vol, ino, &cred, 1, (u16)mode, 0, 0, 0, NYFS_TIME_KEEP, NYFS_TIME_KEEP);
    return nyfs_status_to_errno(status);
}

static int op_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    nyfs_cred_t cred;
    nyfs_ino_t ino;
    nyfs_status_t status;

    (void)fi;
    nyfs_cred_from_context(&cred);

    status = nyfs_resolve(path, &cred, &ino);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    status = nyfs_op_setattr(&g_vol, ino, &cred, 0, 0, 1, (u32)uid, (u32)gid, NYFS_TIME_KEEP, NYFS_TIME_KEEP);
    return nyfs_status_to_errno(status);
}

static int op_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
    nyfs_cred_t cred;
    nyfs_ino_t ino;
    nyfs_status_t status;
    u64 mtime_ns, atime_ns;

    (void)fi;
    nyfs_cred_from_context(&cred);

    status = nyfs_resolve(path, &cred, &ino);
    if (NSTATUS_IS_ERR(status)) {
        return nyfs_status_to_errno(status);
    }

    atime_ns = (u64)tv[0].tv_sec * 1000000000ULL + (u64)tv[0].tv_nsec;
    mtime_ns = (u64)tv[1].tv_sec * 1000000000ULL + (u64)tv[1].tv_nsec;

    status = nyfs_op_setattr(&g_vol, ino, &cred, 0, 0, 0, 0, 0, mtime_ns, atime_ns);
    return nyfs_status_to_errno(status);
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return 0;
}

static int op_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize = g_vol.block_size;
    stbuf->f_frsize = g_vol.block_size;
    stbuf->f_blocks = g_vol.sb.total_blocks;
    stbuf->f_bfree = g_vol.sb.free_blocks;
    stbuf->f_bavail = g_vol.sb.free_blocks;
    stbuf->f_files = g_vol.sb.inode_count;
    stbuf->f_ffree = g_vol.sb.free_inode_count;
    stbuf->f_namemax = NYFS_NAME_MAX;
    return 0;
}

static const struct fuse_operations nyfs_fuse_ops = {
    .getattr  = op_getattr,
    .readdir  = op_readdir,
    .mkdir    = op_mkdir,
    .create   = op_create,
    .open     = op_open,
    .read     = op_read,
    .write    = op_write,
    .unlink   = op_unlink,
    .rmdir    = op_rmdir,
    .truncate = op_truncate,
    .chmod    = op_chmod,
    .chown    = op_chown,
    .utimens  = op_utimens,
    .release  = op_release,
    .statfs   = op_statfs
};

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    fprintf(stderr, "사용법: %s <이미지파일 또는 블록장치> <마운트포인트> [FUSE 옵션...]\n", prog);
    fprintf(stderr, "  예) %s disk.img /mnt/nyfs -f\n", prog);
}

int main(int argc, char **argv)
{
    const char *image_path;
    int fd;
    nyfs_status_t status;
    struct fuse_args args;
    char **fuse_argv;
    int fuse_argc;
    int i;
    int rc;

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    image_path = argv[1];

    fd = open(image_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "%s: %s 열기 실패: %s\n", argv[0], image_path, strerror(errno));
        return 1;
    }

    status = nyfs_volume_open(&g_vol, fd, ntrue);
    if (NSTATUS_IS_ERR(status)) {
        fprintf(stderr, "%s: %s 마운트 실패(코드 %d) - mkfs.nyfs 로 먼저 포맷했는지 확인하세요\n",
                argv[0], image_path, (int)status);
        close(fd);
        return 1;
    }

    /* argv[0]=프로그램명, argv[1]=이미지, argv[2]=마운트포인트, argv[3..]=FUSE 옵션
     * -> FUSE 에는 "프로그램명, 마운트포인트, 옵션..." 형태로 넘긴다. */
    fuse_argc = argc - 1;
    fuse_argv = malloc((size_t)(fuse_argc + 1) * sizeof(char *));
    if (!fuse_argv) {
        fprintf(stderr, "%s: 메모리 부족\n", argv[0]);
        close(fd);
        return 1;
    }
    fuse_argv[0] = argv[0];
    for (i = 2; i < argc; i++) {
        fuse_argv[i - 1] = argv[i];
    }
    fuse_argv[fuse_argc] = NULL;

    args.argc = fuse_argc;
    args.argv = fuse_argv;
    args.allocated = 0;

    rc = fuse_main(args.argc, args.argv, &nyfs_fuse_ops, NULL);

    free(fuse_argv);
    nyfs_volume_close(&g_vol);
    close(fd);

    return rc;
}
