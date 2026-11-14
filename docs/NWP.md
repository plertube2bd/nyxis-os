# NWP — Nyxis Window Protocol

Nyxis 데스크톱은 Wayland 와 같은 구조다. **컴포지터**(`ntui`)만 화면과 키보드를 가지고, 창 프로그램(**클라이언트**)은
따로 도는 프로세스로서 컴포지터에 연결해 자기 창의 내용만 그린다.

```
                 dev:/console (화면 + 키 이벤트)
                            │
        ┌───────────────────┴───────────────────┐
        │ ntui (컴포지터)                         │
        │  창 배치·테두리·포커스, 작업 표시줄, 메뉴, │
        │  실행 상자, 창 전환기, 잠금, 단축키        │
        └──┬──────────┬──────────┬──────────┬───┘
     파이프 + 공유 메모리 (클라이언트마다)
           │          │          │          │
        nterm      nfiles     nclock     nkeys ...   ← 각자 프로세스
          │
        nysh (pty) ─ 그 안에서 친 nfiles 도 같은 컴포지터에 창을 연다 (NWP_DISPLAY 를 물려받는다)
```

| 누가 | 하는 일 |
|---|---|
| 컴포지터 (`userland/ntui`) | 화면을 그리고(창 테두리/제목, 작업 표시줄, 팝업), 키를 받아 **어디로 보낼지 정한다**(단축키 설정 `keys`: 컴포지터 동작 / 명령 / 포커스된 창 / 신호). 창의 크기와 위치를 정한다. |
| 클라이언트 (`nterm`, `nfiles`, `nclock`, `nkeys`, …) | 자기 창의 칸을 그리고 commit 한다. 키/크기/포커스/닫기 이벤트에 반응한다. 화면과 키보드를 직접 만질 수 없다. |
| 커널 | 공유 메모리(`NxSharedMemoryCreate/Map`), 핸들 보내기(`NxChannelSendHandle/ReceiveEx`), 파이프, 채널. |

## 찾기와 연결

컴포지터는 채널 `svc:/<사용자>/nwp` 를 만들고(같은 사용자의 프로세스만 보낼 수 있다) 환경 변수
`NWP_DISPLAY` 에 그 이름을 둔다 (Wayland 의 `WAYLAND_DISPLAY`). 컴포지터가 띄운 프로그램과 그 자식은 모두 이 값을
물려받는다.

클라이언트는 연결할 때:

1. 파이프 두 개를 만든다: **위**(클라이언트 → 컴포지터), **아래**(컴포지터 → 클라이언트). 모두 close-on-exec.
2. 채널로 핸들을 붙여 보낸다 (Unix 소켓의 fd 넘기기와 같다):
   - `"nwp up <쿠키>"` + 위 파이프의 **읽기 끝**
   - `"nwp down <쿠키>"` + 아래 파이프의 **쓰기 끝**

   쿠키는 같은 프로세스의 여러 연결을 구분한다. 컴포지터는 보낸 프로세스의 pid(커널이 붙인 것)와 쿠키로 둘을 짝짓는다.
3. 보낸 끝은 자기 쪽에서 닫는다. 이후는 파이프 위의 메시지다.

연결이 끊기는 것은 파이프의 EOF 로 안다: 클라이언트가 끝나면 컴포지터가 그 창들을 닫고, 컴포지터가 끝나면(로그아웃)
클라이언트의 이벤트 읽기가 EOF 를 보고 끝난다.

## 메시지

```c
struct nwp_hdr { unsigned short type; unsigned short len; unsigned int surface; };  /* + len 바이트 */
```

`len` 은 `NWP_MSG_MAX`(512) 이하. 구조체와 상수는 [`<nwp.h>`](../userland/nlibc/library/nwp.h).

### 요청 (클라이언트 → 컴포지터)

