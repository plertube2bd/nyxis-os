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
    return auth_tests();
}
