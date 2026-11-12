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

데스크톱(ntui)에서는 Super+R로 명령을 입력하면 터미널 창(ntui의 터미널 에뮬레이터 + pty)에서 실행된다. Tab으로 최대화된 창을 고를 수 있다. TUI 앱은 그 창을 터미널로 쓴다 (창 크기가 바뀌면 SIGWINCH).

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
| [ntui](../userland/ntui/sys.c) | 사용자/호스트/시간대(`nx64_whoami`, `nx64_info`), 테마·startup 설정, 시계 창의 업타임 |

ntui의 화면 그리기는 창 관리자용 자체 계층(`screen.c`, 역할 색)을 쓰고, 화면 모드에서는 아래 `nx64_display_*`로 프레임버퍼에 직접 그린다. init·login·sudo는 nlibc 없이 도는 작은 시스템 런타임(`userland/lib/nyx.h`)에 남겨 두었다.

## 프로세스

| 함수 | 하는 일 |
|---|---|
| `nx64_processes(out, max)` | 살아 있는 프로세스 목록 (`struct nx64_proc`: pid, ppid, pgid, uid, 상태, 플래그(사용자/콘솔 세션/pty), CPU 틱, 시작 틱, 메모리 KiB, 실행 파일 이름, 사용자 이름). 전체 수를 돌려준다 |
| `nx64_signal_pid(pid, sig)` | pid 로 신호. 자기 uid 의 프로세스만 (root 는 모두): 아니면 `EPERM`, 없으면 `ESRCH` |
| `nx64_find_process(name)` | 그 이름의 다른 프로세스 pid (없으면 0). ntui 가 두 번 뜨지 않게 쓴다 |
| `nx64_timer_hz()` | CPU 틱의 단위 (초당 틱 수) |
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

## 의사 터미널 (`<pty.h>`)

`openpty(&master, &slave, NULL, NULL, &ws)`: 주 끝에 쓴 바이트는 키 입력이 되고(종속 끝의 줄 편집기/raw 규칙), 주 끝을 읽으면 프로그램 출력이 그대로 나온다. 종속 끝을 자식의 0/1/2로 넘기면 그 pty가 자식의 제어 터미널(`dev:/tty`)이 된다 (`isatty`, `tcsetattr`, `TIOCGWINSZ`, Ctrl-C = SIGINT). 크기는 `ioctl(master, TIOCSWINSZ, &ws)` (포그라운드 그룹에 SIGWINCH). 주 끝을 닫으면 그쪽 프로세스는 SIGHUP, 종속 끝 쪽이 모두 끝나면 주 끝 읽기가 0 (EOF). ntui의 터미널 창(`userland/ntui/nterminal.c`)이 예다.

## 예제

- [userland/sysmon/main.c](../userland/sysmon/main.c): 시스템 모니터. 업타임과 메모리 막대를 보여 주고, 설정 `refresh_ms`를 쓴다. 데스크톱 메뉴 `[+]` → System monitor로 연다.
- [userland/nlibc/examples/nx64demo.c](../userland/nlibc/examples/nx64demo.c): 위의 가장 작은 앱.