| 요청 | 내용 | 뜻 |
|---|---|---|
| `HELLO` | `nwp_hello` | 첫 메시지. 판 번호, 앱 이름(`app_id`: 단축키의 `@앱`, 하나만 띄울 앱 찾기) |
| `CREATE` | `nwp_create` | 창(서피스) 하나. 바라는 크기, 처음 상태(보통/최대화/전체 화면), 제목 |
| `TITLE` | `char[32]` | 제목 바꾸기 |
| `ATTACH` | `nwp_attach` | 이 창이 읽을 버퍼와 그 안의 격자 크기, 형식 |
| `COMMIT` | – | 버퍼를 화면에 올린다 |
| `DESTROY` | – | 창 닫기 |
| `STATE` | `unsigned` | 최대화/전체 화면/최소화/보통 부탁 |
| `INHIBIT` | `unsigned` | 1 이면 이 창이 포커스인 동안 단축키를 막고 모든 키를 받는다 (키 녹음: Wayland 의 keyboard-shortcuts-inhibit) |
| `RELOAD` | – | 컴포지터가 설정(단축키)을 다시 읽는다 |
| `BUFFER_FREE` | `unsigned` | 버퍼를 놓는다 |
| `MOVE` | `nwp_move` | 창(테두리 포함)의 왼쪽 위를 화면 칸 x, y 로 (보통 배치가 된다. 창 영역 안으로 맞춘다) |
| `RESIZE` | `nwp_resize` | 안쪽 크기 (0 = 그대로, 보통 배치가 된다). 컴포지터가 줄일 수 있다: 실제 크기는 `CONFIGURE` |
| `SYNC` | `unsigned` | 앞의 요청을 다 처리하면 같은 번호로 `DONE` (`wl_display.sync`). 그 결과의 `CONFIGURE`/`PLACE` 는 `DONE` 보다 먼저 온다 |

### 이벤트 (컴포지터 → 클라이언트)

| 이벤트 | 내용 | 뜻 |
|---|---|---|
| `WELCOME` | `nwp_welcome` | HELLO 의 답. 화면 모드인지(Super 키가 있는지), 창이 가질 수 있는 가장 큰 크기 |
| `THEME` | `nwp_theme` | 역할(바탕/창/테두리/선택 …)마다 색. 테마가 바뀌면 다시 온다 |
| `CONFIGURE` | `nwp_configure` | 이 크기와 상태로 그려 달라 (처음 한 번, 크기가 바뀔 때마다) |
| `KEY` | `int` | 키 (`NWP_KEY_*` / 글자 \| `NWP_MOD_*`). 컴포지터가 단축키로 쓰지 않은 것만 |
| `FOCUS` | `unsigned` | 포커스를 받았다/잃었다 |
| `CLOSE` | – | 사용자가 창을 닫았다 (Alt+F4). 정리하고 DESTROY 하거나 끝낸다. 3초 넘게 응답하지 않으면 다음 닫기에서 컴포지터가 프로세스를 끝낸다 |
| `FRAME` | – | 앞의 COMMIT 을 화면에 올렸다: 버퍼를 다시 써도 된다 |
| `SIGNAL` | `unsigned` | 단축키의 `signal` 문장 (터미널은 자기 포그라운드 작업에 보낸다) |
| `ERROR` | 글 | 규약 위반. 곧 연결이 끊긴다 |
| `PLACE` | `nwp_place` | 창의 자리 (왼쪽 위, 창 영역 크기). 처음 한 번, 옮겨질 때마다 |
| `DONE` | `unsigned` | `SYNC` 의 답 |

창 조작은 컴포지터가 정한다: 클라이언트는 부탁하고 결과를 `CONFIGURE`/`PLACE` 로 받는다 (`nwp_move`, `nwp_resize`,
`nwp_set_state`, 결과는 `nwp_x`/`nwp_y`/`nwp_state`, 기다리려면 `nwp_roundtrip`). 터미널 안의 프로그램은 nterm 을 거친다:
xterm 의 창 조작 시퀀스(`CSI … t`)를 nterm 이 이 요청으로 바꾼다 (nx64api 의 `nx64_win_*`, [NX64API.md](NX64API.md)).

## 버퍼

