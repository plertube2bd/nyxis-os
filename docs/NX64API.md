# nx64api

Nyxis OS용 C API. 헤더 하나(`<nx64.h>`)로 **전체 화면 TUI 앱**을 만들고, POSIX에 없는 Nyxis 기능(info:/, 신원, 앱 설정, 서비스 채널, 전원 끄기 요청, 화면/키 이벤트)을 다룬다. 의사 터미널은 `<pty.h>`의 `openpty`.

- nlibc(`libnlibc.a`)에 들어 있어서 링크 옵션이 필요 없다. 호스트에서는 `out/nlibc/bin/nlcc`, Nyxis 안에서는 `nlcc`로 그대로 빌드한다.
- C89로 쓸 수 있다.
- 오류가 나면 POSIX처럼 `-1`(또는 `NULL`)을 돌려주고 `errno`를 정한다.
- 레퍼런스는 헤더 주석([userland/nlibc/library/nx64.h](../userland/nlibc/library/nx64.h))이다.

## 가장 작은 앱

```c
#include <nx64.h>

static void draw(void *ctx)
{
    (void)ctx;
    nx64_tui_clear(NX64_DEFAULT);
    nx64_tui_box(2, 1, 30, 5, "hello", NX64_FRAME);
    nx64_tui_text(4, 3, "q: quit", NX64_DEFAULT);
}

static int key(void *ctx, int k)
{
    (void)ctx;
    return k != 'q';            /* 0 을 돌려주면 끝 */
}

int main(void)
{
    struct nx64_app app = { draw, key, NULL, 0 };   /* draw, key, tick, tick_ms */
    return nx64_tui_run(&app, NULL);
}
```

```sh
# 호스트
out/nlibc/bin/nlcc -O2 -o demo.run demo.c
# Nyxis 안 (같은 예제가 app:/src/examples/nx64demo.c 에 있다)
nlcc -o tmp:/demo.run app:/src/examples/nx64demo.c && tmp:/demo.run
```

데스크톱(ntui)에서는 Super+R로 명령을 입력하면 터미널 창(nterm: 터미널 에뮬레이터 + pty, NWP 클라이언트)에서 실행된다. Tab으로 최대화된 창을 고를 수 있다. TUI 앱은 그 창을 터미널로 쓴다 (창 크기가 바뀌면 SIGWINCH).

## TUI

| 함수 | 하는 일 |
|---|---|
| `nx64_tui_run(&app, ctx)` | 이벤트 루프: `draw` → 화면 반영 → 키나 `tick_ms`를 기다림 → 반복. `key`가 0을 돌려주면 끝 |
| `nx64_tui_begin` / `nx64_tui_end` | 루프를 직접 짤 때 화면을 넘겨받고 돌려준다 (raw 모드, 커서 숨김). 끝은 `atexit`로도 불린다 |
| `nx64_tui_cols` / `nx64_tui_rows` | 화면 크기 |
| `nx64_tui_clear`, `_put`, `_text`, `_textf`, `_fill` | 셀 버퍼에 그리기. 화면 밖은 잘린다 |
| `nx64_tui_box(x, y, w, h, title, style)` | `+ - |` 테두리와 제목 |
| `nx64_tui_bar(x, y, w, value, max, on, off)` | 막대 그래프 |
| `nx64_tui_flush` | 바뀐 칸만 화면에 낸다 (깜빡임 없음) |
| `nx64_tui_key(timeout_ms)` | 키 하나. 없으면 `NX64_KEY_NONE`. 방향키·Home/End·PgUp/PgDn·Del은 `NX64_KEY_*`, Ctrl 조합은 `NX64_KEY_CTRL('x')` |

- **스타일:** `nx64_style`은 256색 `fg`/`bg`와 `NX64_BOLD`·`NX64_UNDERLINE`·`NX64_REVERSE`로 이루어진다. `nx64_style_make(fg, bg, attr)`로 만든다.
- **기본 스타일:** `NX64_DEFAULT`, `NX64_FRAME`, `NX64_ACCENT`, `NX64_SELECTED`, `NX64_DIM`은 데스크톱의 nyx 테마와 같은 색이다.

