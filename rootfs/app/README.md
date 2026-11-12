# 앱 기본 설정 (`app:/<이름>.run/<키>`)

nyfs 의 노드는 파일이든 디렉터리든 하나의 오브젝트다 (내용 = 데이터 스트림, 자식 = 목록). 그래서 여기에
`<이름>/` 디렉터리가 있는 앱은 이미지에서 **ELF 를 내용으로, 이 디렉터리의 파일들을 자식으로** 가진 오브젝트가 된다
(`tools/mkrootfs.py --app`). 실행은 그대로 `app:/<이름>.run`, 기본 설정은 `app:/<이름>.run/<키>` 다.

- 값 하나 = 파일 하나. 한 줄 값은 끝의 개행 하나를 뗀다.
- 기본값은 root 소유 0644 (누구나 읽기만). 사용자 값은 `roi:/<uid>/app/<이름>/<키>` 에 두며 기본값보다 먼저 읽는다.
- 읽는 함수: nlibc `nyx_setting_get()` (`<nyxapp.h>`), init/login 같은 작은 프로그램은 `nx_setting_get()` (`userland/lib/nyx.h`).

| 앱 | 키 | 기본값 | 뜻 |
|---|---|---|---|
| init | `welcome` | `Welcome to Nyxis OS!` | 부팅 후 로그인 전에 보이는 문구 |
| login | `prompt` | `nyxis os login: ` | 로그인 프롬프트 |
| nysh | `mark` | `*` | 프롬프트 기호 (출력 가능한 ASCII 1~8 자) |
| nysh | `rc` | (주석뿐) | 대화형 셸 시작 스크립트. 기본 rc 다음에 사용자 rc 도 실행 |
| ntui | `theme` | `nyx` | 테마 (Super+G 로 바꾸면 사용자 값으로 저장) |
| ntui | `startup` | `help` | 시작할 때 열 창: `help` / `terminal` / `files` / `none` |
| ntui | `display` | (없음) | `terminal` 이면 화면(dev:/console)을 가져오지 않고 지금 터미널 위에서 돈다 |
| sysmon | `refresh_ms` | `1000` | 다시 그리는 간격 (100~10000) |
| ntop | `refresh_ms` | `1000` | 다시 읽는 간격 (200~10000) |
| ntop | `sort` | `pid` | 처음 정렬: `pid` / `cpu` / `mem` |
| nyfetch | `fields` | `os abi uptime shell terminal cpu memory allocator palette` | 보일 항목과 순서 |
