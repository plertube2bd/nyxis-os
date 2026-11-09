# kilo — 텍스트 편집기

[antirez/kilo](https://github.com/antirez/kilo)(BSD 2-Clause, 약 1300줄)를 Nyxis 로 옮긴 것입니다.
원본은 `third_party/kilo/` 에 수정 없이 두고(출처와 커밋은 `NYXIS-UPSTREAM.txt`), 빌드할 때 이 디렉터리의
`kilo-nyxis.patch` 를 적용해 `fs:/usr/kilo.run` 을 만듭니다 (`nxkernel/Makefile` 의 kilo 규칙).

```
kilo 파일이름        # 없는 파일이면 저장할 때 만든다
kilo                 # 파일 이름을 묻는다 (빈 줄이나 Ctrl-D 면 끝). ntui 의 실행 상자(Ctrl-R, 전체 화면)에서 편하다
```

| 키 | 동작 |
|---|---|
| 방향키, Home/End, PgUp/PgDn | 커서 이동 (PS/2 키보드와 시리얼 터미널 모두) |
| Ctrl-S | 저장 |
| Ctrl-Q | 끝내기 (저장하지 않은 변경이 있으면 세 번 더 누른다) |
| Ctrl-F | 찾기 (방향키로 다음/이전, Enter 확정, ESC 취소) |

C/C++ 파일(`.c .h .cpp .hpp .cc`)은 문법 강조를 합니다.

## Nyxis 에서 필요했던 것
kilo 는 POSIX 터미널을 그대로 쓰므로 대부분은 패치가 아니라 시스템 쪽에 기능을 더해 해결했습니다.

- **커널**: 콘솔 raw 모드 (`NX_DEVCMD_TTY_MODE`: 줄 편집/에코 없이 바이트를 바로 넘기고, 키보드의 방향키 등은
  VT 시퀀스로 바꾼다)와 창 크기 (`NX_DEVCMD_TTY_WINSIZE`). raw 모드를 켠 프로세스가 끝나면 커널이 되돌린다.
  화면 그리기는 커널 터미널 해석기(`nxkernel/console/outputs/ansi.h`)가 처리한다.
- **nlibc**: `termios.h` (`tcgetattr`/`tcsetattr`, VMIN=0/VTIME 시간 제한 읽기), `sys/ioctl.h` (`TIOCGWINSZ`),
  `getline`/`getdelim`, `SIGWINCH`, `ENOTTY`.
- **패치** (`kilo-nyxis.patch`, 네 군데):
  - 저장: 커널에 파일 자르기가 없어 `ftruncate` 대신 `O_TRUNC` 로 연다.
  - 끝낼 때 화면을 지우고 커서를 맨 위로 (콘솔에 대체 화면이 없어 편집기 화면이 남는다).
  - Home/End 처리 (upstream 버그: 키를 해석만 하고 처리하지 않아 키 코드가 글자로 들어갔다).
  - 인자 없이 실행하면 사용법을 보이고 끝내는 대신, raw 모드로 바꾸기 전에 파일 이름을 한 줄 묻는다.

## 제약
- 콘솔 크기는 바뀌지 않으므로 `SIGWINCH` 는 오지 않는다.
- 저장은 파일을 지우고 다시 만든다 (`O_TRUNC` 의 nlibc 구현): 저장 도중 실패하면 내용을 잃을 수 있다.
- 탭은 kilo 가 공백으로 펼쳐 보이고, 한글 등 비 ASCII 글자는 콘솔이 `?` 로 보인다.
