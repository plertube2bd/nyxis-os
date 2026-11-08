# nlcc 설치 가이드

`nlcc`는 리눅스 호스트에서 **Nyxis용 C 프로그램**을 만드는 컴파일러 드라이버입니다.
호스트의 gcc와 ld를 그대로 쓰면서, Nyxis의 C 라이브러리(nlibc) 헤더, 시작 코드, 라이브러리, 링커 스크립트를 자동으로 붙여 줍니다.
결과물은 Nyxis 커널이 바로 실행할 수 있는 정적 ELF 실행 파일(`.run`)입니다.

- `nlcc` : 컴파일 + 링크 (`gcc`처럼 씀)
- `nlld` : 링크만 (`nlcc`와 같은 파일, 이름만 다름)

> Nyxis **안에서** 컴파일하려면 이미지에 들어 있는 `nlcc`(TinyCC 기반)를 쓰면 됩니다. 이 가이드는 필요 없습니다.
> [userland/nlcc/README.md](../nlcc/README.md) 참고.

---

## 1. 준비물

| 도구 | 최소 버전 | 쓰는 곳 |
|---|---|---|
| x86_64 리눅스 호스트 | — | 컴파일러가 x86_64 ELF를 만들어야 함 |
| GCC | 8 이상 | 컴파일 (`-fcf-protection=none` 필요) |
| GNU binutils (`ld`, `as`, `ar`) | **2.39 이상** | 링크 (`--no-warn-rwx-segments` 옵션 필요) |
| Python | 3.7 이상 | `nlcc` 자체가 Python 스크립트 |
| GNU make | — | SDK 빌드 |

버전 확인:

```sh
gcc --version | head -1
ld --version | head -1
python3 --version
```

> Arch 리눅스 기준 `base-devel` 그룹이면 모두 들어 있습니다. Debian/Ubuntu는 `build-essential python3`.

---

## 2. SDK 빌드

저장소 최상위에서:

```sh
make -C nxkernel nlibc-sdk
```

`out/nlibc/` 아래에 SDK가 만들어집니다:

```
out/nlibc/
├── bin/
│   ├── nlcc          컴파일러 드라이버
│   └── nlld -> nlcc  링크 전용 이름
├── include/          C89 표준 헤더 15개, POSIX 헤더(unistd.h fcntl.h dirent.h spawn.h sys/*.h),
│                     stdint.h, nyx_abi.h (커널 ABI)
└── lib/
    ├── libnlibc.a    C 라이브러리
    ├── nlcrt0.o      프로그램 시작 코드 (_start)
    └── nlibc.ld      링커 스크립트
```

> 빌드 출력 위치를 바꿨다면(`make BUILD=...`) SDK도 `<BUILD>/nlibc/`에 생깁니다.

---

## 3. 설치

SDK는 **위치를 옮겨도 동작**합니다. `nlcc`가 자기 파일의 실제 위치(심볼릭 링크를 따라간 위치)에서
`../include`와 `../lib`를 찾기 때문입니다. 셋 중 편한 방법을 고르세요.

### 방법 A — 그 자리에서 쓰기 (가장 간단)

```sh
export PATH="$PWD/out/nlibc/bin:$PATH"     # ~/.bashrc 등에 넣으면 계속 유지
```

fish 셸이라면:

```fish
fish_add_path (pwd)/out/nlibc/bin
```

### 방법 B — 시스템 위치로 복사

```sh
sudo cp -r out/nlibc /opt/nlibc
sudo ln -sf /opt/nlibc/bin/nlcc /usr/local/bin/nlcc
sudo ln -sf /opt/nlibc/bin/nlld /usr/local/bin/nlld
```

`/usr/local/bin`에 링크만 걸어도 `nlcc`가 `/opt/nlibc`를 찾아갑니다.

### 방법 C — 환경 변수로 SDK 위치 지정

`nlcc` 파일만 따로 복사해서 쓰는 경우처럼, SDK가 `nlcc`의 `../`에 없을 때:

```sh
export NLIBC_SYSROOT=/path/to/nlibc     # include/ 와 lib/ 가 있는 디렉터리
```

> SDK를 다시 빌드하면(nlibc 소스를 고친 뒤 `make nlibc-sdk`) 방법 A는 바로 반영되고,
> 방법 B는 다시 복사해야 합니다.

---

## 4. 설치 확인

```sh
nlcc -o hello.run userland/nlibc/examples/hello.c
file hello.run
```

다음처럼 나오면 정상입니다:

```
hello.run: ELF 64-bit LSB executable, x86-64, version 1 (SYSV), statically linked, not stripped
```

실제로 실행되는 명령을 보려면 `-v`를 붙입니다.

> `hello.run`은 Nyxis 시스템 콜을 쓰므로 **리눅스에서는 실행되지 않습니다.** 실행은 6절을 보세요.

---

## 5. 사용법

```sh
nlcc -O2 -o prog.run main.c util.c                 # 한 번에
nlcc -O2 -c main.c && nlcc -O2 -c util.c           # 따로 컴파일
nlld -o prog.run main.o util.o                     # 링크만
nlcc -O2 -o prog.run main.c -L./mylibs -lfoo       # 추가 정적 라이브러리 (libfoo.a)
```

