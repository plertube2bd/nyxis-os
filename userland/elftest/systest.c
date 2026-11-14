/*
 * systest.c - elftest 의 시스템 콜 종합 검사 (파일/이름공간, 장치, 채널/NxPoll, 인증/신원)
 *
 * elftest "main" 역할(커널 selftest 가 root 로 실행)에서 elftest_system_tests() 를 부른다.
 * 실패하면 실패한 검사 번호(100~499)를 돌려준다. 일반 사용자 쪽 검사는 자기 자신을 "user" 역할로,
 * 인증으로 얻은 test(uid 1000) 토큰을 써서 실행해 확인한다 (그 자식의 실패 코드 n 은 450+n 으로 보고).
 *
 * 전제: SELFTEST 이미지 (tools/mkusers.py --test 로 만든 root/test 계정과 roi:/test_password).
 */
#include "nyx.h"

int elftest_system_tests(void);
int elftest_user_mode(void);
int elftest_ioport_mode(void);

#define CHECK(cond, code) do { if (!(cond)) return (code); } while (0)

static long spawn_self(const char *mode, long token)
{
    const char *argv[3];

    argv[0] = "elftest";
    argv[1] = mode;
    argv[2] = 0;
    return nx_spawn("app:/elftest.run", argv, 0, token);
}

static long wait_code(long h)
{
    int code = -999;

    if (h < 0)
        return -998;
    (void)nx_wait(h, NX_WAIT_INFINITE, &code);
    (void)nx_close(h);
    return code;
}

/* ---------------- 파일 / 이름공간 (root) ---------------- */

static int file_tests(void)
{
    struct nx_dirent de;
    char buf[64];
    long h;
    long r;
    unsigned long i;
    int found = 0;

    (void)nx_remove_node("tmp:/et/a.txt");
    (void)nx_remove_node("tmp:/et/b.txt");
    (void)nx_remove_node("tmp:/et");

    CHECK(nx_create_node("tmp:/et", NX_NODE_DIR, 0755) == 0, 100);
    CHECK(nx_create_node("tmp:/et/a.txt", NX_NODE_FILE, 0644) == 0, 101);
    CHECK(nx_create_node("tmp:/et/a.txt", NX_NODE_FILE, 0644) == NX_E_EXISTS, 102);
    h = nx_open("tmp:/et/a.txt", NX_O_WRITE);
    CHECK(h >= 0, 103);
    CHECK(nx_write(h, "hello", 5) == 5, 104);
    (void)nx_close(h);
    h = nx_open("tmp:/et/a.txt", NX_O_READ);
    CHECK(h >= 0, 105);
    r = nx_read(h, buf, sizeof(buf));
    (void)nx_close(h);
    CHECK(r == 5 && nx_memcmp(buf, "hello", 5) == 0, 106);
    CHECK(nx_read_dir("tmp:/et", 0, &de) == 0 && nx_strcmp(de.name, "a.txt") == 0 && de.type == NX_TYPE_FILE, 107);
    CHECK(nx_read_dir("tmp:/et", 1, &de) == NX_E_NOT_FOUND, 108);
    CHECK(nx_rename_node("tmp:/et/a.txt", "tmp:/et/b.txt") == 0, 109);
    CHECK(nx_open("tmp:/et/a.txt", NX_O_READ) < 0, 110);
    CHECK(nx_remove_node("tmp:/et") < 0, 111);                       /* 비어 있지 않다 */
    CHECK(nx_remove_node("tmp:/et/b.txt") == 0 && nx_remove_node("tmp:/et") == 0, 112);

    /* 노드 = 오브젝트 (ABI 2.5): 파일/디렉터리 구분 없이 내용과 자식을 함께 가진다. 종류는 "지금 자식이 있는지" */
    {
        struct nx_stat ost;
        struct nx_dirent ode;
        char obuf[8];
        long oh;

        CHECK(nx_create_node("tmp:/ob", NX_NODE_FILE, 0644) == 0 &&
              nx_create_node("tmp:/ob/kid", NX_NODE_FILE, 0644) == 0, 125);       /* 파일 아래에 자식 */
        oh = nx_open("tmp:/ob", NX_O_WRITE);                                      /* 자식이 있는 노드에 쓰기 */
        CHECK(oh >= 0 && nx_write(oh, "xyz", 3) == 3, 126);
        (void)nx_close(oh);
        oh = nx_open("tmp:/ob", NX_O_READ);
        CHECK(oh >= 0 && nx_stat(oh, &ost) == 0 && ost.type == NX_TYPE_DIR && ost.size == 3 &&
              nx_read(oh, obuf, sizeof(obuf)) == 3 && nx_memcmp(obuf, "xyz", 3) == 0, 127);
        (void)nx_close(oh);
        oh = nx_open("tmp:/ob", NX_O_WRITE | NX_O_TRUNC);                         /* 자르기: 내용만 */
        CHECK(oh >= 0 && nx_stat(oh, &ost) == 0 && ost.size == 0 &&
              nx_read_dir("tmp:/ob", 0, &ode) == 0 && nx_strcmp(ode.name, "kid") == 0 && ode.type == NX_TYPE_FILE &&
              nx_open("tmp:/ob", NX_O_READ | NX_O_TRUNC) == NX_E_INVALID_ARG, 128);
        (void)nx_close(oh);
        CHECK(nx_remove_node("tmp:/ob") < 0 && nx_remove_node("tmp:/ob/kid") == 0 &&   /* 자식이 있으면 못 지운다 */
              (oh = nx_open("tmp:/ob", NX_O_READ)) >= 0 && nx_stat(oh, &ost) == 0 && ost.type == NX_TYPE_FILE &&
              nx_close(oh) == 0 && nx_remove_node("tmp:/ob") == 0, 129);
    }

    /* root 의 이름공간 */
    h = nx_open("fs:/usr/hellowld.run", NX_O_READ);
    CHECK(h >= 0, 113);
    (void)nx_close(h);
    CHECK(nx_open("app:/../roi/users", NX_O_READ) == NX_E_PERMISSION, 114);   /* 별칭 밖으로 못 나간다 */
    CHECK(nx_open("roi:/users", NX_O_READ) == NX_E_PERMISSION, 115);          /* 커널 전용: root 도 못 연다 */
    CHECK(nx_open("fs:/roi/users", NX_O_READ) == NX_E_PERMISSION, 116);
    CHECK(nx_remove_node("roi:/users") == NX_E_PERMISSION, 117);
    CHECK(nx_rename_node("fs:/roi", "fs:/roi2") == NX_E_PERMISSION, 118);     /* 조상을 옮겨 우회하기 */
    for (i = 0; i < 16; i++) {
        if (nx_read_dir("roi:/", i, &de) != 0)
            break;
        if (nx_strcmp(de.name, "users") == 0)
            found = 1;
    }
    CHECK(found, 119);                                                        /* 이름은 보인다 */
    CHECK(nx_open("zz:/a", NX_O_READ) == NX_E_NOT_FOUND, 120);

    /* face:/ = fs:/home/<사용자> (root 는 fs:/home/root) */
    (void)nx_remove_node("face:/note");
    CHECK(nx_create_node("face:/note", NX_NODE_FILE, 0600) == 0, 121);
    h = nx_open("fs:/home/root/note", NX_O_READ);
    CHECK(h >= 0, 122);
    (void)nx_close(h);
    CHECK(nx_remove_node("face:/note") == 0, 123);
    CHECK(nx_create_node("tmp:/suid", NX_NODE_FILE, 04755) == NX_E_INVALID_ARG, 124);
    return 0;
}

