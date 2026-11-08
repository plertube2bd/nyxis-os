/*
 * sys/stat.h - 파일 정보, mkdir, chmod, umask
 *
 * 종류, 크기, 권한 비트(0777), 소유자(st_uid/st_gid)는 커널(NxStat/NxStatPath)이 알려 준 그대로다.
 * 시각과 inode 번호는 0 이다. setuid/setgid/sticky 비트는 없다 (chmod 는 0777 만 받는다).
 * chmod 는 소유자 또는 root 만 할 수 있다. ACL 이 붙은 항목은 ACL 이 권한을 정한다 (모드 비트는 정보로만 남는다).
 */
#ifndef _NL_SYS_STAT_H
#define _NL_SYS_STAT_H

#include <sys/types.h>

struct stat {
    dev_t   st_dev;
    ino_t   st_ino;
    mode_t  st_mode;
    nlink_t st_nlink;
    uid_t   st_uid;
    gid_t   st_gid;
    dev_t   st_rdev;
    off_t   st_size;
    long    st_blksize;
    long    st_blocks;
    time_t  st_atime;
    time_t  st_mtime;
    time_t  st_ctime;
};

#define S_IFMT   0170000
#define S_IFIFO  0010000    /* 채널 */
#define S_IFCHR  0020000    /* 콘솔, 문자 장치 */
#define S_IFDIR  0040000
#define S_IFBLK  0060000    /* 블록 장치 */
#define S_IFREG  0100000

#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
mode_t umask(mode_t mask);

#endif /* _NL_SYS_STAT_H */
