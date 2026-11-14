# Nyxis OS

> **Notice — name and identity**
>
> - This project, **Nyxis OS** (<https://github.com/plertube2bd/nyxis-os>), is **not the same project as <https://nyxisos.it>**
>   and is not affiliated with it in any way.
> - The name is written **"Nyxis OS"**, with a space between *Nyxis* and *OS*.
> - This matter has already been settled by mutual agreement. **No further contact about it will be accepted.**
>
> **알림 — 이름과 정체**
>
> - 이 프로젝트 **Nyxis OS** 는 **<https://nyxisos.it> 와 다른 프로젝트**이며, 어떤 관계도 없습니다.
> - 이름은 **"Nyxis OS"** 로, *Nyxis* 와 *OS* 사이에 띄어쓰기가 있습니다.
> - 이 문제는 이미 서로 합의를 마쳤습니다. **이에 관한 추가 연락은 받지 않습니다.**

**Languages:** [English](#english) · [한국어](#한국어)

> **Most documentation in this repository is written in Korean.** That includes the files in `docs/`, the other
> READMEs, configuration file headers and source code comments. This English section covers the essentials; for
> details, follow the links to the Korean documents. Commands, file names and code are the same in either language.

---

# English

## About

Nyxis OS is an experimental, hobby x86-64 operating system written from scratch, with user experience and
consistency at its center. The name comes from *Nyx*, the Greek goddess of the night, plus the suffix *-is*, giving the
feel of "a tendency toward something." It prefers a simple, consistent design over legacy compatibility: everything is
a handle, paths are written `namespace:/path`, and system features go through one central API (nx64api, NWP).

What is in it today:

| Part | Contents |
|---|---|
| Bootloader | UEFI bootloader NYTB (`bootloader/nytb`); GRUB (Multiboot2) boot also works |
| Kernel | `nxkernel/`: preemptive multitasking, user mode, handles and rights, namespaces, signals, process groups, pipes, channels (IPC), pseudo-terminals, shared memory, authentication (PBKDF2) |
| Drivers | `drivers/`: AHCI (SATA), ATA, RAM disk, PIC, its own file system *nyfs*, VFS |
| C library | nlibc (C89 standard library + basic POSIX layer) and the Nyxis-specific nx64api |
| System | init (with a service manager), login, elev (privilege elevation), journald (system journal) |
| Commands | nysh (shell), coreutils, kilo (editor), nlcc (a C compiler that runs inside Nyxis, based on TinyCC), nex, journalman, ntop, sysmon, ... |
| Desktop | ntui (a compositor) with window programs nterm (terminal), nfiles (files), nclock (clock), nkeys (shortcut settings) |

> ⚠️ Nyxis OS is an experimental hobby project. Do not use it where production use, security or automation matter.
> Running it in QEMU is recommended over real hardware.

## Quick start

```bash
git clone https://github.com/plertube2bd/nyxis-os.git
cd nyxis-os/nxkernel
make users     # create login accounts (once)
make run       # build everything and boot it in QEMU
```

After boot you get a login screen. Log in with an account made by `make users` and the desktop (ntui) starts.

## Building

The full guide, including troubleshooting, is [docs/INSTALL.md](docs/INSTALL.md) (Korean).

**1. Requirements.** Build on an x86_64 Linux host. External sources (TinyCC, kilo) are already in `third_party/`.

| Purpose | Tools |
|---|---|
| Compiling | `gcc`, `binutils` (`as`, `ld`, `objcopy`), `make`, `patch` |
| UEFI bootloader | `gnu-efi` (3.x and 4.x both work) |
| Disk images | `dosfstools` (`mkfs.vfat`), `mtools` (`mmd`, `mcopy`) |
| Build scripts | `python3` |
| Running | `qemu-system-x86_64`, OVMF (UEFI firmware for QEMU) |

```bash
# Arch Linux
sudo pacman -S --needed base-devel gnu-efi dosfstools mtools python qemu-system-x86 edk2-ovmf
# Ubuntu / Debian
sudo apt install build-essential gnu-efi dosfstools mtools python3 qemu-system-x86 ovmf
```

If your distribution puts files elsewhere, pass them as variables: `OVMF`, `OVMF_VARS` (firmware), `EFIINC`, `EFILIB`,
`CRT0` (gnu-efi). Example: `make OVMF=/path/OVMF_CODE.fd OVMF_VARS=/path/OVMF_VARS.fd run`

**2. Accounts.** Run `make users` in `nxkernel/`. It asks for a name, uid and password. Create the administrator `root`
(uid 0) and an everyday account (uid 1000 and up). The result goes to `config/users.db`, which stores only salted
hashes and is not tracked by git. Rebuild the image after changing accounts.

**3. Build and run.** Run every command in `nxkernel/` (or `make -C nxkernel ...` from the repository root).

| Command | What it does |
|---|---|
| `make run` | Build everything and run in QEMU (GTK window; serial output in your terminal) |
| `make run-nographic` | Serial only, no display (e.g. over SSH). Quit with `Ctrl-A` then `X` |
| `make os` | Build the boot image only (`out/esp.img`) |
| `make run-sata` | Boot over SATA (port 0 = ESP, port 1 = `disk.img`) |
| `make iso` / `make run-grub-uefi` / `make run-grub-bios` | GRUB boot ISO (needs `grub-mkrescue`, `xorriso`) |
| `make clean` | Remove build output (`out/`). `disk.img` and `config/` are kept |
| `make nlibc-sdk` | SDK for building Nyxis programs on the host (`out/nlibc/bin/nlcc`) |

The first build compiles the bootloader, kernel, nlibc, commands, shell and nlcc, so it takes a few minutes. Output goes
to `out/` (`out/esp.img` is the boot disk, `out/usr/*.run` are the programs). `build.sh` in the repository root runs
`make -B os` and copies the image to `out/nyxis-os`.

Build options: `RES=WxH` (screen resolution, default `1600x900`), `SELFTEST=1` (kernel self-test + test accounts with
random passwords), `DEBUG=1` (bootloader framebuffer test pattern), `DEBUG_SYSCALLS=1` (debug system calls 768–1023),
`DISK_SIZE=256M` (size of the `disk.img` made by `make disk-reset`, default 64M).

**4. Storage.** The root file system (including the home directory `face:/` and the settings in `roi:/`) is loaded fresh
from the image at every boot. Keep files you want to survive a reboot in `disk:/`. `make run` creates `disk.img` in the
repository root if it is missing and attaches it as a second disk; `make disk-reset` empties it and creates it again.

**5. Tests.**

```bash
make check                         # strict C89 compile (-O0/-O2)
make test-host                     # FAT16/VFS/RAM disk host tests (+ ASan/UBSan fuzzing)
make test-nlibc                    # C library (including floating-point differential tests against glibc)
make test-coreutils                # commands and shell, on Linux
make test-ntui                     # terminal emulator, shortcut parser
make clean && make SELFTEST=1 os   # image for end-to-end tests
make test-e2e                      # boot in QEMU -> log in -> shell -> commands -> power off (serial and PS/2)
make test-disk                     # disk:/ survives a reboot
```

Always run `make clean` when switching between `SELFTEST=1` and normal images. The Makefile does not track compiler
option changes, so mixing them produces an image without the self-test. Use `BUILD=<dir>` to keep them apart.

**6. Real hardware (experimental).** Writing `out/esp.img` to a USB stick may boot on a UEFI PC (turn off Secure Boot,
use AHCI mode; NVMe is not supported). This is untested on real hardware; see section 6 of
[docs/INSTALL.md](docs/INSTALL.md).

## Using the desktop (ntui)

After login, the default login shell **ntui** takes over the screen and keyboard and becomes the desktop: a taskbar at
the bottom and, at first, a help window. Everything is done with the keyboard.

> **Super (Windows) key in QEMU:** your host desktop may take the Super key first. Press `Ctrl+Alt+G` in the QEMU window
> to grab the keyboard so that Super reaches Nyxis. There are also shortcuts without Super (`Alt+F1`, `Ctrl+Alt+T`, ...).

| Keys | Action |
|---|---|
| `Super` (press and release), `Super+Q`, `Alt+F1` | Menu (apps, theme, help, log out, power off) |
| `Super+Enter`, `Super+T`, `Ctrl+Alt+T` | New terminal window |
| `Super+E` | File manager |
| `Super+R`, `Alt+F2` | Run box (run a command) |
| `F1`, `Super+/` | Help (the current shortcut list) |
| `Super+I` | Shortcut settings window (record new keys) |
| `Super+L`, `Ctrl+Alt+L` | Lock the screen |
| `Ctrl+Shift+Esc`, `Ctrl+Alt+Delete` | Process manager (ntop) |
| `Print` | Screenshot |
| `Super+G` | Change theme |
| `Alt+F4`, `Super+W` | Close window |
| `Alt+Tab` / `Alt+Shift+Tab`, `Super+Tab` | Switch windows / task view |
| `Super+↑` / `Super+↓` / `Super+←` / `Super+→` | Maximize / restore or minimize / left or right half |
| `F11` | Fullscreen |
| `Alt+Space` | Window menu (move, size, ...). `Alt+F7` move, `Alt+F8` resize (arrow keys, Enter to finish) |
| `Super+D`, `Super+M` | Show desktop, minimize all |
| `Super+1` … `Super+9` | Go to window n |
| In a terminal: `Ctrl+Shift+T`, `Ctrl+Shift+W`, `Shift+PgUp/PgDn` | New terminal, close, scroll back |

All shortcuts live in one file (`app:/ntui.run/keys`; your own copy is `roi:/<uid>/app/ntui/keys`). Change them in the
`Super+I` window or any editor, then reload with `Super+Shift+R`. The format is described at the top of
[rootfs/app/ntui/keys](rootfs/app/ntui/keys).

- **Terminal (nterm)** runs the shell `nysh`; full-screen programs such as `ntop`, `sysmon` and `kilo` run inside it.
  Typing `nfiles` in a terminal opens a new window.
- **Files (nfiles)**, **Clock (nclock)** and **Shortcuts (nkeys)** open from the menu or with shortcuts.
- Programs in a terminal can control their own window: `nwin size 100x30`, `nwin center`, `nwin full`
  (from C, the `nx64_win_*` functions in nx64api, [docs/NX64API.md](docs/NX64API.md)).
- Settings such as the startup window and theme are listed in [rootfs/app/README.md](rootfs/app/README.md).
- **Log out** in the menu returns to the login screen; **Power off** turns the machine off (both ask once more). In a
  terminal, `poweroff` also works.
- **Without a display** (serial, `make run-nographic`), ntui draws as text on the current terminal and uses `Ctrl`
  shortcuts instead of Super: `Ctrl+Q` menu, `Ctrl+T` terminal, `Ctrl+E` files, `Ctrl+R` run, `Ctrl+W` close,
  `Ctrl+F` maximize, `Ctrl+N`/`Ctrl+P` switch windows. To use only the shell, as root run
  `echo app:/nysh.run > roi:/<uid>/shell` (that user's login shell).

## Getting around

Paths are written `namespace:/path`.

| Namespace | Contents |
|---|---|
| `face:/` | Your home directory |
| `app:/` | Installed programs (`app:/ls.run`, ...; the shell runs them by name) |
| `tmp:/` | Temporary space in RAM (gone after a reboot) |
| `disk:/` | Persistent space on disk |
| `roi:/` | Settings registry (one value per file; yours is `roi:/<uid>/`) |
| `info:/` | System information from the kernel (read-only) |
| `dev:/`, `svc:/` | Devices, service channels |

```
nyfetch                     system information
ls app:/                    installed programs
kilo face:/memo.txt         text editor
nlcc -o hello.run app:/src/examples/hello.c && ./hello.run     compile C inside Nyxis
elev cat roi:/hostname      run as root (allowed users, password check)
ntop                        processes
nex                         services (nex status journald, elev nex restart <unit>)
journalman -b               log of this boot (journalman -f to follow)
poweroff                    power off
```

## Documentation (Korean)

These documents are written in Korean. The full list with descriptions is in the Korean section below
([문서와 README 위치](#문서와-readme-위치)).

| Document | Topic |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | Building and running on Linux |
| [docs/NX64API.md](docs/NX64API.md) | nx64api: TUI, system, settings, services, journal, window control, display |
| [docs/NWP.md](docs/NWP.md) | NWP (Nyxis Window Protocol) between the compositor and window programs |
| [userland/README.md](userland/README.md) | User programs and how to add one |
| [nxkernel/kernel/README.md](nxkernel/kernel/README.md) | Kernel components (this one also has an English part) |
| [rootfs/app/README.md](rootfs/app/README.md) | Default app settings |

## License

GPL-3.0 ([LICENSE](LICENSE)). Code in `third_party/` keeps its own license (TinyCC: LGPL-2.1 or later, kilo:
BSD-2-Clause). Rules for importing external code: [docs/LICENSING.md](docs/LICENSING.md) (Korean).

---

# 한국어

> 이 저장소의 문서(`docs/`, 각 README, 설정 파일 머리말, 소스 주석)는 대부분 한국어로 쓰여 있습니다.

## 소개

Nyxis OS 는 사용자 경험과 일관성을 중심에 두고 처음부터 만들고 있는 x86-64 운영체제입니다. 이름은 밤의 여신
*Nyx* 에 "~하는 경향" 이라는 느낌의 *-is* 를 붙인 것입니다. 레거시 호환보다 단순하고 일관된 설계를 고릅니다:
모든 것은 핸들, 경로는 `이름공간:/경로`, 시스템 기능은 중앙 API(nx64api, NWP) 하나로.

지금 들어 있는 것:

| 부분 | 내용 |
|---|---|
| 부트로더 | UEFI 부트로더 NYTB (`bootloader/nytb`), GRUB(Multiboot2) 부팅도 가능 |
| 커널 | `nxkernel/`: 선점형 멀티태스킹, 사용자 모드, 핸들/권한, 이름공간, 신호, 프로세스 그룹, 파이프, 채널(IPC), 의사 터미널, 공유 메모리, 인증(PBKDF2) |
| 드라이버 | `drivers/`: AHCI(SATA), ATA, RAM 디스크, PIC, 자체 파일시스템 nyfs, VFS |
| C 라이브러리 | nlibc (C89 표준 + POSIX 기본 계층) 와 Nyxis 전용 API nx64api |
| 시스템 | init(서비스 관리자), login, elev(권한 상승), journald(저널) |
| 명령어 | nysh(셸), coreutils, kilo(편집기), nlcc(Nyxis 안의 C 컴파일러, TinyCC), nex, journalman, ntop, sysmon ... |
| 데스크톱 | ntui (컴포지터) 와 창 프로그램 nterm(터미널), nfiles(파일), nclock(시계), nkeys(단축키 설정) |

> ⚠️ Nyxis OS 는 실험/취미 프로젝트입니다. 실서비스, 보안, 자동화가 중요한 환경에서는 쓰지 마세요.
> 실제 PC 보다 QEMU 에서 실행하는 것을 권장합니다.

---

## 빠른 시작

```bash
git clone https://github.com/plertube2bd/nyxis-os.git
cd nyxis-os/nxkernel
make users     # 로그인 계정 만들기 (처음 한 번)
make run       # 모두 빌드하고 QEMU 로 부팅
```

부팅하면 로그인 화면이 나옵니다. `make users` 로 만든 계정으로 들어가면 데스크톱(ntui)이 뜹니다.

---

## 빌드 자세히

자세한 설명과 문제 해결은 [docs/INSTALL.md](docs/INSTALL.md) 에 있습니다. 여기에는 순서와 자주 쓰는 것만 둡니다.

### 1. 필요한 것

리눅스(x86_64) 호스트에서 빌드합니다. 외부 소스(TinyCC, kilo)는 `third_party/` 에 들어 있어 따로 받을 것이 없습니다.

| 용도 | 도구 |
|---|---|
| 컴파일 | `gcc`, `binutils`(`as`, `ld`, `objcopy`), `make`, `patch` |
| UEFI 부트로더 | `gnu-efi` (3.x, 4.x 모두 동작) |
| 디스크 이미지 | `dosfstools`(`mkfs.vfat`), `mtools`(`mmd`, `mcopy`) |
| 빌드 스크립트 | `python3` |
| 실행 | `qemu-system-x86_64`, OVMF (QEMU 용 UEFI 펌웨어) |

```bash
# Arch Linux
sudo pacman -S --needed base-devel gnu-efi dosfstools mtools python qemu-system-x86 edk2-ovmf
# Ubuntu / Debian
sudo apt install build-essential gnu-efi dosfstools mtools python3 qemu-system-x86 ovmf
```

배포판의 파일 위치가 다르면 변수로 넘깁니다: `OVMF`, `OVMF_VARS` (펌웨어), `EFIINC`, `EFILIB`, `CRT0` (gnu-efi).
예: `make OVMF=/경로/OVMF_CODE.fd OVMF_VARS=/경로/OVMF_VARS.fd run`

### 2. 계정 만들기

```bash
cd nxkernel
make users
```

이름, uid, 비밀번호를 차례로 묻습니다. 관리자 `root`(uid 0)와 평소에 쓸 계정(uid 1000 부터)을 만들어 두세요.
결과는 `config/users.db` 에 솔트를 친 해시로만 저장되고 git 에는 올라가지 않습니다. 계정을 바꾸면 이미지를 다시 빌드합니다.

### 3. 빌드하고 실행하기

모든 명령은 `nxkernel/` 에서 실행합니다 (저장소 루트에서는 `make -C nxkernel ...`).

| 명령 | 내용 |
|---|---|
| `make run` | 모두 빌드하고 QEMU 로 실행 (GTK 창 + 시리얼은 지금 터미널) |
| `make run-nographic` | 화면 없이 시리얼만으로 (SSH 등). 끝내기: `Ctrl-A` 다음 `X` |
| `make os` | 실행하지 않고 부팅 이미지만 (`out/esp.img`) |
| `make run-sata` | SATA 로 부팅 (포트 0 = ESP, 포트 1 = `disk.img`) |
| `make iso` / `make run-grub-uefi` / `make run-grub-bios` | GRUB 부팅 ISO (`grub-mkrescue`, `xorriso` 필요) |
| `make clean` | 빌드 결과(`out/`)를 지운다. `disk.img` 와 `config/` 는 남는다 |
| `make nlibc-sdk` | 호스트에서 Nyxis 프로그램을 만드는 SDK (`out/nlibc/bin/nlcc`) |

처음 빌드는 부트로더, 커널, nlibc, 명령어, 셸, nlcc(Nyxis 안의 C 컴파일러)까지 모두 만들어서 몇 분 걸립니다.
결과는 `out/` 에 생깁니다 (`out/esp.img` = 부팅 디스크, `out/usr/*.run` = 프로그램). 저장소 루트의 `build.sh` 는
`make -B os` 를 부르고 이미지를 `out/nyxis-os` 로 복사합니다.

빌드 옵션:

| 옵션 | 내용 |
|---|---|
| `RES=WxH` | 화면 해상도 (기본 `1600x900`). 예: `make RES=1280x800 run` |
| `SELFTEST=1` | 부팅 뒤 커널 자체 점검 + 테스트 계정(root/test, 무작위 비밀번호)의 테스트 이미지 |
| `DEBUG=1` | 부트로더 프레임버퍼 테스트 패턴 |
| `DEBUG_SYSCALLS=1` | 디버그 시스템 콜(768~1023)을 넣는다 |
| `DISK_SIZE=256M` | `make disk-reset` 으로 만들 `disk.img` 의 크기 (기본 64M) |

### 4. 저장 공간

루트 파일시스템(`fs:/`, 홈 `face:/`, 설정 `roi:/` 포함)은 부팅할 때마다 이미지에서 새로 읽습니다. 남겨야 하는 파일은
`disk:/` 에 두세요. `make run` 은 저장소 루트에 `disk.img` 가 없으면 만들어 두 번째 디스크로 붙입니다.

```bash
make disk-reset                  # disk.img 를 비우고 다시 만든다 (내용이 모두 사라진다)
```

### 5. 검사

```bash
make check                         # C89 엄격 컴파일 (-O0/-O2)
make test-host                     # FAT16/VFS/램디스크 호스트 검사 (+ ASan/UBSan 퍼징)
make test-nlibc                    # C 라이브러리 (glibc 와 부동소수점 차분 비교 포함)
make test-coreutils                # 명령어와 셸을 리눅스에서
make test-ntui                     # 터미널 에뮬레이터, 단축키 파서
make clean && make SELFTEST=1 os   # 종단 간 검사용 이미지
make test-e2e                      # QEMU 로 부팅 -> 로그인 -> 셸 -> 명령들 -> 전원 끄기 (시리얼, PS/2 두 경로)
make test-disk                     # disk:/ 가 재부팅 뒤에도 남는지
```

`SELFTEST=1` 이미지와 일반 이미지를 오갈 때는 꼭 `make clean` 을 먼저 하세요. Makefile 이 컴파일 옵션의 변화를
추적하지 않아서, 섞어 빌드하면 자체 점검이 빠진 이미지가 만들어집니다. 일반 빌드와 따로 두려면 `BUILD=<디렉터리>` 를 줍니다.

### 6. 실제 PC (실험적)

`out/esp.img` 를 USB 에 기록하면 UEFI PC 에서 부팅을 시도할 수 있습니다 (Secure Boot 끄기, 저장 장치는 AHCI 모드,
NVMe 미지원). 실제 하드웨어에서는 검증되지 않았습니다. 방법과 주의점은 [docs/INSTALL.md](docs/INSTALL.md) 6절.

---

## 데스크톱 (ntui) 쓰기

로그인하면 기본 로그인 셸인 **ntui** 가 화면과 키보드를 가져와 데스크톱이 됩니다. 아래쪽에 작업 표시줄, 처음에는
도움말 창이 뜹니다. 모든 것은 키보드로 합니다.

> **QEMU 에서 Super(Windows) 키:** 호스트의 데스크톱이 Super 키를 먼저 가져갈 수 있습니다. QEMU 창에서
> `Ctrl+Alt+G` 로 키보드를 잡으면 Super 키가 Nyxis 로 갑니다. Super 없이 쓰는 단축키(`Alt+F1`, `Ctrl+Alt+T` 등)도 있습니다.

### 기본 단축키

| 키 | 동작 |
|---|---|
| `Super` (눌렀다 떼기), `Super+Q`, `Alt+F1` | 메뉴 (앱, 테마, 도움말, 로그아웃, 전원 끄기) |
| `Super+Enter`, `Super+T`, `Ctrl+Alt+T` | 새 터미널 창 |
| `Super+E` | 파일 탐색기 |
| `Super+R`, `Alt+F2` | 실행 상자 (명령 실행) |
| `F1`, `Super+/` | 도움말 (지금 단축키 목록) |
| `Super+I` | 단축키 설정 창 (키를 녹음해 바꾼다) |
| `Super+L`, `Ctrl+Alt+L` | 화면 잠금 |
| `Ctrl+Shift+Esc`, `Ctrl+Alt+Delete` | 프로세스 관리 (ntop) |
| `Print` | 스크린샷 |
| `Super+G` | 테마 바꾸기 |
| `Alt+F4`, `Super+W` | 창 닫기 |
| `Alt+Tab` / `Alt+Shift+Tab`, `Super+Tab` | 창 전환 / 작업 보기 |
| `Super+↑` / `Super+↓` / `Super+←` / `Super+→` | 최대화 / 되돌리기·최소화 / 왼쪽·오른쪽 절반 |
| `F11` | 전체 화면 |
| `Alt+Space` | 창 메뉴 (옮기기, 크기 …). `Alt+F7` 옮기기, `Alt+F8` 크기 조절 (방향키, Enter 로 끝) |
| `Super+D`, `Super+M` | 바탕 화면 보기, 모두 최소화 |
| `Super+1` … `Super+9` | n 번째 창 |
| 터미널 안: `Ctrl+Shift+T`, `Ctrl+Shift+W`, `Shift+PgUp/PgDn` | 새 터미널, 닫기, 지난 출력 보기 |

단축키는 모두 설정 파일 하나(`app:/ntui.run/keys`, 내 설정은 `roi:/<uid>/app/ntui/keys`)에 있고, `Super+I` 창이나
편집기로 고친 뒤 `Super+Shift+R` 로 다시 읽습니다. 형식은 [rootfs/app/ntui/keys](rootfs/app/ntui/keys) 머리말.

### 창 프로그램과 터미널

- **터미널 (nterm)**: 셸 `nysh` 가 돌고, `ntop`, `sysmon`, `kilo` 같은 전체 화면 프로그램도 그 안에서 돈다.
  터미널에서 `nfiles` 를 치면 새 창이 뜬다.
- **파일 (nfiles)**, **시계 (nclock)**, **단축키 (nkeys)**: 메뉴나 단축키로 연다.
- 터미널 안의 프로그램은 자기 창을 다룰 수 있다: `nwin size 100x30`, `nwin center`, `nwin full`
  (프로그램에서는 nx64api 의 `nx64_win_*`, [docs/NX64API.md](docs/NX64API.md)).
- 시작할 때 열 창, 테마 같은 설정은 [rootfs/app/README.md](rootfs/app/README.md) 의 표.

### 끝내기

메뉴의 **Log out** 은 로그인 화면으로, **Power off** 는 전원을 끕니다 (한 번 더 묻습니다). 터미널에서는 `poweroff`.

### 화면 없이 (시리얼, `make run-nographic`)

화면을 가져올 수 없으면 ntui 는 지금 터미널 위에서 글자로 그립니다. Super 키가 없으므로 `Ctrl` 단축키를 씁니다:
`Ctrl+Q` 메뉴, `Ctrl+T` 터미널, `Ctrl+E` 파일, `Ctrl+R` 실행, `Ctrl+W` 닫기, `Ctrl+F` 최대화, `Ctrl+N`/`Ctrl+P` 창 전환.
데스크톱 없이 셸만 쓰려면 root 로 `echo app:/nysh.run > roi:/<uid>/shell` (그 사용자의 로그인 셸).

---

## 시스템 둘러보기

경로는 `이름공간:/경로` 입니다.

| 이름공간 | 내용 |
|---|---|
| `face:/` | 내 홈 디렉터리 |
| `app:/` | 설치된 프로그램 (`app:/ls.run` …. 셸에서는 이름만으로) |
| `tmp:/` | RAM 임시 공간 (재부팅하면 사라진다) |
| `disk:/` | 디스크의 영구 공간 |
| `roi:/` | 설정 레지스트리 (파일 하나에 값 하나. 내 것은 `roi:/<uid>/`) |
| `info:/` | 커널이 알려주는 시스템 정보 (읽기 전용) |
| `dev:/`, `svc:/` | 장치, 서비스 채널 |

자주 쓰는 명령:

```
nyfetch                     시스템 정보
ls app:/                    설치된 프로그램
kilo face:/memo.txt         텍스트 편집기
nlcc -o hello.run app:/src/examples/hello.c && ./hello.run     Nyxis 안에서 C 컴파일
elev cat roi:/hostname      root 로 실행 (허용된 사용자, 비밀번호 확인)
ntop                        프로세스
nex                         서비스 목록 (nex status journald, elev nex restart <유닛>)
journalman -b               이번 부팅의 로그 (journalman -f 로 따라가기)
poweroff                    전원 끄기
```

---

## 문서와 README 위치

| 문서 | 내용 |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | 리눅스에서 빌드하고 실행하기 (패키지, 계정, 실행, 저장 공간, 실제 PC, 문제 해결) |
| [docs/NX64API.md](docs/NX64API.md) | nx64api: TUI, 시스템 정보, 설정, 서비스 채널, 저널, ntui 창 다루기, 화면 |
| [docs/NWP.md](docs/NWP.md) | NWP (Nyxis Window Protocol): 컴포지터와 창 프로그램 사이의 규약 |
| [docs/AUDIT.md](docs/AUDIT.md) | 커널 소스 감사와 개선 기록 |
| [docs/LICENSING.md](docs/LICENSING.md) | 외부 코드를 가져올 때의 라이선스 규칙 |
| [nxkernel/kernel/README.md](nxkernel/kernel/README.md) | 커널 구성 요소 (메모리, 프로세스, 시스템 콜, IPC ...) |
| [userland/README.md](userland/README.md) | 사용자 프로그램 목록과 빌드 규칙 (새 프로그램 넣는 법) |
| [userland/coreutils/README.md](userland/coreutils/README.md) | 기본 명령어 목록과 옵션 |
| [userland/nlibc/README.md](userland/nlibc/README.md) | C 라이브러리 nlibc (지원 범위, 이름공간 경로 규칙) |
| [userland/nlibc/INSTALL.md](userland/nlibc/INSTALL.md) | 호스트의 nlcc 로 Nyxis 프로그램 만들기 (SDK) |
| [userland/nlcc/README.md](userland/nlcc/README.md) | Nyxis 안의 C 컴파일러 nlcc (TinyCC) |
| [userland/kilo/README.md](userland/kilo/README.md) | 텍스트 편집기 kilo |
| [rootfs/app/README.md](rootfs/app/README.md) | 앱 기본 설정 (`app:/<앱>.run/<키>`) 목록: init 유닛, journald, ntui, nysh ... |
| [tools/nyfs-utils/README.md](tools/nyfs-utils/README.md) | 호스트에서 nyfs 볼륨 만들기/마운트(FUSE) |
| [userland/init/service.c](userland/init/service.c) 머리말 | 서비스 유닛 파일 형식 (`nex help-unit` 으로도) |
| [rootfs/app/ntui/keys](rootfs/app/ntui/keys) 머리말 | 데스크톱 단축키 설정 형식 |

### 저장소 구조

```
bootloader/     UEFI 부트로더 (nytb)
nxkernel/       커널과 빌드 시스템 (Makefile)
drivers/        AHCI, ATA, 램디스크, PIC, 파일시스템(nyfs, VFS)
include/        커널/유저랜드 공용 헤더 (nyx_abi.h = 시스템 콜 ABI)
userland/       사용자 프로그램, nlibc, 데스크톱
rootfs/         이미지에 들어가는 설정 (roi:/, 앱 기본 설정)
tools/          이미지 도구 (mkrootfs.py, nyfs-utils), 종단 간 검사
tests/          호스트 검사
third_party/    TinyCC, kilo 원본 (출처는 각 NYXIS-UPSTREAM.txt)
config/         로그인 계정 DB (make users, git 에 올리지 않음)
docs/           문서
```

---

## 라이선스

GPL-3.0 ([LICENSE](LICENSE)). `third_party/` 의 코드는 각자의 라이선스를 따릅니다 (TinyCC: LGPL-2.1 이상, kilo: BSD-2-Clause).
외부 코드를 가져오는 규칙은 [docs/LICENSING.md](docs/LICENSING.md).