/* ---------------- 장치 (root) ---------------- */

static int device_tests(void)
{
    static unsigned char buf[1024];
    static unsigned char back[1024];
    struct nx_stat st;
    struct nx_devinfo info;
    unsigned long addr = 0;
    unsigned long i;
    unsigned char lsr = 0;
    long h;
    long h2;

    /* dev:/rd0: 마운트 안 된 RAM 블록 장치 - 섹터 단위 raw 읽기/쓰기 */
    h = nx_open("dev:/rd0", NX_O_READ | NX_O_WRITE);
    CHECK(h >= 0, 200);
    CHECK(nx_stat(h, &st) == 0 && st.type == NX_TYPE_DEVICE && st.size == 1024UL * 1024UL, 201);
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = (unsigned char)(i * 13 + 7);
    CHECK(nx_seek(h, 4096, NX_SEEK_SET) == 4096, 202);
    CHECK(nx_write(h, buf, sizeof(buf)) == (long)sizeof(buf), 203);
    CHECK(nx_write(h, buf, 100) == NX_E_INVALID_ARG, 204);                     /* 섹터 단위가 아니다 */
    CHECK(nx_seek(h, 4096, NX_SEEK_SET) == 4096, 205);
    CHECK(nx_read(h, back, sizeof(back)) == (long)sizeof(back) && nx_memcmp(buf, back, sizeof(buf)) == 0, 206);
    CHECK(nx_open("dev:/rd0", NX_O_READ) == NX_E_BUSY, 207);                  /* 쓰기 열기는 독점 */
    (void)nx_close(h);

    /* dev:/rootdisk: fs:/ 가 올라간 장치 - 읽기만, 쓰기 열기는 Nbusy */
    h = nx_open("dev:/rootdisk", NX_O_READ);
    CHECK(h >= 0, 208);
    CHECK(nx_read(h, back, 512) == 512, 209);
    CHECK(nx_open("dev:/rootdisk", NX_O_READ | NX_O_WRITE) == NX_E_BUSY, 210);
    (void)nx_close(h);

    /* dev:/fb0: MMIO 직접 접근 (매핑 후 픽셀 쓰기) */
    h = nx_open("dev:/fb0", NX_O_READ | NX_O_WRITE);
    CHECK(h >= 0, 211);
    CHECK(nx_device_command(h, NX_DEVCMD_INFO, (unsigned long)&info) == 0 &&
          info.kind == NX_DEV_FRAMEBUFFER && info.width > 0 && info.pitch >= info.width * 4U, 212);
    CHECK(nx_device_map(h, &addr) == 0 && addr != 0, 213);
    {
        volatile unsigned int *px = (volatile unsigned int *)addr;

        px[0] = 0x0000FF00U;
        px[info.pitch / 4U * (info.height - 1U) + info.width - 1U] = 0x00FF0000U;   /* 마지막 픽셀 */
        CHECK(px[0] == 0x0000FF00U, 214);
    }
    h2 = nx_open("dev:/fb0", NX_O_READ | NX_O_WRITE);
    CHECK(h2 == NX_E_BUSY, 215);                                              /* 독점 */
    CHECK(nx_close(h) == 0, 216);

    /* dev:/com1: I/O 포트 직접 접근 (TSS I/O 비트맵) */
    h = nx_open("dev:/com1", NX_O_READ | NX_O_WRITE);
    CHECK(h >= 0, 217);
    CHECK(nx_device_command(h, NX_DEVCMD_INFO, (unsigned long)&info) == 0 &&
          info.kind == NX_DEV_STREAM && info.io_base == 0x3F8 && info.io_count == 8, 218);
    CHECK(nx_device_command(h, NX_DEVCMD_IOPERM, 1) == 0, 219);
    __asm__ volatile ("inb %1, %0" : "=a"(lsr) : "Nd"((unsigned short)0x3FD));   /* 허가 후에는 #GP 가 나지 않는다 */
    CHECK(lsr != 0xFF, 220);
    CHECK(nx_write(h, "elftest: COM1 written by a user process\r\n", 41) == 41, 221);
    CHECK(nx_close(h) == 0, 222);

    /* 허가 없이 포트에 접근하면 그 프로세스만 #GP 로 죽는다 */
    CHECK(wait_code(spawn_self("ioport", NX_NO_TOKEN)) == NX_EXIT_FAULT, 223);

    /* dev:/console: 프레임버퍼 + 키보드 (root 는 연다). fb0 과 배타적 */
    {
        static unsigned char font[95 * 16 * 16];
        struct nx_fontinfo fi;
        struct nx_pollfd pf;

        h = nx_open("dev:/console", NX_O_READ | NX_O_WRITE);
        CHECK(h >= 0, 230);
        CHECK(nx_device_command(h, NX_DEVCMD_INFO, (unsigned long)&info) == 0 &&
              info.kind == NX_DEV_CONSOLE && info.width > 0 &&
              (info.format == NX_FB_FORMAT_XRGB || info.format == NX_FB_FORMAT_XBGR), 231);
        CHECK(nx_open("dev:/fb0", NX_O_READ | NX_O_WRITE) == NX_E_BUSY &&
              nx_open("dev:/console", NX_O_READ | NX_O_WRITE) == NX_E_BUSY, 232);
        nx_memset(&fi, 0, sizeof(fi));
        CHECK(nx_device_command(h, NX_DEVCMD_CONSOLE_FONT, (unsigned long)&fi) == 0 &&
              fi.first == 32 && fi.count == 95 && fi.cell_w > 0 && fi.cell_h > 0 &&
              fi.count * fi.cell_w * fi.cell_h <= sizeof(font), 233);
        fi.buf = (unsigned long)font;
        fi.cap = 10;
        CHECK(nx_device_command(h, NX_DEVCMD_CONSOLE_FONT, (unsigned long)&fi) == NX_E_BUF_OVERFLOW, 234);
        fi.cap = sizeof(font);
        CHECK(nx_device_command(h, NX_DEVCMD_CONSOLE_FONT, (unsigned long)&fi) == 0, 235);
        {
            /* 'M'(0x4D) 에는 칠한 픽셀이 있고, ' ' 는 비어 있다 */
            unsigned long cell = fi.cell_w * fi.cell_h;
            unsigned long k;
            int inked = 0;
            int blank = 1;

            for (k = 0; k < cell; k++) {
                inked |= font[('M' - 32) * cell + k] != 0;
                blank &= font[k] == 0;
            }
            CHECK(inked && blank, 236);
        }
        CHECK(nx_device_map(h, &addr) == 0 && addr != 0, 237);
        pf.handle = (unsigned long)h;
        pf.events = NX_POLL_IN;
        pf.revents = 0;
        CHECK(nx_poll(&pf, 1, 0) == 0, 238);                              /* 키 이벤트 없음 */
        CHECK(nx_read(h, buf, 4) == NX_E_INVALID_ARG, 239);                 /* 이벤트 하나(8)보다 작다 */
        CHECK(nx_close(h) == 0, 240);
        h = nx_open("dev:/fb0", NX_O_READ | NX_O_WRITE);                    /* 콘솔로 돌아왔다 */
        CHECK(h >= 0 && nx_close(h) == 0, 241);
    }
    return 0;
}

