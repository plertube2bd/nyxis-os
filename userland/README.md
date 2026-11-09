## nyxis os userland programs

사용자 프로그램은 **C99 까지의 문법**으로 컴파일한다 (`-std=c99 -pedantic -Werror`, VLA 는 금지). 라이브러리(`lib/`, `nlibc/`)와
nlibc 의 검사/예제는 C89 그대로다. nlibc 에는 아직 `<stdbool.h>` 가 없다 (`_Bool` 은 쓸 수 있다).

새 디렉터리 `userland/<이름>/` 에 `.c` 를 넣으면 빌드가 자동으로 찾아 `fs:/usr/<이름>.run` 으로 rootfs 에 넣는다
(`main.c` 가 `"nyx.h"` 를 포함하면 userland/lib 런타임, 아니면 nlibc 로 디렉터리의 `*.c` 를 모두 묶는다. `nxkernel/Makefile` 의 `USERLAND_DIRS`).

- `init/`, `login/`, `poweroff/`, `sudo/`: 시스템 프로그램 (userland/lib 의 작은 런타임). `sudo 명령` 은 init 에게
  root 실행을 요청한다 (허용 uid: `roi:/app/0/init/sudoers.dat`, 기본 0 과 1000, 비밀번호 확인)
- `ntui/`: TUI 데스크탑 (기본 로그인 셸: `rootfs/roi/default/shell`). 창 관리, 작업 표시줄, 터미널(Ctrl-T), 파일 탐색기(Ctrl-E),
  실행 상자(Ctrl-R). 단축키는 `ntui/main.c` 머리말. SELFTEST 이미지는 테스트 하네스를 위해 셸을 nysh 로 둔다
- `nysh/`: 셸 (nlibc). 파이프, 리디렉션, 변수, 따옴표, 글롭, `&&`/`||`, cd. 문법은 `nysh/main.c` 머리말
- `coreutils/`: cat, ls, cp, mv, rm, touch, mkdir, echo 등 기본 명령어 (nlibc, [README](coreutils/README.md))
- `nlibc/`: C 라이브러리와 호스트 SDK (`nlcc` = gcc/ld 래퍼)
- `kilo/`: 텍스트 편집기 `kilo 파일` (antirez/kilo 포팅, 원본은 `third_party/kilo`. [README](kilo/README.md))
- `nlcc/`: Nyxis 안의 C 컴파일러 `app:/nlcc.run` (TinyCC 기반, 자기 자신을 컴파일한다. [README](nlcc/README.md))
