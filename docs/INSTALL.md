# 리눅스에서 Nyxis OS 빌드하고 실행하기

Nyxis OS 를 리눅스(x86_64)에서 소스로 빌드해 QEMU 가상 머신으로 실행하는 방법입니다.
빌드에 필요한 외부 소스(TinyCC, kilo)는 저장소의 `third_party/` 에 들어 있어서 따로 받을 것은 없습니다.

> Nyxis OS 는 실험용 취미 운영체제입니다. 실제 PC 보다 QEMU 에서 실행하는 것을 권장합니다.

## 1. 필요한 패키지

| 용도 | 도구 |
|---|---|
| 컴파일 | `gcc`, `binutils`(`as`, `ld`, `objcopy`), `make`, `patch` |
| UEFI 부트로더 | `gnu-efi` (3.x, 4.x 모두 동작) |
| 디스크 이미지 | `dosfstools`(`mkfs.vfat`), `mtools`(`mmd`, `mcopy`) |
| 빌드 스크립트 | `python3` |
| 실행 | `qemu-system-x86_64`, OVMF (QEMU 용 UEFI 펌웨어) |

### Arch Linux

```bash
sudo pacman -S --needed base-devel gnu-efi dosfstools mtools python qemu-system-x86 edk2-ovmf
```

### Ubuntu / Debian

```bash
sudo apt install build-essential gnu-efi dosfstools mtools python3 qemu-system-x86 ovmf
```

### 다른 배포판

같은 도구를 설치하면 됩니다. 다만 Makefile 은 아래 경로를 기본값으로 씁니다.
배포판의 파일 위치가 다르면 `make` 에 변수로 넘기세요 (예: `make OVMF=/경로/OVMF_CODE.fd run`).

| 변수 | 기본값 | 내용 |
|---|---|---|
| `OVMF` | `/usr/share/OVMF/OVMF_CODE_4M.fd` | UEFI 펌웨어 코드 |
| `OVMF_VARS` | `/usr/share/OVMF/OVMF_VARS_4M.fd` | UEFI 변수 원본 (실행할 때마다 복사본을 쓴다) |
| `EFIINC` | `/usr/include/efi` | gnu-efi 헤더 |
| `EFILIB` | `/usr/lib` | `elf_x86_64_efi.lds`, `libefi.a`, `libgnuefi.a` 위치 |
| `CRT0` | `/usr/lib/crt0-efi-x86_64.o` | gnu-efi 시작 코드 |

## 2. 소스 받기

```bash
git clone https://github.com/plertube2bd/nyxis-os.git
cd nyxis-os
```

이후 명령은 모두 `nxkernel/` 에서 실행합니다 (`make -C nxkernel ...` 로 해도 같습니다).

```bash
cd nxkernel
```

## 3. 로그인 계정 만들기

Nyxis 는 부팅하면 로그인 화면이 나옵니다. 이미지를 빌드하기 전에 계정을 한 번 만들어 둡니다.

```bash
make users
```

이름, uid, 비밀번호를 차례로 묻습니다. 관리자로 쓰려면 `root`(uid 0)를 만들고, 평소에 쓸 일반 계정도 하나
만들어 두세요 (uid 1000 부터). 결과는 `config/users.db` 에 저장되며, 비밀번호는 솔트를 친 해시로만
저장되고 git 에는 올라가지 않습니다. 계정을 바꾸려면 `make users` 를 다시 실행한 뒤 이미지를 다시 빌드합니다.

## 4. 빌드하고 실행하기

```bash
make run
```

처음 실행하면 부트로더, 커널, C 라이브러리(nlibc), 기본 명령어, 셸, Nyxis 안에서 쓰는 C 컴파일러(nlcc)까지
모두 빌드합니다. 몇 분 걸릴 수 있습니다. 끝나면 QEMU 창이 뜨고, 시리얼 출력(커널 로그)은 실행한
터미널에 나옵니다. 로그인한 뒤에는 화면 창과 터미널 어느 쪽에서도 입력할 수 있습니다.

화면이 없는 환경(SSH 등)에서는 시리얼만으로 실행합니다. QEMU 를 끝내려면 `Ctrl-A` 다음 `X` 를 누릅니다.

```bash
make run-nographic
```

자주 쓰는 다른 타깃:

| 명령 | 내용 |
|---|---|
| `make os` | 실행하지 않고 부팅 이미지만 만든다 (`out/esp.img`) |
| `make clean` | 빌드 결과(`out/`)를 지운다. `disk.img` 와 `config/` 는 남는다 |
| `make check` | C89 엄격 컴파일 검사 |

## 5. 처음 써 보기

로그인하면 셸 `nysh` 가 뜹니다. 프롬프트의 `~` 는 홈 디렉터리(`face:/`)입니다.