/* ---------------- 공유 메모리 / 핸들 보내기 (root) ---------------- */

static long send_handle(long ch, const char *msg, long h)
{
    return nx_syscall(NX_SYS_CHANNEL_SEND_HANDLE, (unsigned long)ch, (unsigned long)msg, nx_strlen(msg), (unsigned long)h, 0);
}

static long recv_ex(long ch, char *buf, unsigned long cap, struct nx_msginfo_ex *info)
{
    return nx_syscall(NX_SYS_CHANNEL_RECEIVE_EX, (unsigned long)ch, (unsigned long)buf, cap, (unsigned long)info, 0);
}

static int shm_tests(void)
{
    struct nx_stat st;
    struct nx_msginfo_ex mi;
    struct nx_msginfo mi0;
    unsigned long a1 = 0;
    unsigned long a2 = 0;
    unsigned long p[2];
    char buf[64];
    long h;
    long ro;
    long rx;
    long tx;
    long got;

    /* 만들기, 매핑, 읽기 전용 사본이 같은 프레임을 본다 */
    h = nx_syscall(NX_SYS_SHM_CREATE, 10000, 0, 0, 0, 0);
    CHECK(h >= 0 && nx_stat(h, &st) == 0 && st.type == NX_TYPE_SHM && st.size == 12288, 740);
    CHECK(nx_syscall(NX_SYS_SHM_MAP, (unsigned long)h, (unsigned long)&a1, 0, 0, 0) == 0 && a1 != 0, 741);
    CHECK(((volatile unsigned char *)a1)[0] == 0 && ((volatile unsigned char *)a1)[12287] == 0, 742);   /* 0 으로 시작 */
    ((volatile unsigned int *)a1)[5] = 0xC0FFEEU;
    ro = nx_dup(h, NX_RIGHT_READ | NX_RIGHT_STAT | NX_RIGHT_DUP);
    CHECK(ro >= 0 && nx_syscall(NX_SYS_SHM_MAP, (unsigned long)ro, (unsigned long)&a2, 0, 0, 0) == 0 && a2 != a1 &&
          ((volatile unsigned int *)a2)[5] == 0xC0FFEEU, 743);
    CHECK(nx_close(h) == 0 && ((volatile unsigned int *)a2)[5] == 0xC0FFEEU, 744);   /* 사본이 살아 있는 동안은 남는다 */
    CHECK(nx_syscall(NX_SYS_SHM_CREATE, NX_SHM_MAX + 1, 0, 0, 0, 0) == NX_E_INVALID_ARG &&
          nx_syscall(NX_SYS_SHM_CREATE, 0, 0, 0, 0, 0) == NX_E_INVALID_ARG, 745);

    /* 채널로 핸들 보내기: 받은 쪽의 새 핸들이 같은 객체 */
    rx = nx_channel_create("svc:/sys/elftest-h", 0);
    tx = nx_open("svc:/sys/elftest-h", NX_O_WRITE);
    CHECK(rx >= 0 && tx >= 0 && send_handle(tx, "shm", ro) == 0, 746);
    nx_memset(&mi, 0, sizeof(mi));
    got = recv_ex(rx, buf, sizeof(buf), &mi);
    CHECK(got == 3 && mi.handle != NX_HANDLE_INVALID_VALUE && (long)mi.handle != ro, 747);
    a1 = 0;
    CHECK(nx_syscall(NX_SYS_SHM_MAP, mi.handle, (unsigned long)&a1, 0, 0, 0) == 0 &&
          ((volatile unsigned int *)a1)[5] == 0xC0FFEEU, 748);
    CHECK(nx_close((long)mi.handle) == 0 && nx_close(ro) == 0, 749);

    /* 파이프 끝을 보내고 받은 핸들로 쓴다 */
    CHECK(nx_syscall(NX_SYS_PIPE_CREATE, (unsigned long)p, 0, 0, 0, 0) == 0, 750);
    CHECK(send_handle(tx, "w", (long)p[1]) == 0 && nx_close((long)p[1]) == 0, 751);
    got = recv_ex(rx, buf, sizeof(buf), &mi);
    CHECK(got == 1 && mi.handle != NX_HANDLE_INVALID_VALUE && nx_write((long)mi.handle, "hi", 2) == 2 &&
          nx_read((long)p[0], buf, sizeof(buf)) == 2 && buf[0] == 'h', 752);
    CHECK(nx_close((long)mi.handle) == 0 && nx_read((long)p[0], buf, sizeof(buf)) == 0, 753);   /* 마지막 쓰기 끝: EOF */
    CHECK(nx_close((long)p[0]) == 0, 754);

    /* 핸들 없이 받으면(NxChannelReceive) 붙은 핸들은 닫힌다 / 받지 않은 채 채널이 사라져도 닫힌다 */
    CHECK(nx_syscall(NX_SYS_PIPE_CREATE, (unsigned long)p, 0, 0, 0, 0) == 0, 755);
    CHECK(send_handle(tx, "w", (long)p[1]) == 0 && send_handle(tx, "w", (long)p[1]) == 0 &&
          nx_close((long)p[1]) == 0, 756);
    CHECK(nx_channel_receive(rx, buf, sizeof(buf), &mi0, 0) == 1, 757);
    CHECK(nx_close(rx) == 0 && nx_read((long)p[0], buf, sizeof(buf)) == 0, 758);    /* 두 사본 모두 닫혔다: EOF */
    CHECK(nx_close((long)p[0]) == 0 && nx_close(tx) == 0, 759);

    /* 보낼 수 없는 종류 */
    rx = nx_channel_create("svc:/sys/elftest-h", 0);
    tx = nx_open("svc:/sys/elftest-h", NX_O_WRITE);
    CHECK(send_handle(tx, "rx", rx) == NX_E_UNSUPPORTED, 760);                      /* 받는 끝 */
    CHECK(nx_close(tx) == 0 && nx_close(rx) == 0, 761);
    return 0;
}

/* ---------------- 의사 터미널 (root) ---------------- */

static long pty_create(long h[2])
{
    unsigned long out[2];
    long r = nx_syscall(NX_SYS_PTY_CREATE, (unsigned long)out, 0, 0, 0, 0);

    h[0] = (long)out[0];
    h[1] = (long)out[1];
    return r;
}