## ntui 창 (터미널 창 다루기)

TUI 프로그램은 ntui(데스크톱)의 터미널 창(nterm) 안에서 돈다. 그 창을 프로그램이 직접 다룰 수 있다.

```c
int main(void)
{
    struct nx64_app app = { draw, key, NULL, 0 };

    nx64_ntui_require("mygame");            /* ntui 의 터미널 창이 아니면 이유를 쓰고 끝낸다 */
    nx64_win_resize(80, 25);                /* 안쪽 80 x 25 칸 */
    nx64_win_center();
    return nx64_tui_run(&app, NULL);        /* 크기가 바뀌면 셀 버퍼를 맞추고 다시 그린다 */
}                                           /* 끝나면 창은 처음 모양으로 돌아간다 */
```

| 함수 | 하는 일 |
|---|---|
| `nx64_ntui_present()` | ntui 의 터미널 창 안이면 1. `NWP_DISPLAY` 가 있을 때만 터미널에 묻는다 (XTVERSION `CSI > 0 q` 에 nterm 이 답한다) |
| `nx64_ntui_require(prog)` | 아니면 `prog: this program runs only in an ntui terminal window ...` 를 쓰고 `exit(1)` |
| `nx64_win_info(&w)` | `x`, `y` (창 왼쪽 위, 화면 칸), `cols`, `rows` (안쪽 크기), `state`, `area_cols`, `area_rows` (창 영역) |
| `nx64_win_resize(cols, rows)` | 안쪽 크기 (0 = 그대로). 컴포지터가 창 영역에 맞게 줄일 수 있다 |
| `nx64_win_move(x, y)` / `nx64_win_center()` | 옮기기 |
| `nx64_win_set_state(s)` | `NX64_WIN_NORMAL` / `MAXIMIZED` / `FULLSCREEN` / `MINIMIZED` |
| `nx64_win_title(t)` | 창 제목 (OSC 2) |
| `nx64_win_keep()` | 끝날 때 되돌리지 않는다 (기본은 크기/위치/상태를 처음 것으로 되돌린다) |

- ntui 밖이면 `-1` + `ENOTSUP`. 최대화/전체 화면인 창을 옮기거나 크기를 바꾸면 보통 배치가 된다.
- 바꾸기는 비동기다. 결과는 곧 온다: `nx64_tui_run` 이 다시 그리고, 직접 `nx64_tui_key` 를 쓰면 `NX64_KEY_RESIZE`.
  바꾼 바로 뒤의 `nx64_win_info` 는 nterm 이 컴포지터와 맞춘 뒤에 답한다 (NWP `SYNC`).
- 터미널 시퀀스는 xterm 과 같다 (`CSI 8;행;열 t`, `CSI 3;x;y t`, `CSI 9;1 t`, `CSI 10;1 t`, `CSI 2 t`, 질의 `CSI 11/13/18/19 t`,
  `OSC 2;제목`): 다른 언어나 셸 스크립트에서도 그대로 쓸 수 있다. 위치는 픽셀이 아니라 칸이다.
- 셸에서는 [`nwin`](../userland/nwin/main.c): `nwin`, `nwin size 100x30`, `nwin move 4 2`, `nwin center`, `nwin full`, `nwin demo`.

## 시스템 (비 POSIX)