### 기본으로 붙는 것

- 헤더: 호스트 헤더 대신 SDK의 `include/`만 씀 (`-nostdinc`)
- 표준: `-std=c89`. 다른 표준을 원하면 직접 지정합니다(예: `-std=gnu99`)
- 코드 생성: 정적, 비-PIE, 스택 보호기 끔 (Nyxis가 요구하는 형식)
- 링크 순서: `nlcrt0.o` → 내 오브젝트 → `-l` 라이브러리 → `libnlibc.a` → `libgcc.a`

### 옵션

| 옵션 | 뜻 |
|---|---|
| `-o <파일>` | 출력 파일 (링크 기본값 `a.run`) |
| `-c` / `-S` / `-E` | 컴파일만 / 어셈블리 출력 / 전처리만 |
| `-l<이름>` `-L<경로>` | 정적 라이브러리 추가 |
| `-Wl,<a>,<b>` | 링커에 그대로 전달 |
| `-nostartfiles` | `nlcrt0.o`를 빼고 링크 (`_start`를 직접 작성할 때) |
| `-nostdlib` | 시작 코드, nlibc, libgcc 모두 빼고 링크 |
| `--nlibc-sysroot=<경로>` | SDK 위치 지정 (`NLIBC_SYSROOT`와 같음) |
| `-v` | 실행하는 gcc/ld 명령 출력 |
| 그 밖의 옵션 | gcc에 그대로 전달 (`-O2`, `-Wall`, `-D`, `-I`, `-g` …) |

`-fPIC`, `-fPIE`, `-shared`, `-pie`는 거부합니다(Nyxis는 정적 실행 파일만 실행). `-static`은 무시합니다.

### 환경 변수

| 변수 | 기본값 | 뜻 |
|---|---|---|
| `NLIBC_SYSROOT` | `nlcc`의 `../` | SDK 위치 |
| `NLCC_CC` | `gcc` | 호스트 C 컴파일러 |
| `NLCC_LD` | `ld` | 호스트 링커 |

---

## 6. Nyxis에서 실행하기

루트 파일시스템 이미지를 만들 때 빌드 디렉터리의 `usr/`에 있는 `*.run`이 모두 `fs:/usr/`(= `app:/`)에 들어갑니다.
그곳의 프로그램은 셸(nysh)에서 이름만으로 실행됩니다.

```sh
cp prog.run out/usr/
rm -f out/initrd.img          # 직접 넣은 파일은 make 가 변경을 감지하지 못하므로 이미지를 다시 만들게 한다
make -C nxkernel os           # (테스트 이미지라면 SELFTEST=1)
make -C nxkernel run
```

로그인한 뒤:

```
test@nyxis$ prog
```

- 인자는 그대로 전달됩니다 (`prog a b` → `argv = {"prog", "a", "b"}`).
- 이미지의 루트 파일시스템은 8MiB입니다(`ROOTFS_SIZE`). 프로그램이 크면 늘려야 합니다.

---

## 7. 문제 해결

| 증상 | 원인과 해결 |
|---|---|
| `nlcc: nlibc SDK not found at ...` | SDK를 아직 빌드하지 않았거나 위치가 다름. `make -C nxkernel nlibc-sdk` 후 3절 참고, 또는 `NLIBC_SYSROOT` 설정 |
| `ld: unrecognized option '--no-warn-rwx-segments'` | binutils가 2.39보다 오래됨. binutils를 올리거나 `NLCC_LD`로 새 ld 지정 |
| `fatal error: xxx.h: No such file or directory` | nlibc에 없는 헤더(`pthread.h`, `sys/socket.h`, `termios.h` 등). 있는 헤더는 `out/nlibc/include/` 참고 |
| `implicit declaration of function 'fileno'` (또는 `kill`, `setenv` …, `-Wall` 일 때의 경고) | `-std=c89` 엄격 모드에서는 C 표준 헤더 안의 POSIX 확장이 숨겨짐. `-D_POSIX_C_SOURCE=200809L`을 주거나 `-std=gnu89` 사용 |
| `ISO C90 does not support 'long long'` | 기본 표준이 C89라서 그럼. `-std=gnu99` 등을 지정하거나 `-pedantic`을 빼기 |
| `undefined reference to ...` | nlibc에 없는 함수(chown, sigaction, pthread 등). 지원 범위는 [README.md](README.md)의 "POSIX 계층" 참고. 필요하면 `nyx_abi.h`의 시스템 콜을 직접 사용 |
| 리눅스에서 실행하면 바로 죽음 | 정상. Nyxis 시스템 콜을 쓰는 실행 파일이라 Nyxis(QEMU)에서만 실행됨 |
| nysh에서 `command not found` | 이미지에 안 들어감. 6절대로 `out/usr/`에 넣고 이미지를 다시 만들었는지 확인 |

---

## 8. 제거

```sh
sudo rm /usr/local/bin/nlcc /usr/local/bin/nlld    # 방법 B로 설치했다면
sudo rm -r /opt/nlibc
```

방법 A는 PATH 설정만 지우면 됩니다. `out/nlibc/`는 `make -C nxkernel clean`으로 함께 지워집니다.

---

nlibc가 무엇을 지원하는지와 알려진 제약은 [README.md](README.md)를 보세요.