/* 주 끝에서 정확히 want 가 나오는지 (조각으로 와도 모은다) */
static int master_says(long m, const char *want)
{
    char got[128];
    unsigned long n = nx_strlen(want);
    unsigned long have = 0;
    struct nx_pollfd pf;

    while (have < n) {
        long r;

        pf.handle = (unsigned long)m;
        pf.events = NX_POLL_IN;
        pf.revents = 0;
        if (nx_poll(&pf, 1, 1000) != 1)
            return 0;
        r = nx_read(m, got + have, sizeof(got) - have);
        if (r <= 0)
            return 0;
        have += (unsigned long)r;
    }
    return have == n && nx_memcmp(got, want, n) == 0;
}

static int pty_tests(void)
{
    struct nx_stat st;
    struct nx_winsize ws;
    struct nx_pollfd pf;
    char buf[64];
    long h[2];

    CHECK(pty_create(h) == 0 && h[0] >= 0 && h[1] >= 0, 700);
    CHECK(nx_stat(h[0], &st) == 0 && st.type == NX_TYPE_PTY, 701);
    CHECK(nx_stat(h[1], &st) == 0 && st.type == NX_TYPE_CONSOLE, 702);

    /* cooked: 주 끝에 친 줄이 종속 끝에서 한 줄로 읽히고, 에코는 주 끝으로 */
    CHECK(nx_write(h[0], "ab\r", 3) == 3, 703);
    CHECK(nx_read(h[1], buf, sizeof(buf)) == 3 && nx_memcmp(buf, "ab\n", 3) == 0, 704);
    CHECK(master_says(h[0], "ab\r\n"), 705);
    /* 줄 편집: Ctrl-U 로 지우고 다시 */
    CHECK(nx_write(h[0], "xy\025z\r", 5) == 5, 706);
    CHECK(nx_read(h[1], buf, sizeof(buf)) == 2 && nx_memcmp(buf, "z\n", 2) == 0, 707);
    CHECK(master_says(h[0], "xy\b\b  \b\bz\r\n"), 708);
    /* Ctrl-D: 빈 줄이면 EOF */
    CHECK(nx_write(h[0], "\004", 1) == 1 && nx_read(h[1], buf, sizeof(buf)) == 0, 709);

    /* 종속 끝 출력은 걸러지지 않고 주 끝으로 */
    CHECK(nx_write(h[1], "\033[1mok", 6) == 6 && master_says(h[0], "\033[1mok"), 710);
    pf.handle = (unsigned long)h[0];
    pf.events = NX_POLL_IN;
    pf.revents = 0;
    CHECK(nx_poll(&pf, 1, 0) == 0, 711);                                  /* 더 읽을 것이 없다 */

    /* 창 크기: 주 끝이 정하고 종속 끝이 읽는다. 콘솔(stdin)에는 정할 수 없다 */
    ws.rows = 30;
    ws.cols = 100;
    ws.xpixel = 0;
    ws.ypixel = 0;
    CHECK(nx_device_command(h[0], NX_DEVCMD_TTY_SET_WINSIZE, (unsigned long)&ws) == 0, 712);
    nx_memset(&ws, 0, sizeof(ws));
    CHECK(nx_device_command(h[1], NX_DEVCMD_TTY_WINSIZE, (unsigned long)&ws) == 0 && ws.rows == 30 && ws.cols == 100, 713);
    CHECK(nx_device_command(0, NX_DEVCMD_TTY_SET_WINSIZE, (unsigned long)&ws) == NX_E_UNSUPPORTED, 714);

    /* raw: 바이트 그대로, 에코 없음, Ctrl-C 도 바이트 */
    CHECK(nx_device_command(h[1], NX_DEVCMD_TTY_MODE, NX_TTY_MODE_RAW) == 0, 715);
    CHECK(nx_write(h[0], "\003q\r", 3) == 3, 716);
    CHECK(nx_read(h[1], buf, sizeof(buf)) == 3 && buf[0] == 3 && buf[1] == 'q' && buf[2] == '\r', 717);
    CHECK(nx_poll(&pf, 1, 0) == 0, 718);                                  /* 에코 없음 */
    CHECK(nx_device_command(h[1], NX_DEVCMD_TTY_MODE, NX_TTY_MODE_COOKED) == 0, 719);

    /* 주 끝을 닫으면 끊김: 종속 끝 읽기는 EOF, 쓰기는 Ndisconnected, poll 은 HUP */
    CHECK(nx_close(h[0]) == 0, 720);
    CHECK(nx_read(h[1], buf, sizeof(buf)) == 0, 721);
    CHECK(nx_write(h[1], "x", 1) == NX_E_DISCONNECTED, 722);
    pf.handle = (unsigned long)h[1];
    pf.revents = 0;
    CHECK(nx_poll(&pf, 1, 0) == 1 && (pf.revents & NX_POLL_HUP), 723);
    CHECK(nx_close(h[1]) == 0, 724);

    /* NxProcessList: 자기 자신이 실행 파일 이름으로 보인다 (argv[0] 이 아니라 경로에서) */
    {
        static struct nx_procent pe[32];
        struct nx_procinfo me;
        long n = nx_syscall(NX_SYS_PROCESS_LIST, (unsigned long)pe, 32, 0, 0, 0);
        long k;
        int seen = 0;
        int idle = 0;

        CHECK(n >= 2 && nx_process_info(NX_PID_SELF, &me) == 0, 730);   /* 자체 점검 중에는 idle 과 elftest 뿐일 수 있다 */
        for (k = 0; k < n && k < 32; k++) {
            if (pe[k].pid == me.pid && nx_strcmp(pe[k].name, "elftest") == 0 && pe[k].uid == 0 &&
                (pe[k].flags & NX_PROCENT_USER) && pe[k].mem_kib > 0)
                seen = 1;
            if (pe[k].pid == 0 && nx_strcmp(pe[k].name, "idle") == 0 && !(pe[k].flags & NX_PROCENT_USER))
                idle = 1;
        }
        CHECK(seen && idle, 731);
        CHECK(nx_syscall(NX_SYS_PROCESS_LIST, 0, 0, 0, 0, 0) == n, 732);            /* max 0: 수만 */
        CHECK(nx_syscall(NX_SYS_SIGNAL_SEND_PID, me.pid, 0, 0, 0, 0) == 0, 733);     /* 확인만 */
        CHECK(nx_syscall(NX_SYS_SIGNAL_SEND_PID, 0, 0, 0, 0, 0) == NX_E_PERMISSION, 734);   /* idle */
        CHECK(nx_syscall(NX_SYS_SIGNAL_SEND_PID, 0x7FFFFFF0UL, 0, 0, 0, 0) == NX_E_NOT_FOUND, 735);
        /* 보호 프로세스는 보호 프로세스(init)만 만든다: root 라도 elftest 는 아니다 */
        {
            const char *av[2];

            av[0] = "hellowld";
            av[1] = 0;
            CHECK(nx_spawn_ex("app:/hellowld.run", av, 0, NX_NO_TOKEN, NX_SPAWN_CRITICAL) == NX_E_PERMISSION, 736);
        }
    }

    /* 종속 끝이 모두 닫히면 주 끝은 남은 출력을 다 읽은 뒤 EOF */
    CHECK(pty_create(h) == 0, 725);
    CHECK(nx_write(h[1], "bye", 3) == 3 && nx_close(h[1]) == 0, 726);
    CHECK(nx_read(h[0], buf, sizeof(buf)) == 3 && nx_read(h[0], buf, sizeof(buf)) == 0, 727);
    pf.handle = (unsigned long)h[0];
    pf.revents = 0;
    CHECK(nx_poll(&pf, 1, 0) == 1 && (pf.revents & NX_POLL_HUP), 728);
    CHECK(nx_close(h[0]) == 0, 729);
    return 0;
}