| 함수 | 하는 일 |
|---|---|
| `nx64_info("hostname", buf, cap)` | `info:/<이름>` 값 (`version`, `abi`, `timezone`, `uptime_ms`, `memory/...`) |
| `nx64_info_ulong(name, &v)` | 숫자 값 |
| `nx64_memory(&m)` | `ram_kib`, `free_kib`, `used_kib`(= ram − free), `reserved_kib`(커널, initrd, 펌웨어), `managed_kib`(할당기가 관리하는 RAM), `alloc_kib`(= managed − free) |
| `nx64_uptime_ms()` | 부팅 후 밀리초 |
| `nx64_whoami(&id)` | uid, gid, 사용자 이름 (커널이 인증한 신원) |
| `nx64_setting_get` / `_set` / `_reset` | 앱 설정. 사용자 값 `roi:/<uid>/app/<앱>/<키>`, 없으면 기본값 `app:/<앱>.run/<키>` |
| `nx64_setting_path(where, app, key, buf, cap)` | 설정 파일의 경로 (`NX64_SETTING_DEFAULT` / `_USER`). 여러 줄 설정을 직접 읽을 때 |
| `nx64_service_create(name, public)` | 받는 쪽 채널. `"name"`이면 `svc:/<내 이름>/name`. 핸들을 닫으면 사라진다 |
| `nx64_service_receive(h, buf, cap, &from, timeout_ms)` | 메시지 하나. `from`에 보낸 쪽 pid/uid/gid (커널이 붙이므로 사칭할 수 없다) |
| `nx64_service_send(name, buf, len)` | 보내기 (열고, 보내고, 닫는다) |
| `nx64_request_poweroff()` | init에게 전원 끄기를 요청한다 (`svc:/sys/init`) |

시스템 부분은 nlibc 플랫폼 계층으로 시스템 콜을 부른다. 그래서 호스트 검사용 nlibc(Linux)에도 링크되고, 거기서는 `info:/`·`svc:/`가 없어 `-1`이 된다. `nx64_whoami`는 이름 대신 `getlogin()`을 쓴다.

## 앱 기본 설정 배포

앱 기본값은 `rootfs/app/<앱>/<키>`에 파일로 두면 된다. 이미지를 만들 때 앱 오브젝트 `app:/<앱>.run`의 자식으로 들어간다. 예를 들어 sysmon은 `rootfs/app/sysmon/refresh_ms`이고, 전체 목록은 [rootfs/app/README.md](../rootfs/app/README.md)에 있다.

## nx64api로 만든 프로그램

| 프로그램 | 쓰는 것 |
|---|---|
| [ntop](../userland/ntop/main.c) | TUI, `nx64_processes`, `nx64_signal_pid`, `nx64_timer_hz`, `nx64_memory`, 설정 `refresh_ms` `sort` |
| [sysmon](../userland/sysmon/main.c) | TUI 전부, `nx64_memory`, `nx64_uptime_ms`, `nx64_info`, `nx64_whoami`, 설정 `refresh_ms` |
| [nyfetch](../userland/coreutils/nyfetch.c) | `nx64_whoami`, `nx64_info`, `nx64_info_ulong`, `nx64_memory`, 설정 `fields` |
| [poweroff](../userland/poweroff/main.c) | `nx64_request_poweroff` (nlibc 앱이 되어 오류를 `strerror`로 알린다) |
| [nysh](../userland/nysh/main.c) | 설정 `mark`, `nx64_setting_path`로 `rc`, 내장 `whoami`는 `nx64_whoami` (커널 신원) |
| [journald](../userland/journald/main.c) | `nx64_service_create`/`_receive_fd`/`_fd` (로그, 서비스 출력 파이프, 읽기 요청), `nx64_processes`, 설정 `storage` `max_kib` |
| [journalman](../userland/journalman/main.c) | `nx64_journal_open`/`_parse`/`_format`/`_send`, `nx64_info` |
| [nex](../userland/nex/main.c) | `nx64_service_create`/`_connect`/`_send_fd` (init 에게 답장 채널을 넘긴다), 저널 읽기 |
| [ntui](../userland/ntui/sys.c) | 사용자/호스트/시간대(`nx64_whoami`, `nx64_info`), 테마·startup 설정, 시계 창의 업타임 |

