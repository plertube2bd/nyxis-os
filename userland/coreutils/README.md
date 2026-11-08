# coreutils — Nyxis 기본 명령어

nlibc로 만든 작은 명령어 모음입니다. 각 명령은 `fs:/usr/<이름>.run`(= `app:/<이름>.run`)으로 이미지에 들어가고,
nysh에서 이름만으로 실행됩니다 (`ls`, `cat a.txt` …). 경로는 nlibc 규칙을 따릅니다: `tmp:/x` 같은 이름공간 경로는
절대 경로, 그 밖에는 현재 디렉터리(nysh 의 `cd`, 처음에는 홈 `face:/`) 기준입니다.

| 명령 | 사용법 | 메모 |
|---|---|---|
| `cat` | `cat [-n] [파일...]` | 파일이 없거나 `-` 이면 표준 입력. `-n` 줄 번호 |
| `echo` | `echo [-neE] [문자열...]` | 백슬래시 이스케이프를 기본으로 해석 (XSI/dash 방식: `\n \t \\ \a \b \c \e \f \r \v \0nnn \xHH`). `-n` 끝 줄바꿈 없음, `-E` 해석 안 함, `-e` 해석(기본). n/e/E 가 아닌 `-x` 는 글자 그대로 |
| `clear` | `clear` | 화면 지우기 (`ESC[H ESC[2J` 출력. 콘솔이 지원하는 시퀀스는 `nxkernel/console/outputs/ansi.h`) |
| `ls` | `ls [-lad1] [경로...]` | 이름순. 디렉터리는 끝에 `/`. 터미널이면 색(디렉터리 굵은 파랑, `*.run` 초록; `NO_COLOR`/`TERM=dumb` 이면 끔). `-l` 권한·uid·gid·크기, `-a` 점 파일, `-d` 디렉터리 자체 |
| `cp` | `cp [-r] 원본... 대상` | 대상이 디렉터리면 그 안으로. 모드는 원본 그대로. `-r` 디렉터리 통째로 |
| `mv` | `mv 원본... 대상` | 파일 대상은 덮어쓴다. 이름 바꾸기가 안 되는 곳(이름공간 사이)의 파일은 복사 후 원본 삭제 |
| `rm` | `rm [-rf] 경로...` | `-r` 디렉터리 내용까지, `-f` 없는 파일 무시. 이름공간 뿌리·`.`·`..` 는 거부 |
| `touch` | `touch [-c] 파일...` | 없는 파일을 빈 파일로 만든다. 파일에 바꿀 수 있는 시각이 없어 있는 파일은 그대로 |
| `mkdir` | `mkdir [-p] [-m 모드] 디렉터리...` | `-p` 중간 디렉터리도, `-m` 8진수 모드 |
| `rmdir` | `rmdir 디렉터리...` | 빈 디렉터리만 |
| `chmod` | `chmod 모드 파일...` | 8진수(`644`) 또는 기호(`u+x,go-w`, `a=r`, `+x`). 소유자나 root 만 |
| `head` | `head [-n 줄수] [파일...]` | 기본 10줄 |
| `tail` | `tail [-n 줄수] [파일...]` | 기본 10줄 (입력을 모두 메모리에 읽는다) |
| `wc` | `wc [-lwc] [파일...]` | 줄·단어·바이트, 파일이 여럿이면 합계 |
| `pwd` | `pwd` | 현재 디렉터리 |
| `nyfetch` | `nyfetch` | 시스템 정보 (사용자@호스트, OS·ABI·업타임·셸·터미널·CPU·메모리). 값은 `info:/` 와 CPUID 에서. 터미널이면 색 |
| `sleep` | `sleep 초` | 소수 가능 (`0.5`) |
| `true` / `false` | | 종료 코드 0 / 1 |

종료 코드: 성공 0, 처리 중 오류 1, 사용법 오류 2. 오류 메시지는 `명령: 경로: 이유` 형식으로 stderr 에 씁니다.

## 구조와 빌드
- 명령마다 `<이름>.c` 하나 + 공용 `common.c`(옵션 해석 `cu_getopt`, 오류 출력, 경로 잇기, 파일 복사).
- `make -C nxkernel os` 가 SDK의 `nlcc` 로 빌드해 이미지에 넣습니다 (`COREUTILS` 변수에 목록).
  새 명령은 `<이름>.c` 를 만들고 `COREUTILS` 에 이름을 더하면 됩니다.

## 검사
- `make -C nxkernel test-coreutils` — 같은 소스를 호스트용 nlibc(플랫폼 계층만 Linux)로 링크해
  `tests/host/coreutils/run.sh` 로 동작을 확인합니다 (133개).
- `make test-e2e` — Nyxis 안에서 `mkdir -p`, `cp`, `cat`, `echo`, `touch`, `mv`, `ls`, `wc`, `rm -r` 을 실행합니다.
- `make -C nxkernel test-coreutils` 의 두 번째 단계는 nysh 를 같은 방식으로 빌드해 `tests/host/nysh/run.sh` 로
  셸 문법(따옴표, 변수, 파이프, 리디렉션, cd, 글롭, source)을 확인합니다 (69개).