/* ---------------- 채널 / NxPoll (root) ---------------- */

static int channel_tests(void)
{
    struct nx_pollfd pf;
    struct nx_msginfo info;
    struct nx_procinfo self;
    char buf[NX_CHAN_MSG_MAX];
    long rx;
    long tx;
    long h;
    unsigned long i;

    CHECK(nx_process_info((long)NX_PID_SELF, &self) == 0, 299);
    rx = nx_channel_create("svc:/sys/elftest", NX_CHAN_PUBLIC);
    CHECK(rx >= 0, 300);
    CHECK(nx_channel_create("svc:/sys/elftest", NX_CHAN_PUBLIC) == NX_E_EXISTS, 301);
    CHECK(nx_channel_create("svc:/sys", 0) == NX_E_INVALID_ARG, 302);
    tx = nx_open("svc:/sys/elftest", NX_O_WRITE);
    CHECK(tx >= 0, 303);
    CHECK(nx_open("svc:/sys/elftest", NX_O_READ) == NX_E_INVALID_ARG, 304);

    pf.handle = (unsigned long)rx;
    pf.events = NX_POLL_IN;
    pf.revents = 0;
    CHECK(nx_poll(&pf, 1, 0) == 0, 305);
    CHECK(nx_channel_send(tx, "ping", 4) == 0, 306);
    CHECK(nx_poll(&pf, 1, NX_WAIT_INFINITE) == 1 && (pf.revents & NX_POLL_IN), 307);
    CHECK(nx_channel_receive(rx, buf, sizeof(buf), &info, 0) == 4 && nx_memcmp(buf, "ping", 4) == 0 &&
          info.sender_uid == 0 && info.sender_pid == self.pid, 308);
    CHECK(nx_channel_receive(rx, buf, sizeof(buf), &info, 0) == NX_E_TIMEOUT, 309);
    CHECK(nx_channel_receive(rx, buf, sizeof(buf), &info, 30) == NX_E_TIMEOUT, 310);

    CHECK(nx_channel_send(tx, "abcdef", 6) == 0, 311);
    CHECK(nx_channel_receive(rx, buf, 2, &info, 0) == NX_E_BUF_OVERFLOW && info.length == 6, 312);
    CHECK(nx_channel_receive(rx, buf, sizeof(buf), &info, 0) == 6, 313);           /* 메시지는 남아 있었다 */

    for (i = 0; i < NX_CHAN_QUEUE_MAX; i++)
        CHECK(nx_channel_send(tx, "x", 1) == 0, 314);
    CHECK(nx_channel_send(tx, "x", 1) == NX_E_BUSY, 315);
    pf.handle = (unsigned long)tx;
    pf.events = NX_POLL_OUT;
    CHECK(nx_poll(&pf, 1, 0) == 0, 316);
    CHECK(nx_channel_receive(rx, buf, sizeof(buf), &info, 0) == 1, 317);
    CHECK(nx_poll(&pf, 1, 0) == 1 && (pf.revents & NX_POLL_OUT), 318);

    CHECK(nx_close(rx) == 0, 319);
    CHECK(nx_channel_send(tx, "x", 1) == NX_E_DISCONNECTED, 320);
    CHECK(nx_poll(&pf, 1, 0) == 1 && (pf.revents & NX_POLL_HUP), 321);
    (void)nx_close(tx);
    rx = nx_channel_create("svc:/sys/elftest", 0);                                /* 이름은 다시 쓸 수 있다 */
    CHECK(rx >= 0, 322);
    (void)nx_close(rx);

    /* 프로세스 종료도 NxPoll 로 기다린다 */
    h = spawn_self("child", NX_NO_TOKEN);
    CHECK(h >= 0, 323);
    pf.handle = (unsigned long)h;
    pf.events = NX_POLL_EXIT;
    CHECK(nx_poll(&pf, 1, NX_WAIT_INFINITE) == 1 && (pf.revents & NX_POLL_EXIT), 324);
    (void)nx_close(h);
    return 0;
}

/* ---------------- 레지스트리 (roi:/) / info:/ ---------------- */

/* path 의 값(끝 '\n' 제외)이 want 와 같은가 */
/* "주.부" (info:/abi 의 기대값, nyx_abi.h 에서 만든다: 버전을 올릴 때 이 검사를 고칠 필요가 없도록) */
static const char *abi_string(void)
{
    static char out[24];
    unsigned long parts[2];
    unsigned long n = 0;
    int i;

    parts[0] = NX_ABI_MAJOR;
    parts[1] = NX_ABI_MINOR;
    for (i = 0; i < 2; i++) {
        char tmp[12];
        unsigned long v = parts[i];
        int k = 0;

        do {
            tmp[k++] = (char)('0' + v % 10UL);
            v /= 10UL;
        } while (v);
        while (k > 0)
            out[n++] = tmp[--k];
        if (i == 0)
            out[n++] = '.';
    }
    out[n] = '\0';
    return out;
}

static int value_is(const char *path, const char *want)
{
    char buf[128];

    return nx_read_value(path, buf, sizeof(buf)) >= 0 && nx_strcmp(buf, want) == 0;
}

/* 파일을 새로 만들어(있으면 지우고) data 를 쓴다 */
static int put_file(const char *path, const char *data, unsigned long mode)
{
    long h;
    long n;

    (void)nx_remove_node(path);
    if (nx_create_node(path, NX_NODE_FILE, mode) != 0)
        return 0;
    h = nx_open(path, NX_O_WRITE);
    if (h < 0)
        return 0;
    n = nx_write(h, data, nx_strlen(data));
    (void)nx_close(h);
    return n == (long)nx_strlen(data);
}

static unsigned long parse_ul(const char *s)
{
    unsigned long v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned long)(*s++ - '0');
    return v;
}