ntui의 화면 그리기는 창 관리자용 자체 계층(`screen.c`, 역할 색)을 쓰고, 화면 모드에서는 아래 `nx64_display_*`로 프레임버퍼에 직접 그린다. init·login·elev는 nlibc 없이 도는 작은 시스템 런타임(`userland/lib/nyx.h`)에 남겨 두었다.

## 프로세스

| 함수 | 하는 일 |
|---|---|
| `nx64_processes(out, max)` | 살아 있는 프로세스 목록 (`struct nx64_proc`: pid, ppid, pgid, uid, 상태, 플래그(사용자/콘솔 세션/pty), CPU 틱, 시작 틱, 메모리 KiB, 실행 파일 이름, 사용자 이름). 전체 수를 돌려준다 |
| `nx64_signal_pid(pid, sig)` | pid 로 신호. 자기 uid 의 프로세스만 (root 는 모두): 아니면 `EPERM`, 없으면 `ESRCH` |
| `nx64_find_process(name)` | 그 이름의 다른 프로세스 pid (없으면 0). ntui 가 두 번 뜨지 않게 쓴다 |
| `nx64_timer_hz()` | CPU 틱의 단위 (초당 틱 수) |
| `nx64_pty_foreground(master_fd)` | pty 주 끝에서 본 포그라운드 프로세스 그룹 (작업이 없으면 0). 터미널 에뮬레이터가 단축키로 신호를 보낼 때 |
| `nx64_check_password(pw)` | 지금 사용자의 비밀번호가 맞으면 0 (화면 잠금). 틀리면 -1 + `EACCES` (커널이 1초 늦춘다) |

이름은 커널이 실행 파일 경로에서 정한다 (argv[0] 이 아니다: 다른 프로그램인 척할 수 없다).

## 화면 (dev:/console)

데스크톱처럼 화면 전체와 키보드를 직접 다루는 프로그램용. **로그인 셸(로그인이 콘솔 세션으로 띄운 그 프로세스)과 root 만** 열 수 있다 (그 밖에는 `EACCES`, 다른 프로세스가 쓰는 중이면 `EBUSY`). 그동안 커널 콘솔은 화면에 그리지 않고, PS/2 키는 stdin 대신 키 이벤트로 온다. 닫거나 프로세스가 끝나면 커널이 화면을 지우고 콘솔로 돌려준다.

| 함수 | 하는 일 |
|---|---|
| `nx64_display_open(&d)` | 화면을 가져온다. `d.fb`(32비트 픽셀, 줄 간격 `d.stride`), `d.width`/`d.height`, 커널 콘솔 글꼴(`d.font`, 글자 칸 `d.cell_w` x `d.cell_h`, 알파 0~255) |
| `nx64_display_rgb(&d, 0xRRGGBB)` | 이 화면의 픽셀 값 (RGB/BGR 배치를 맞춘다) |
| `nx64_display_keys(&d, ev, max, timeout_ms)` | 키 이벤트를 읽는다: `code`(PS/2 세트 1, E0 확장은 +0x100, `NX64_KC_*`: PrintScreen 과 메뉴 키 포함), `mods`(`NX64_KMOD_SHIFT/CTRL/ALT/SUPER/ALTGR/CAPS`), `pressed`(누름/뗌), `ch`(Shift 반영 ASCII). `d.fd` 를 poll 해도 된다 |
| `nx64_display_close(&d)` | 돌려준다 |

## 공유 메모리와 fd 보내기

| 함수 | 하는 일 |
|---|---|
| `nx64_shm_create(size)` | 0 으로 채운 공유 메모리 fd (close-on-exec) |
| `nx64_shm_map(fd)` | 매핑 주소 (fd 를 닫으면 매핑도 사라진다) |
| `nx64_service_send_fd(name, buf, len, fd)` | 서비스 채널에 메시지와 fd 사본을 보낸다 (Unix 소켓의 fd 넘기기) |
| `nx64_service_receive_fd(h, buf, cap, &from, timeout, &fd)` | 받은 fd 는 `fd` 에 (없으면 -1) |
| `nx64_service_fd(h)` | 서비스 받는 핸들을 poll 할 수 있는 fd 로 |
| `nx64_service_connect(name)` | 서비스에 보내는 끝을 fd 로. 이 fd 를 다른 서비스에 붙여 보내면 그쪽이 답장할 수 있다 (nex 가 init 에게) |