버퍼는 **공유 메모리**다 (`wl_shm` 과 같다). 클라이언트가 `NxSharedMemoryCreate` 로 만들고 채널로
`"nwp buf <쿠키> <번호>"` 에 붙여 보낸 뒤, `ATTACH` 로 그 번호를 창에 붙인다. 컴포지터는 채널을 파이프보다 먼저 비우므로
ATTACH 를 읽을 때는 버퍼가 와 있다.

- 형식 `NWP_FORMAT_CELLS`: `struct nwp_cell` (8바이트: 글자, 256색 글자색/배경색, 속성) 의 `cols × rows` 격자, 줄 순서.
  글자는 ASCII 와 상자 그리기/블록 문자(U+2500–259F). 컴포지터는 화면 모드에서는 선으로, 터미널 위에서는 `+ - |` 로 그린다.
- 형식 `NWP_FORMAT_XRGB8888`: 번호만 정해 두었다. 그래픽 컴포지터가 생기면 같은 연결·버퍼·configure·commit 규약에
  픽셀 버퍼를 붙인다.
- COMMIT 을 받으면 컴포지터는 버퍼를 자기 쪽으로 복사하고 `FRAME` 을 보낸다. 클라이언트는 FRAME 전에 버퍼를 덮어쓰지 않는다
  (`nwp_commit` 이 알아서 기다렸다 보낸다: 그리기는 막히지 않는다).
- 클라이언트 라이브러리는 WELCOME 의 가장 큰 크기로 버퍼를 한 번 만든다: 창 크기가 바뀌어도 다시 만들 일이 없다.

## 컴포지터가 멈추지 않는 규칙

- 이벤트는 클라이언트마다 쌓아 두고, 파이프에 남은 자리만큼만 쓴다 (`fstat` 의 크기로 안다). 쌓인 것이 64KiB 를 넘으면
  (읽지 않는 클라이언트) 연결을 끊는다.
- 규약을 어기면(모르는 버퍼, 너무 긴 메시지, HELLO 없이 요청 …) ERROR 를 보내고 끊는다.
- 모르는 요청/이벤트 번호는 넘긴다 (새 판과 함께 쓸 수 있게).

## 클라이언트 라이브러리

nlibc 에 들어 있다 (`#include <nwp.h>`). 창 하나 = 연결 하나.

```c
#include <nwp.h>

int main(void)
{
    struct nwp_window *w = nwp_open("hello", "hello", 30, 5, NWP_STATE_NORMAL);
    struct nwp_event ev;

    if (w == NULL)
        return 1;                       /* NWP_DISPLAY 가 없다: ntui 안에서 띄운다 */
    for (;;) {
        nwp_clear(w, NWP_ROLE_WINDOW);
        nwp_text(w, 2, 2, "hello, q to quit", NWP_ROLE_WINDOW);
        nwp_commit(w);
        if (nwp_next_event(w, &ev, -1) < 0 || ev.type == NWP_EVENT_CLOSE ||
            (ev.type == NWP_EVENT_KEY && ev.key == 'q'))
            break;
    }
    nwp_close(w);
    return 0;
}
```

다른 fd 와 함께 기다리려면 `nwp_fd(w)` 를 poll 하고 `nwp_next_event(w, &ev, 0)` 으로 비운다 (`userland/nterm/main.c`).

## 지금 있는 클라이언트

| 프로그램 | 앱 이름 | 하는 일 |
|---|---|---|
| [`nterm`](../userland/nterm/main.c) | `nterminal` | 터미널 에뮬레이터 + pty + nysh. `-e 명령`, `-d 디렉터리`, `-m`/`-f`/`-x`/`-g` |
| [`nfiles`](../userland/nfiles/main.c) | `nfiles` | 파일 탐색기. 터미널/보기/편집은 nterm 을 띄운다 |
| [`nclock`](../userland/nclock/main.c) | `nclock` | 시계와 달력 |
| [`nkeys`](../userland/nkeys/main.c) | `nkeys` / `nhelp` | 단축키 설정 (`INHIBIT` 로 키 녹음, `RELOAD`), `-h` 는 단축키 보기 |

`ntop`, `sysmon`, `kilo` 처럼 터미널에서 도는 프로그램은 `nterm -e` 안에서 돈다.