/* root(uid 0)로 실행. 실패 코드 500~ */
static int registry_tests(void)
{
    struct nx_dirent de;
    struct nx_stat st;
    char buf[64];
    char buf2[64];
    unsigned long t0;
    unsigned long t1;
    unsigned long total;
    unsigned long avail;
    long h;
    long h2;

    /* info:/ 값 */
    CHECK(value_is("roi:/hostname", "nyxis") && value_is("info:/hostname", "nyxis"), 500);
    CHECK(value_is("info:/version", "0.1") && value_is("info:/abi", abi_string()), 501);
    CHECK(value_is("roi:/timezone", "Europe/London") && value_is("info:/timezone", "Europe/London"), 536);
    CHECK(value_is("info:/utc_offset", "+00:00"), 541);
    CHECK(nx_open("info:/hostname", NX_O_WRITE) == NX_E_PERMISSION &&
          nx_open("info:/hostname", NX_O_READ | NX_O_WRITE) == NX_E_PERMISSION, 502);
    CHECK(nx_create_node("info:/x", NX_NODE_FILE, 0644) == NX_E_PERMISSION &&
          nx_remove_node("info:/hostname") == NX_E_PERMISSION &&
          nx_rename_node("info:/hostname", "tmp:/h") == NX_E_PERMISSION, 503);
    CHECK(nx_open("info:/nope", NX_O_READ) == NX_E_NOT_FOUND && nx_open("info:/memory/nope", NX_O_READ) == NX_E_NOT_FOUND, 504);

    /* 나열: info:/ 와 info:/memory */
    CHECK(nx_read_dir("info:/", 0, &de) == 0 && nx_strcmp(de.name, "hostname") == 0 && de.type == NX_TYPE_FILE, 505);
    CHECK(nx_read_dir("info:/", 6, &de) == 0 && nx_strcmp(de.name, "memory") == 0 && de.type == NX_TYPE_DIR, 506);
    CHECK(nx_read_dir("info:/", 7, &de) == NX_E_NOT_FOUND, 507);
    CHECK(nx_read_dir("info:/memory", 0, &de) == 0 && nx_strcmp(de.name, "total_kib") == 0 &&
          nx_read_dir("info:/memory", 1, &de) == 0 && nx_strcmp(de.name, "free_kib") == 0 &&
          nx_read_dir("info:/memory", 2, &de) == 0 && nx_strcmp(de.name, "ram_kib") == 0 &&
          nx_read_dir("info:/memory", 3, &de) == 0 && nx_strcmp(de.name, "reserved_kib") == 0 &&
          nx_read_dir("info:/memory", 4, &de) == NX_E_NOT_FOUND, 508);
    CHECK(nx_read_dir("info:/hostname", 0, &de) == NX_E_INVALID_ARG, 509);

    /* 스냅샷: 열 때 만든 값은 그대로, 다시 열면 새 값 */
    CHECK(nx_read_value("info:/uptime_ms", buf, sizeof(buf)) > 0, 510);
    t0 = parse_ul(buf);
    h = nx_open("info:/uptime_ms", NX_O_READ);
    CHECK(h >= 0, 511);
    (void)nx_sleep(30);
    CHECK(nx_read(h, buf2, sizeof(buf2)) > 0, 512);
    t1 = parse_ul(buf2);
    CHECK(t1 >= t0 && t1 - t0 < 30, 513);                                     /* 연 시점의 값 */
    CHECK(nx_read_value("info:/uptime_ms", buf, sizeof(buf)) > 0 && parse_ul(buf) >= t0 + 30, 514);
    CHECK(nx_stat(h, &st) == 0 && st.type == NX_TYPE_FILE && st.size > 0, 515);
    CHECK(nx_seek(h, 0, NX_SEEK_SET) == 0, 516);
    h2 = nx_dup(h, NX_RIGHT_ALL);
    CHECK(h2 >= 0, 517);
    CHECK(nx_read(h2, buf, sizeof(buf)) == (long)st.size && nx_memcmp(buf, buf2, st.size) == 0, 518);   /* 같은 스냅샷 */
    CHECK(nx_read(h, buf, 1) == 1 && nx_read(h2, buf, 1) == 0, 519);         /* 오프셋은 따로 */
    CHECK(nx_write(h, "x", 1) == NX_E_PERMISSION, 520);
    (void)nx_close(h2);
    (void)nx_close(h);

    CHECK(nx_read_value("info:/memory/total_kib", buf, sizeof(buf)) > 0, 521);
    total = parse_ul(buf);
    CHECK(nx_read_value("info:/memory/free_kib", buf, sizeof(buf)) > 0, 522);
    avail = parse_ul(buf);
    CHECK(total > 0 && avail > 0 && avail <= total, 523);
    {
        unsigned long ram, rsv;

        CHECK(nx_read_value("info:/memory/ram_kib", buf, sizeof(buf)) > 0, 533);
        ram = parse_ul(buf);
        CHECK(nx_read_value("info:/memory/reserved_kib", buf, sizeof(buf)) > 0, 534);
        rsv = parse_ul(buf);
        CHECK(ram >= total && rsv == ram - total, 535);      /* total 이 그 사이에 바뀌지 않는다 (부팅 때 고정) */
    }

    h = nx_open("info:/memory", NX_O_READ);                                   /* 디렉터리도 열 수는 있다 */
    CHECK(h >= 0 && nx_stat(h, &st) == 0 && st.type == NX_TYPE_DIR && nx_read(h, buf, 1) == NX_E_INVALID_ARG, 524);
    (void)nx_close(h);

    /* roi:/hostname 은 원본: root 가 바꾸면 info:/ 에 반영, 형식이 틀리면 기본값 */
    CHECK(put_file("roi:/hostname", "testbox\n", 0644) && value_is("info:/hostname", "testbox"), 525);
    CHECK(put_file("roi:/hostname", "Bad_Host\n", 0644) && value_is("info:/hostname", "nyxis"), 526);
    CHECK(put_file("roi:/hostname", "-x", 0644) && value_is("info:/hostname", "nyxis"), 527);
    CHECK(put_file("roi:/hostname", "edge\n", 0600) && value_is("info:/hostname", "edge"), 528);   /* 모드와 무관 (커널 권한) */
    CHECK(nx_remove_node("roi:/hostname") == 0 && value_is("info:/hostname", "nyxis"), 529);
    CHECK(put_file("roi:/hostname", "nyxis\n", 0644) && value_is("info:/hostname", "nyxis"), 530);

    /* roi:/timezone 도 같은 규칙 (기본값 Europe/London) */
    CHECK(put_file("roi:/timezone", "Asia/Seoul\n", 0644) && value_is("info:/timezone", "Asia/Seoul"), 537);
    CHECK(value_is("info:/utc_offset", "+00:00"), 542);                   /* 이름이 바뀌어도 오프셋은 그대로 (계산 없음) */
    CHECK(put_file("roi:/timezone", "../etc\n", 0644) && value_is("info:/timezone", "Europe/London"), 538);
    CHECK(nx_remove_node("roi:/timezone") == 0 && value_is("info:/timezone", "Europe/London"), 539);
    CHECK(put_file("roi:/timezone", "Europe/London\n", 0644) && value_is("info:/timezone", "Europe/London"), 540);

    /* 틀과 사용자 공간 (빌드 때 mkrootfs 가 복사) */
    CHECK(value_is("roi:/default/shell", "app:/nysh.run") && value_is("roi:/1000/shell", "app:/nysh.run") &&
          value_is("roi:/0/shell", "app:/nysh.run"), 531);
    CHECK(nx_read_dir("roi:/default/app", 0, &de) == NX_E_NOT_FOUND, 532);  /* 빈 디렉터리 (.keep 은 안 들어간다) */
    return 0;
}