```
~* nyfetch                시스템 정보
~* ls app:/               설치된 프로그램
~* echo hi > tmp:/a.txt   tmp:/ 는 재부팅하면 사라진다
~* echo hi > disk:/a.txt  disk:/ 는 재부팅해도 남는다
~* kilo face:/memo.txt    텍스트 편집기
~* poweroff               전원 끄기
```

실행 중인 명령은 `Ctrl-C` 로 멈출 수 있습니다 (`&` 로 띄운 백그라운드 작업은 그대로 둡니다).
프롬프트에서 `Ctrl-C` 를 누르면 쓰던 줄을 버리고, `Ctrl-D` 는 로그아웃입니다.

경로는 `이름공간:/경로` 형식입니다.

| 이름공간 | 내용 |
|---|---|
| `face:/` | 내 홈 디렉터리 |
| `app:/` | 설치된 프로그램 (`ls`, `cat`, `nysh` …) |
| `tmp:/` | RAM 에 있는 임시 공간. 재부팅하면 사라진다 |
| `disk:/` | 디스크에 있는 영구 공간. 재부팅해도 남는다 |
| `info:/` | 커널이 알려주는 시스템 정보 (읽기 전용) |

명령어 목록은 [userland/coreutils/README.md](../userland/coreutils/README.md) 에 있습니다.

### 저장 공간 (`disk:/`)

`make run` 은 저장소 루트에 `disk.img`(64 MiB)가 없으면 새로 만들어 두 번째 디스크로 붙입니다.
`face:/` 를 비롯한 루트 파일시스템은 부팅할 때마다 이미지에서 새로 읽어 오므로, 남겨야 하는 파일은
`disk:/` 에 저장하세요.

```bash
make disk-reset                 # disk.img 를 지우고 빈 디스크로 다시 만든다 (내용이 모두 사라진다)
make DISK_SIZE=256M disk-reset  # 크기를 바꿔서 다시 만든다
```

## 6. 실제 PC 에서 부팅하기 (실험적)

`out/esp.img` 는 UEFI 부팅 파일(`EFI/BOOT/BOOTX64.EFI`)이 들어 있는 FAT 이미지입니다. USB 메모리에
기록하면 UEFI PC 에서 부팅을 시도할 수 있습니다. 이 방법은 실제 하드웨어에서 검증되지 않았습니다.

```bash
lsblk                                         # USB 장치 이름 확인 (예: /dev/sdX)
sudo dd if=../out/esp.img of=/dev/sdX bs=4M conv=fsync status=progress
```

- `of=` 를 잘못 지정하면 그 디스크의 내용이 지워집니다. `lsblk` 로 꼭 확인하세요.
- 펌웨어 설정에서 Secure Boot 를 꺼야 합니다 (부트로더가 서명되어 있지 않습니다).
- 저장 장치는 AHCI(SATA) 모드여야 합니다. NVMe 디스크는 아직 지원하지 않습니다.
- 실제 PC 에서는 `disk:/` 를 쓸 수 없는 것이 보통입니다. 커널은 다른 운영체제의 디스크를 지우지 않도록
  디스크를 스스로 포맷하지 않고, nyfs 로 포맷된 SATA 디스크만 `disk:/` 로 붙입니다.

## 7. 문제 해결

| 증상 | 해결 |
|---|---|
| `error: no user database` | 3단계의 `make users` 를 먼저 실행한다 |
| `cp: cannot stat '/usr/share/OVMF/...'` | OVMF 를 설치하거나 `make OVMF=... OVMF_VARS=... run` 으로 경로를 준다 |
| `efi.h: No such file or directory` 또는 `crt0-efi-x86_64.o` 없음 | `gnu-efi` 를 설치하거나 `EFIINC`/`EFILIB`/`CRT0` 를 준다 |
| `mkfs.vfat` / `mmd` 를 찾을 수 없음 | `dosfstools`, `mtools` 를 설치한다 |
| QEMU 창이 뜨지 않음 (`gtk initialization failed`) | 그래픽 세션 밖이면 `make run-nographic` 을 쓴다 |
| `disk:/` 가 없다고 나옴 (`disk:/ unavailable`) | `make disk-reset` 으로 디스크 이미지를 다시 만든다 |

## 개발자용: 검사 실행

```bash
make test-coreutils                # 명령어와 셸을 리눅스에서 검사
make test-nlibc                    # C 라이브러리 검사
make clean && make SELFTEST=1 os   # 테스트용 이미지 (무작위 비밀번호의 root/test 계정)
make test-e2e                      # QEMU 로 부팅 -> 로그인 -> 명령 실행 -> 전원 끄기
make test-disk                     # disk:/ 가 재부팅 뒤에도 남는지
```

`SELFTEST=1` 이미지와 일반 이미지를 오갈 때는 꼭 `make clean` 을 먼저 하세요. Makefile 이 컴파일 옵션의
변화를 추적하지 않아서, 섞어 빌드하면 자체 점검이 빠진 이미지가 만들어집니다.
