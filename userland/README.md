## nyxis os userland programs

사용자 프로그램은 **C99 까지의 문법**으로 컴파일한다 (`-std=c99 -pedantic -Werror`, VLA 는 금지). 라이브러리(`lib/`, `nlibc/`)와
nlibc 의 검사/예제는 C89 그대로다. nlibc 에는 아직 `<stdbool.h>` 가 없다 (`_Bool` 은 쓸 수 있다).

새 디렉터리 `userland/<이름>/` 에 `.c` 를 넣으면 빌드가 자동으로 찾아 `fs:/usr/<이름>.run` 으로 rootfs 에 넣는다
(디렉터리의 `*.c` 를 모두 묶는다: `main.c` 가 `"nyx.h"` 를 포함하면 userland/lib 런타임, 아니면 nlibc. `nxkernel/Makefile` 의 `USERLAND_DIRS`).

- `init/`, `login/`, `elev/`: 시스템 프로그램 (userland/lib 의 작은 런타임). `elev 명령` 은 init 에게
  root 실행을 요청한다 (허용 uid: `roi:/app/0/init/sudoers.dat`, 기본 0 과 1000, 비밀번호 확인).
  예전 이름 `sudo` 도 된다: `app:/sudo.run` 은 `elev.run` 을 가리키는 심볼릭 링크 (`nxkernel/Makefile` 의 `ROOTFS_LINKS`)
- `init/`: 첫 프로세스. 로그인, elev, 그리고 **서비스 관리자**(`init/service.c`): 유닛 파일(`app:/init.run/units/<이름>`,
  관리자 것은 `roi:/app/0/init/units/<이름>`)대로 백그라운드 프로그램을 띄우고 지켜보다가 restart 정책대로 다시 띄운다.
  서비스의 stdout/stderr 는 저널로 간다. 유닛 파일 형식은 `service.c` 머리말 (또는 `nex help-unit`)
- `nex/`: 서비스 관리 명령 (systemctl 과 비슷하다). `nex`, `nex status journald`, `elev nex restart 유닛`,
  `elev nex enable --now 유닛`, `nex cat 유닛`, `elev nex reload`
- `journald/`: 시스템 저널 데몬 (유닛 `journald`, 부팅 때 init 이 띄운다). `svc:/sys/journal` 로 로그와 서비스 출력을 받아
  `disk:/journal/` (디스크가 없으면 `fs:/var/log/journal/`) 에 쓴다. 프로그램에서 쓰기: `nx64_journal_printf` ([docs/NX64API.md](../docs/NX64API.md))
- `journalman/`: 저널 보기/쓰기 (journalctl + logger). `journalman -b`, `journalman -u journald -n 20`, `journalman -f`,
  `journalman -p err`, `journalman log -p warning 메시지`. root 가 아니면 자기 항목만 (시스템 로그는 `elev journalman`)
- `poweroff/`: init 에게 전원 끄기를 요청한다 (nlibc + nx64api `nx64_request_poweroff`)
- `sysmon/`: 시스템 모니터 (nx64api 의 TUI 예제. [docs/NX64API.md](../docs/NX64API.md))
- `ntop/`: 프로세스 관리 (nx64api TUI). PID/사용자/상태/CPU%/메모리/이름, 정렬, k=SIGTERM K=SIGKILL. `ntop -b` 는 표를 한 번 찍는다.
  ntui 메뉴의 Processes. 신호는 자기 프로세스에만 (root 는 모두)
- `ntui/`: 데스크탑 **컴포지터** (기본 로그인 셸: `rootfs/roi/default/shell`). Wayland 와 같은 구조다: ntui 만 화면과
  키보드를 가지고(로그인 셸로 시작하면 `dev:/console`), 창 프로그램은 따로 도는 프로세스로서 NWP([docs/NWP.md](../docs/NWP.md),
  `<nwp.h>`: 파이프 + 공유 메모리)로 연결해 자기 창만 그린다. ntui 가 하는 것은 창 배치/테두리/포커스, 작업 표시줄, 메뉴(Super),
  실행 상자(Super+R), 창 전환기(Alt+Tab), 잠금(Super+L), 스크린샷, 그리고 키를 어디로 보낼지(단축키 설정 `keys`:
  `rootfs/app/ntui/keys`, 사용자 값 `roi:/<uid>/app/ntui/keys`, `ntui/keymap.c`). 화면을 가져올 수 없으면(셸에서 띄움)
  지금 터미널 위에서 Ctrl 단축키로 돈다. SELFTEST 이미지는 테스트 하네스를 위해 셸을 nysh 로 둔다
- `nterm/`: 터미널 창 (NWP 클라이언트): pty + 터미널 에뮬레이터(`vt.c`) + 대화형 nysh. Super+Enter. `nterm -e 명령` 은 명령 하나
- `nfiles/`: 파일 탐색기 창 (NWP 클라이언트). Super+E
- `nclock/`: 시계/달력 창 (NWP 클라이언트). 작업 표시줄의 시계
- `nwin/`: 지금 터미널 창 다루기 (nx64api `nx64_win_*` 예제): `nwin` (정보), `nwin size 100x30`, `nwin move 4 2`,
  `nwin center`, `nwin max|full|normal|min`, `nwin title 제목`, `nwin demo`. ntui 의 터미널 창 밖에서는 실패한다
- `nkeys/`: 단축키 설정 창 (NWP 클라이언트, Super+I. 키 녹음 중에는 컴포지터의 단축키를 막는다). `nkeys -h` 는 단축키 보기 (F1)
- `nysh/`: 셸 (nlibc). 파이프, 리디렉션, 변수, 따옴표, 글롭, `&&`/`||`, cd. 문법은 `nysh/main.c` 머리말
- `coreutils/`: cat, ls, cp, mv, rm, touch, mkdir, echo 등 기본 명령어 (nlibc, [README](coreutils/README.md))
- `nlibc/`: C 라이브러리와 호스트 SDK (`nlcc` = gcc/ld 래퍼). Nyxis 전용 API `<nx64.h>` (nx64api) 도 여기 있다
- `kilo/`: 텍스트 편집기 `kilo 파일` (antirez/kilo 포팅, 원본은 `third_party/kilo`. [README](kilo/README.md))
- `nlcc/`: Nyxis 안의 C 컴파일러 `app:/nlcc.run` (TinyCC 기반, 자기 자신을 컴파일한다. [README](nlcc/README.md))