/* ---------------- 인증 / 토큰 / 신원 ---------------- */

static int read_test_password(char *pw, unsigned long max)
{
    char buf[256];
    long h = nx_open("roi:/test_password", NX_O_READ);
    long n;
    long i;
    long start;

    if (h < 0)
        return 0;
    n = nx_read(h, buf, sizeof(buf) - 1);
    (void)nx_close(h);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    for (start = 0; start < n; ) {
        long end = start;

        while (end < n && buf[end] != '\n')
            end++;
        if (end - start > 5 && nx_strncmp(buf + start, "test:", 5) == 0) {
            long len = end - start - 5;

            if ((unsigned long)len >= max)
                return 0;
            for (i = 0; i < len; i++)
                pw[i] = buf[start + 5 + i];
            pw[len] = '\0';
            nx_wipe(buf, sizeof(buf));
            return 1;
        }
        start = end + 1;
    }
    nx_wipe(buf, sizeof(buf));
    return 0;
}

static int auth_tests(void)
{
    char pw[64];
    char buf[64];
    struct nx_stat st;
    struct nx_msginfo info;
    struct nx_identity id;
    long tok;
    long priv;
    long pub;
    long code;
    long h;

    CHECK(read_test_password(pw, sizeof(pw)), 400);
    CHECK(nx_authenticate("test", "wrong-password") == NX_E_AUTH, 401);        /* 1초 지연 후 */
    CHECK(nx_authenticate("nobody", "x") == NX_E_AUTH, 402);                   /* 없는 사용자도 같은 오류 */
    tok = nx_authenticate("test", pw);
    nx_wipe(pw, sizeof(pw));
    CHECK(tok >= 0, 403);
    CHECK(nx_stat(tok, &st) == 0 && st.type == NX_TYPE_TOKEN, 404);
    CHECK(nx_dup(tok, NX_RIGHT_ALL) == NX_E_PERMISSION, 405);                  /* 일회용: DUP 권한이 없다 */
    CHECK(nx_token_identity(tok, &id) == 0 && id.uid == 1000 && id.gid == 1000 && nx_strcmp(id.name, "test") == 0, 414);
    CHECK(nx_stat(tok, &st) == 0, 415);                                        /* 조회는 토큰을 소모하지 않는다 */
    CHECK(nx_token_identity(NX_HANDLE_STDOUT, &id) == NX_E_INVALID_ARG, 416);  /* 토큰이 아니다 (0 = stdin = NX_NO_TOKEN) */
    CHECK(nx_identity(&id) == 0 && id.uid == 0, 417);                          /* 자신은 그대로 root */

    priv = nx_channel_create("svc:/sys/elftest-priv", 0);                     /* root 만 보낼 수 있음 */
    pub = nx_channel_create("svc:/sys/elftest-pub", NX_CHAN_PUBLIC);
    CHECK(priv >= 0 && pub >= 0, 406);

    h = spawn_self("user", tok);
    CHECK(h >= 0, 407);
    CHECK(nx_close(tok) == NX_E_INVALID_ARG, 408);                            /* 토큰은 소모되었다 */
    code = wait_code(h);
    if (code != 0)
        return (int)(450 + (code > 0 && code < 50 ? code : 49));

    CHECK(nx_channel_receive(pub, buf, sizeof(buf), &info, 0) == 9 && nx_memcmp(buf, "from-user", 9) == 0 &&
          info.sender_uid == 1000, 409);                                       /* 커널이 붙인 보낸 쪽 신원 */
    h = nx_open("fs:/home/test/hello.txt", NX_O_READ);                         /* 자식이 face:/ 에 쓴 것 */
    CHECK(h >= 0, 410);
    CHECK(nx_read(h, buf, sizeof(buf)) == 2 && buf[0] == 'h' && buf[1] == 'i', 411);
    (void)nx_close(h);
    CHECK(nx_remove_node("fs:/home/test/hello.txt") == 0, 412);
    CHECK(spawn_self("child", tok) < 0, 413);                                  /* 같은 토큰 재사용 불가 */

    (void)nx_close(priv);
    (void)nx_close(pub);
    return 0;
}

