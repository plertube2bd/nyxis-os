# nlibc — Nyxis C89 표준 C 라이브러리

목표: ISO C89 (ANSI X3.159-1989) 호스트 구현 + POSIX 기본 계층. 라이브러리 자신도 `-std=c89 -pedantic -Werror`
로 빌드된다. C89 의 표준 헤더 15개를 모두 제공하고, 그 위에 POSIX 의 파일 디스크립터 I/O(dup2, pipe 포함),
디렉터리, 권한(chmod/umask), 프로세스(fork/exec/posix_spawn/waitpid/kill/system), 현재 디렉터리를 제공한다.

```
library/   공개 헤더: C89 15개 + stdint.h
           POSIX: unistd.h fcntl.h dirent.h spawn.h termios.h sys/types.h sys/stat.h sys/wait.h sys/ioctl.h
           내부용: _nl_types.h _nl_features.h
src/       구현
  nxsys.c        플랫폼 계층 (Nyxis 시스템 콜 + Nstatus -> errno). 경계는 src/nl_internal.h
  crt0.s start.c 진입점, exit/atexit/abort/assert
  setjmp.s       setjmp/longjmp
  string.c ctype.c stdlib.c malloc.c
  stdio.c printf.c scanf.c   (stdio.c 에 POSIX getline/getdelim)
  fpconv.c       double <-> 십진 정확 변환 (printf %e %f %g, strtod/atof, scanf)
  math.c         math.h (x87 확장 정밀도 + Payne-Hanek 인자 축소)
  time.c signal.c locale.c
  posix_fd.c     open/read/write/lseek/dup/dup2/pipe/fcntl/stat/mkdir/unlink/rmdir/access/chmod/umask
  termios.c      tcgetattr/tcsetattr/cfmakeraw, ioctl(TIOCGWINSZ): 콘솔 raw 모드와 창 크기 (termios.h 머리말)
  posix_path.c   현재 디렉터리 (getcwd/chdir) 와 경로 해석 __nl_resolve
  dirent.c       opendir/readdir
  process.c      getpid/getppid/getuid/getlogin/sleep, fork/exec*/_exit, posix_spawn, waitpid/wait, kill, system
  env.c          getenv/setenv/unsetenv, environ
nlibc.ld   링커 스크립트 (정적 ET_EXEC, R-X / R-- / RW- 세그먼트)
tools/nlcc 컴파일러 드라이버 (nlld 라는 이름으로 부르면 링크만)
test/      nlibc_test.c (호스트 + Nyxis), fputest.c (커널 FPU 상태 저장 검사, Nyxis)
examples/  hello.c
```

## SDK 와 드라이버

설치와 사용법 전체는 [INSTALL.md](INSTALL.md) 에 있다.

```sh
make -C nxkernel nlibc-sdk                 # out/nlibc/{bin,include,lib}
out/nlibc/bin/nlcc -O2 -o hello.run hello.c
out/nlibc/bin/nlcc -O2 -c a.c b.c && out/nlibc/bin/nlld -o prog.run a.o b.o
```

`nlcc` 는 호스트 gcc/ld 를 부른다: `-nostdinc` + nlibc 헤더, 기본 `-std=c89`, 정적 비-PIE.
링크 순서는 `nlcrt0.o <오브젝트> <-l...> libnlibc.a libgcc.a`, 스크립트는 `lib/nlibc.ld`.
`-nostartfiles`, `-nostdlib`, `-Wl,...`, `-v` 를 지원하고 `-fPIC`/`-shared` 는 거부한다.
만든 `.run` 을 `fs:/usr/` 에 넣으면 nysh 에서 이름으로 실행된다 (이미지 빌드의 `--bin` 디렉터리).

## 검사

- `make test-nlibc` — 호스트에서 두 단계:
  1. `test/nlibc_test.c` 를 플랫폼 계층만 Linux 로 바꿔(`tests/host/nlibc/sys_linux.c`) 정적 바이너리로 실행 (9365개, POSIX 검사는 자기 자신을 fork/exec/spawn 해 확인)
  2. `tests/host/nlibc/fpdiff.c` — glibc 와 차분 퍼징: printf `%e %f %g` 문자열 일치, strtod 비트 일치
     (두 double 의 정확한 중간점과 그 ±1 포함), math 는 정확한 함수 비트 일치 / 초월 함수 1 ulp 이내.
     `FP_ITER=`, `SEED=` 로 조절
