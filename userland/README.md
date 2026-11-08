## nyxis os userland programs

새 디렉터리 `userland/<이름>/` 에 `.c` 를 넣으면 빌드가 자동으로 찾아 `fs:/usr/<이름>.run` 으로 rootfs 에 넣는다
(`main.c` 가 `"nyx.h"` 를 포함하면 userland/lib 런타임, 아니면 nlibc 로 디렉터리의 `*.c` 를 모두 묶는다. `nxkernel/Makefile` 의 `USERLAND_DIRS`).

- `init/`, `login/`, `poweroff/`, `sudo/`: 시스템 프로그램 (userland/lib 의 작은 런타임). `sudo 명령` 은 init 에게
  root 실행을 요청한다 (허용 uid: `roi:/app/0/init/sudoers.dat`, 기본 0 과 1000, 비밀번호 확인)
- `ntui/`: TUI 라이브러리/설치 프로그램 (작업 중, 지금은 키 입력 확인용 main)
- `nysh/`: 셸 (nlibc). 파이프, 리디렉션, 변수, 따옴표, 글롭, `&&`/`||`, cd. 문법은 `nysh/main.c` 머리말
- `coreutils/`: cat, ls, cp, mv, rm, touch, mkdir, echo 등 기본 명령어 (nlibc, [README](coreutils/README.md))
- `nlibc/`: C 라이브러리와 호스트 SDK (`nlcc` = gcc/ld 래퍼)
- `kilo/`: 텍스트 편집기 `kilo 파일` (antirez/kilo 포팅, 원본은 `third_party/kilo`. [README](kilo/README.md))
- `nlcc/`: Nyxis 안의 C 컴파일러 `app:/nlcc.run` (TinyCC 기반, 자기 자신을 컴파일한다. [README](nlcc/README.md))