/* "user" 역할: test(uid 1000) 신원으로 실행된다. 실패 코드 1~49 */
int elftest_user_mode(void)
{
    struct nx_identity id;
    long h;

    CHECK(nx_identity(&id) == 0 && id.uid == 1000 && id.gid == 1000 && nx_strcmp(id.name, "test") == 0, 1);

    CHECK(nx_create_node("face:/hello.txt", NX_NODE_FILE, 0600) == 0, 2);     /* face:/ = fs:/home/test */
    h = nx_open("face:/hello.txt", NX_O_WRITE);
    CHECK(h >= 0 && nx_write(h, "hi", 2) == 2, 3);
    (void)nx_close(h);

    CHECK(nx_open("fs:/usr/hellowld.run", NX_O_READ) == NX_E_PERMISSION, 4);  /* fs:/ 는 root 만 */
    CHECK(nx_open("roi:/users", NX_O_READ) == NX_E_PERMISSION, 5);
    CHECK(nx_open("dev:/rd0", NX_O_READ) == NX_E_PERMISSION, 6);              /* 장치는 root 만 */
    CHECK(nx_open("face:/../root/note", NX_O_READ) == NX_E_PERMISSION, 7);    /* 남의 홈으로 못 나간다 */
    h = nx_open("app:/hellowld.run", NX_O_READ);
    CHECK(h >= 0, 8);
    (void)nx_close(h);
    CHECK(nx_create_node("app:/evil", NX_NODE_FILE, 0644) == NX_E_PERMISSION, 9);   /* fs:/usr 는 root 소유 0755 */
    CHECK(nx_create_node("tmp:/test-was-here", NX_NODE_FILE, 0644) == 0 &&
          nx_remove_node("tmp:/test-was-here") == 0, 10);

    CHECK(nx_channel_create("svc:/sys/x", 0) == NX_E_PERMISSION, 11);         /* 시스템 이름 사칭 불가 */
    CHECK(nx_channel_create("svc:/root/x", 0) == NX_E_PERMISSION, 12);        /* 남의 이름 아래 불가 */
    h = nx_channel_create("svc:/test/echo", 0);
    CHECK(h >= 0, 13);
    (void)nx_close(h);
    CHECK(nx_open("svc:/sys/elftest-priv", NX_O_WRITE) == NX_E_PERMISSION, 14);
    h = nx_open("svc:/sys/elftest-pub", NX_O_WRITE);
    CHECK(h >= 0 && nx_channel_send(h, "from-user", 9) == 0, 15);
    (void)nx_close(h);

    CHECK(nx_power_off() == NX_E_PERMISSION, 16);                             /* 끄는 건 root(init)만 */
    /* 화면/키보드는 콘솔 세션(로그인이 띄운 셸)과 root 만, 콘솔 세션은 root 만 만든다 */
    CHECK(nx_open("dev:/console", NX_O_READ | NX_O_WRITE) == NX_E_PERMISSION, 37);
    {
        const char *av[2];

        av[0] = "hellowld";
        av[1] = 0;
        CHECK(nx_spawn_ex("app:/hellowld.run", av, 0, NX_NO_TOKEN, NX_SPAWN_CONSOLE_SESSION) == NX_E_PERMISSION, 38);
    }
    /* 남(root)의 프로세스에는 pid 로 신호를 못 보낸다: 부모(root 의 elftest) */
    {
        struct nx_procinfo me;

        CHECK(nx_process_info(NX_PID_SELF, &me) == 0 &&
              nx_syscall(NX_SYS_SIGNAL_SEND_PID, me.parent_pid, 15, 0, 0, 0) == NX_E_PERMISSION, 39);
    }

    /* roi:/<자기 uid>/ 만 */
    {
        struct nx_dirent de;
        char buf[64];
        int saw_app = 0;
        int saw_shell = 0;
        unsigned long i;

        CHECK(value_is("roi:/1000/shell", "app:/nysh.run"), 17);                /* 관리자 값: 읽기 */
        CHECK(nx_open("roi:/1000/shell", NX_O_WRITE) == NX_E_PERMISSION, 18);  /* 0444 root: 못 고침 */
        CHECK(nx_remove_node("roi:/1000/shell") == NX_E_PERMISSION &&
              nx_rename_node("roi:/1000/shell", "roi:/1000/app/shell") == NX_E_PERMISSION, 19);
        CHECK(nx_create_node("roi:/1000/mine", NX_NODE_FILE, 0644) == NX_E_PERMISSION, 20);   /* 0500 */
        CHECK(put_file("roi:/1000/app/editor", "dark\n", 0600) && value_is("roi:/1000/app/editor", "dark"), 21);
        CHECK(nx_create_node("roi:/1000/app/sub", NX_NODE_DIR, 0700) == 0 && nx_remove_node("roi:/1000/app/sub") == 0, 22);
        CHECK(nx_remove_node("roi:/1000/app/editor") == 0, 23);
        for (i = 0; i < 8 && nx_read_dir("roi:/1000", i, &de) == 0; i++) {
            if (nx_strcmp(de.name, "app") == 0)     /* 자식이 없으면 파일로 보인다 (노드 = 오브젝트, ABI 2.5) */
                saw_app = 1;
            if (nx_strcmp(de.name, "shell") == 0)
                saw_shell = 1;
        }
        CHECK(saw_app && saw_shell, 24);
        CHECK(nx_open("roi:/hostname", NX_O_READ) == NX_E_PERMISSION &&
              nx_open("roi:/default/shell", NX_O_READ) == NX_E_PERMISSION &&
              nx_open("roi:/0/shell", NX_O_READ) == NX_E_PERMISSION, 25);
        CHECK(nx_read_dir("roi:/", 0, &de) == NX_E_PERMISSION &&
              nx_open("roi:/1000/../0/shell", NX_O_READ) == NX_E_PERMISSION &&
              nx_open("roi:/01000/shell", NX_O_READ) == NX_E_PERMISSION, 26);
        CHECK(nx_create_node("roi:/1001", NX_NODE_DIR, 0700) == NX_E_PERMISSION, 27);
        CHECK(value_is("info:/hostname", "nyxis") && nx_read_value("info:/uptime_ms", buf, sizeof(buf)) > 0, 28);
        CHECK(nx_open("info:/hostname", NX_O_WRITE) == NX_E_PERMISSION, 29);
    }

    /* 앱 오브젝트: app:/<이름>.run 은 ELF 를 내용으로, 기본 설정을 자식으로 가진 디렉터리 (rootfs/app/) */
    {
        struct nx_dirent de;
        struct nx_stat st;
        char buf[64];
        char elf[4];
        long ho = nx_open("app:/init.run", NX_O_READ);

        CHECK(ho >= 0 && nx_stat(ho, &st) == 0 && st.type == NX_TYPE_DIR && st.size > 4 &&
              nx_read(ho, elf, 4) == 4 && elf[0] == 0x7F && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F', 30);
        if (ho >= 0)
            (void)nx_close(ho);
        CHECK(value_is("app:/init.run/welcome", "Welcome to Nyxis OS!") &&
              nx_read_dir("app:/init.run", 0, &de) == 0 && nx_strcmp(de.name, "welcome") == 0, 31);
        /* 기본값은 root 소유: 사용자는 읽기만 */
        CHECK(nx_open("app:/nysh.run/mark", NX_O_WRITE) == NX_E_PERMISSION &&
              nx_create_node("app:/nysh.run/mine", NX_NODE_FILE, 0644) == NX_E_PERMISSION, 32);
        /* 사용자 값이 기본값보다 먼저, 지우면 기본값 */
        CHECK(nx_setting_get("nysh", "mark", buf, sizeof(buf)) == 1 && nx_strcmp(buf, "*") == 0, 33);
        CHECK(nx_create_node("roi:/1000/app/nysh", NX_NODE_DIR, 0700) == 0 &&
              put_file("roi:/1000/app/nysh/mark", ">\n", 0600) &&
              nx_setting_get("nysh", "mark", buf, sizeof(buf)) == 1 && nx_strcmp(buf, ">") == 0, 34);
        CHECK(nx_remove_node("roi:/1000/app/nysh/mark") == 0 && nx_remove_node("roi:/1000/app/nysh") == 0 &&
              nx_setting_get("nysh", "mark", buf, sizeof(buf)) == 1 && nx_strcmp(buf, "*") == 0, 35);
        CHECK(nx_setting_get("nysh", "../x", buf, sizeof(buf)) == NX_E_INVALID_ARG &&
              nx_setting_get("..", "mark", buf, sizeof(buf)) == NX_E_INVALID_ARG &&
              nx_setting_get("nysh", "nope", buf, sizeof(buf)) < 0, 36);
    }
    return 0;
}

/* "ioport" 역할: 허가 없이 포트를 읽는다 -> #GP 로 종료되어야 한다 (돌아오면 실패) */
int elftest_ioport_mode(void)
{
    unsigned char v;

    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"((unsigned short)0x3FD));
    (void)v;
    return 0;
}

int elftest_system_tests(void)
{
    int r;

    r = file_tests();
    if (r)
        return r;
    r = device_tests();
    if (r)
        return r;
    r = channel_tests();
    if (r)
        return r;
    r = pty_tests();
    if (r)
        return r;
    r = shm_tests();
    if (r)
        return r;
    r = registry_tests();
    if (r)
        return r;
    return auth_tests();
}
