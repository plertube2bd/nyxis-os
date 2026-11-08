# nlcc — Nyxis 안에서 쓰는 C 컴파일러

Nyxis 안의 `nlcc`(`app:/nlcc.run`)는 [TinyCC](../../third_party/tinycc/NYXIS-UPSTREAM.txt)를 Nyxis용으로 빌드한
것입니다. 컴파일러, 어셈블러, 링커가 한 프로그램에 들어 있어서 Nyxis 안에서 바로 실행 파일을 만듭니다.
호스트의 `nlcc`(`out/nlibc/bin/nlcc`, gcc/ld 래퍼 — [nlibc INSTALL.md](../nlibc/INSTALL.md))와 이름과 역할이 같고,
둘 다 nlibc와 링크한 정적 실행 파일을 만듭니다.

```
~* nlcc -o hello.run app:/src/examples/hello.c
~* ./hello.run
Hello from nlibc!
```

## 사용법

tcc 옵션을 그대로 씁니다 (`nlcc -h`).

| 예 | 뜻 |
|---|---|
| `nlcc -o prog.run a.c b.c` | 컴파일 + 링크 |
| `nlcc -c a.c` | 오브젝트만 (`a.o`) |
| `nlcc -o prog.run a.o b.o -lm` | 링크만 (`-lm` 은 받아들이지만 수학 함수는 libc 안에 있다) |
| `nlcc -E a.c` | 전처리만 |
| `nlcc -ar rcs libx.a a.o b.o` | 정적 라이브러리 만들기 |
| `nlcc -I dir -D NAME=1 -Wall -g` | 흔한 옵션 |

- 실행 파일은 항상 정적 링크입니다 (Nyxis에는 동적 링커가 없다). `-run`(메모리에서 바로 실행)은 없습니다.
- 표준은 tcc 기본값(C99에 GNU 확장 일부)입니다. 미리 정의되는 매크로는 `__nyxis__`, `__x86_64__`,
  `__TINYC__`이고 `__linux__`는 없습니다.
- 최적화(`-O`)는 받아들이지만 tcc는 최적화하지 않습니다. 빠른 코드가 필요하면 호스트의 nlcc(gcc)를 쓰세요.
- 출력은 쓸 수 있는 곳에: 홈(`face:/`, 처음 위치)이나 `tmp:/`.

## 설치 위치 (이미지의 app:/)

| 경로 | 내용 |
|---|---|
| `app:/nlcc.run` | 컴파일러 |
| `app:/lib/tcc/include/` | tcc 자신의 헤더 (`stdarg.h` `stddef.h` `float.h` `stdbool.h` `tccdefs.h` …). nlibc 헤더보다 먼저 찾는다 |
| `app:/lib/tcc/libtcc1.a` | tcc 런타임 (x86_64 `va_arg`, long double 변환, `alloca` …) |
| `app:/lib/nlibc/include/` | nlibc 헤더 (호스트 SDK와 같다) |
| `app:/lib/nlibc/lib/` | `crt1.o`(= `nlcrt0.o`), `crti.o`/`crtn.o`(빈 오브젝트), `libc.a`(= `libnlibc.a`) |
| `app:/src/tcc/` | nlcc 자신의 소스 (패치 적용본 + `config.h`): 부트스트랩용 |
| `app:/src/examples/` | 예제 (`hello.c`) |

tcc는 리눅스처럼 `crt1.o crti.o … -lc … crtn.o` 를 붙이므로, nlibc를 그 이름으로 두어 tcc를 고치지 않았습니다.
gcc로 빌드한 nlibc와 tcc가 만든 코드는 같은 ABI(System V x86_64, `va_list` 포함)를 써서 그대로 섞입니다.

## 부트스트랩 (Nyxis 안에서 자기 자신을 컴파일)

```
~* nlcc -DONE_SOURCE=1 -o tcc1.run app:/src/tcc/tcc.c         # stage1: stage0(gcc 로 빌드)이 만든 것
~* ./tcc1.run -DONE_SOURCE=1 -o tcc2.run app:/src/tcc/tcc.c   # stage2: stage1 이 만든 것
~* cmp tcc1.run tcc2.run && echo same
same
```

stage1과 stage2가 바이트 단위로 같으면 Nyxis 안의 컴파일러가 자기 자신을 올바르게 재생산한다는 뜻입니다.
`make test-e2e`가 매번 이것을 확인합니다 (QEMU에서 각 단계 1초 남짓).

## 빌드 구조 (`make -C nxkernel nlcc`, `make os`에 포함)

| 파일 | 역할 |
|---|---|
| `third_party/tinycc/` | 수정하지 않은 upstream (커밋은 `NYXIS-UPSTREAM.txt`), LGPL |
| `userland/nlcc/tinycc-nyxis.patch` | Nyxis용 수정 (아래) |
| `userland/nlcc/config.h` | tcc의 `configure`가 만드는 파일 대신: 대상, 경로, 꺼 둔 기능 |

1. `out/nlcc/src` — upstream 사본에 패치 적용 + `config.h`
2. `out/usr/nlcc.run` — stage0: 호스트 nlcc(gcc)로 위 소스를 빌드 (`-DONE_SOURCE=1`)
3. `out/nlcc/tcc-host` — 같은 소스를 호스트 gcc로 빌드한 tcc. sysroot(`out/nlcc/root`)를 가리킨다.
   `libtcc1.a`와 빈 `crti.o`/`crtn.o`를 만든다 (libtcc1은 tcc의 호출 규약으로 컴파일해야 한다)
4. `out/nlcc/root/{lib,src}` — 이미지의 `app:/lib`, `app:/src` (`mkrootfs.py --tree`)

호스트 tcc로 Nyxis용 프로그램을 만들 수도 있습니다: `out/nlcc/tcc-host -o x.run x.c`.

### 패치 내용 (`TARGETOS_Nyxis`)

- 경로: Nyxis 경로는 `이름공간:/…`라서 경로 목록 구분자를 `:` 대신 `;`, `app:/x`도 절대 경로로 본다
- 실행 파일은 항상 정적 링크
- `-run`(mmap/mprotect 필요)을 끈다
- 미리 정의하는 매크로 `__nyxis__` (`__linux__` 대신), `nlcc -v`의 OS 이름

### Nyxis 쪽에서 함께 바꾼 것

- 커널 ELF 로더: 세그먼트가 페이지 중간에서 시작해도 된다 (`p_vaddr ≡ p_offset (mod 4KiB)`, ELF 표준 규칙).
  세그먼트끼리 같은 페이지를 쓰면 거부한다 (W^X 유지)
- 커널 VFS: 경로 캐시(dentry)가 차면 쓰지 않는 항목을 거둔다. 예전에는 서로 다른 파일을 64개쯤 연 뒤로
  시스템 전체에서 파일을 열 수 없었다 (tcc가 헤더를 많이 열면서 드러남)
- tmp:/ 2MiB(inode 64개) → 16MiB(inode 1024개), 루트 파일시스템 8MiB → 32MiB
- nlibc: `sys/time.h`, `inttypes.h`, `stdint.h` 확장, C99 함수(`strtoll` `strtoull` `strtof` `strtold` `ldexpl`
  `va_copy` …), `realpath`
- coreutils: `cmp`

## 한계

- 최적화가 없어 gcc 결과보다 느린 코드
- `strtold`는 double 정밀도로 읽는다 (nlibc): 아주 긴 long double 상수의 마지막 자리
- 디버거가 없어 `-g`는 정보만 넣는다