- `make test-e2e` (SELFTEST 이미지) — Nyxis 안에서 `nlibctest`, `hello`, `fputest` 를 실행

## 구현 메모

- **부동소수점**: 커널이 문맥 전환 때 프로세스마다 FXSAVE/FXRSTOR 한다 (`process.h` 의 `fpu_state`).
  그래서 nlibc 와 그 프로그램은 `-mgeneral-regs-only` 없이 빌드된다. `fputest` 가 이를 검사한다
  (저장을 빼면 실패하는 것까지 확인했다).
- **printf/strtod**: 정확한 큰 정수 연산이라 결과가 항상 올바르게 반올림된다 (동점은 짝수).
  long double 은 double 로 바꿔 출력/입력한다.
- **math**: sqrt floor ceil fabs fmod modf frexp ldexp 는 정확, 나머지는 x87 확장 정밀도로 1 ulp 이내.
  삼각 함수는 2/π 1280비트 표로 인자를 정확히 줄인다 (sin(1e22) 도 정확).
- **time**: 시간대는 UTC 하나 (`localtime == gmtime`). `time()` 은 커널의 `NX_CLOCK_REALTIME`
  (CMOS RTC, `kernel/timer/rtc`), `clock()` 은 프로그램 시작 후 경과 시간(프로세스별 CPU 시간이 없다).
- **signal**: 커널에 신호가 없어 `raise` 는 프로세스 안에서만 처리기를 부른다. CPU 예외는 처리기를 거치지 않는다.
- **locale**: "C" 로캘만.
- **임시 파일**: `tmp:/nl<pid>_<번호>`. `tmpfile` 로 연 파일은 `fclose` 또는 `exit` 때 지운다.

## POSIX 계층

C 표준 헤더 안의 POSIX 확장(`fileno`, `fdopen`, `kill`, `setenv`, `clock_gettime`, `nanosleep` ...)은 glibc 와 같은
규칙으로 보인다: `-std=c89` 같은 엄격 모드에서는 `_POSIX_C_SOURCE` (또는 `_NYX_SOURCE`) 를 정의해야 한다.
`unistd.h` 처럼 POSIX 에만 있는 헤더는 항상 모든 선언이 보인다.

- **fd** = Nyxis 핸들의 슬롯 번호. 커널이 가장 낮은 빈 슬롯을 주므로 "가장 낮은 fd" 규칙이 지켜진다.
  플랫폼 계층의 fd 표가 세대까지 담긴 전체 핸들과 열기 플래그를 들고 있다. 표에 없는 fd(상속, exec 뒤)는
  "세대 0 핸들" 로 확인해 등록한다. `FILE` 은 fd 를 들고 입출력마다 핸들을 찾으므로 `dup2` 로 바꾼 fd 를 따라간다.
  dup/dup2/fork 는 열린 파일(오프셋)을 공유한다. `O_APPEND` 는 커널의 `NX_O_APPEND` 로 열린 파일에 기억되어 dup/fork/exec 에도 이어진다 (셸의 `>>`).
- **경로와 현재 디렉터리**: `이름공간:/...` 는 절대 경로, 나머지는 현재 디렉터리 기준. 커널에 현재 디렉터리가
  없어서 nlibc 가 들고 있다 (시작값: 환경 변수 `PWD`, 없으면 `face:/`). `.` `..` 는 nlibc 가 정리한다
  (뿌리 위로는 올라가지 않는다). `fopen`, `remove`, `rename` 도 상대 경로를 받는다.