창 프로그램은 이것들 위에 만든 [`<nwp.h>`](NWP.md) 를 쓴다.

## 저널 (journald)

로그는 journald(`svc:/sys/journal`)가 모은다. 보낸 쪽의 pid/uid 는 커널이 붙이므로 속일 수 없다.

- **서비스(nex 유닛)**: 아무것도 하지 않아도 된다. stdout/stderr 의 줄마다 그 유닛의 항목이 된다. 줄이 `<3>` 으로 시작하면 우선순위 3(err).
- **그 밖의 프로그램**: `nx64_journal_printf(NX64_LOG_WARNING, "disk %d%% full", pct);`

| 함수 | 하는 일 |
|---|---|
| `nx64_journal_send(prio, tag, msg)` | 항목 쓰기. `tag` 가 NULL 이면 이 프로세스의 이름. 개행마다, `NX64_JOURNAL_LINE_MAX`(200) 바이트마다 나뉜다. journald 가 없으면 -1 |
| `nx64_journal_printf(prio, fmt, ...)` | printf 형식으로 |
| `nx64_log_priority("err")` / `nx64_log_priority_name(3)` | 우선순위 이름 ↔ 번호 (`emerg alert crit err warning notice info debug`, `0`..`7`) |
| `nx64_journal_open(&q)` | 읽기: 조건(`unit`, `tag`, `max_prio`, `pid`, `lines`, `this_boot`, `follow`, `since`)에 맞는 항목을 한 줄에 하나씩 보내 주는 fd. root 는 모두, 다른 사용자는 자기 uid 의 항목만 |
| `nx64_journal_parse(line, &e)` | 한 줄을 `struct nx64_journal_entry` 로 (`usec`, `boot`, `pid`, `uid`, `prio`, `unit`, `tag`, `message`) |
| `nx64_journal_format(&e, host, buf, cap)` | `Nov 14 12:34:56 host tag[pid]: message` (UTC) |

저장 형식(한 줄에 항목 하나)은 `<UTC 마이크로초> <부팅 번호> <pid> <uid> <우선순위> <유닛|-> <태그|-> <메시지>` 이다. 명령으로는 [journalman](../userland/journalman/main.c) (`journalman -u journald -f`, `journalman log -p err 메시지`), 서비스 관리는 [nex](../userland/nex/main.c).

## 의사 터미널 (`<pty.h>`)

`openpty(&master, &slave, NULL, NULL, &ws)`: 주 끝에 쓴 바이트는 키 입력이 되고(종속 끝의 줄 편집기/raw 규칙), 주 끝을 읽으면 프로그램 출력이 그대로 나온다. 종속 끝을 자식의 0/1/2로 넘기면 그 pty가 자식의 제어 터미널(`dev:/tty`)이 된다 (`isatty`, `tcsetattr`, `TIOCGWINSZ`, Ctrl-C = SIGINT). 크기는 `ioctl(master, TIOCSWINSZ, &ws)` (포그라운드 그룹에 SIGWINCH). 주 끝을 닫으면 그쪽 프로세스는 SIGHUP, 종속 끝 쪽이 모두 끝나면 주 끝 읽기가 0 (EOF). 터미널 창 nterm(`userland/nterm/main.c`)이 예다.

## 예제

- [userland/sysmon/main.c](../userland/sysmon/main.c): 시스템 모니터. 업타임과 메모리 막대를 보여 주고, 설정 `refresh_ms`를 쓴다. 데스크톱 메뉴 `[+]` → System monitor로 연다.
- [userland/nlibc/examples/nx64demo.c](../userland/nlibc/examples/nx64demo.c): 위의 가장 작은 앱.