- **프로세스**: `fork` 는 부모 메모리를 즉시 복사한다(커널에 쓰기 시 복사가 아직 없다) — 곧바로 exec 할 거라면
  `posix_spawn` 이 훨씬 싸다. exec 류와 `posix_spawnp("ls")` 는 이름만 주면 `app:/ls.run`.
  자식은 FD_CLOEXEC 가 아닌 fd 를 같은 번호로, 현재 디렉터리는 `PWD` 로 물려받는다.
  `posix_spawn` 은 파일 동작이 없으면 커널의 상속 플래그로 바로 만들고, 있으면 fork -> 동작 적용 -> exec 하며
  exec 실패는 CLOEXEC 파이프로 받아 반환값으로 돌려준다.
  pid -> 핸들 표로 `waitpid` 를 구현하고(동시에 16개), `waitpid(-1)` 은 `NxPoll` 의 `NX_POLL_EXIT` 로 기다린다.
  대기 상태는 POSIX 형식: 강제 종료면 `SIGKILL`, CPU 예외면 `SIGSEGV` 로 끝난 것으로 보인다.
- **kill**: 자기 자신(`raise`) 또는 자기 자식에게만. 자식에게 보내는 신호는 모두 강제 종료다.
- **system**: 명령 처리기 대신 nlibc 가 한 줄을 나눠(공백, `'...'`, `"..."`, `\`) `posix_spawnp` 로 실행한다.
  파이프/리디렉션/변수 치환은 없다. 실행할 수 없으면 "127 로 종료" 한 상태를 돌려준다.
- **stat/chmod/umask**: 종류, 크기, 권한 비트, 소유자는 커널이 알려 준 그대로(시각은 0). chmod 는 소유자나 root 만,
  0777 만 (setuid 류 없음). umask 는 커널이 들고 있어 fork/spawn/exec 에 이어진다. x 비트 없는 파일은 exec 할 수 없다.
- **표준 스트림**: `printf` 등은 fd 1 에 `NxWrite` 하고, 커널은 콘솔 핸들의 쓰기를 터미널 장치로 바로 보낸다
  (커널 로그 printk 를 거치지 않는다). `fopen("dev:/stdout", "w")`, `open("dev:/stdin", O_RDONLY)` 처럼 장치 이름으로도
  열 수 있다 — 리눅스의 `/dev/stdout` 처럼 지금의 0/1/2 번(리디렉션된 파일/파이프 포함)을 가리킨다. 다만 파일이면
  새로 여는 대신 dup 처럼 오프셋을 공유하고, `"w"` 로 열어도 자르지 않는다. 터미널 자체는 `dev:/tty`.
- **파이프**: 읽는 쪽이 없으면 쓰기가 `EPIPE` (SIGPIPE 는 없다). 4KiB 버퍼, 512바이트 이하 쓰기는 섞이지 않는다.
- **없는 것**: chown, setuid 류, 프로세스 그룹, 진짜 신호, 쓰기 시 복사(fork 가 느리다).

## C99 확장

C89 라이브러리지만 흔히 쓰이는 C99 기능을 함께 둔다 (`-std=c99` 이상, 또는 POSIX 확장이 보일 때 선언된다):
`stdint.h`(최소/빠른/최대 폭 정수, `INTn_C`), `inttypes.h`(`PRId64` …), `va_copy`, `snprintf`/`vsnprintf`,
`strtoll` `strtoull` `atoll` `llabs` (long long = long, 64비트), `strtof` `strtold`(double 정밀도), `ldexpl`(x87, 80비트)
`fabsl`, `_Exit`. POSIX: `realpath`, `gettimeofday`(`sys/time.h`). Nyxis 안의 컴파일러(userland/nlcc, TinyCC)가 쓴다.

## 알려진 제약 (커널 기능 필요)
- `O_CREAT`: 만들기와 열기가 따로라서 흉내 낸다. 읽기 전용 모드(0444 등)로 새로 만든 파일도 POSIX 처럼
  쓰기로 열리도록, 소유자 읽기/쓰기를 더해 만들고 연 뒤 모드를 되돌린다 (그 사이 아주 잠깐 모드가 다르다).
- `fopen("w")` 의 자르기: NxOpen 에 만들기/자르기 플래그가 없어 지우고 다시 만든다 (모드/ACL 이 기본값으로 돌아간다).
- `stat` 의 시각: nyfs 의 mtime 을 아직 NxStat 으로 내보내지 않는다.
- 신호, 프로세스 그룹: 커널에 해당 기능이 없다.
