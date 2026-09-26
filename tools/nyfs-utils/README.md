# nyfs-utils

NyxisOS 의 자체 파일시스템 **NyFS** 를 리눅스 호스트에서 다루기 위한 도구
모음. 커널(`drivers/filesystem/nyfs/nyfs.c`) 없이도 리눅스에서 NyFS 볼륨을
만들고, FUSE 로 마운트해서 평범한 파일처럼 쓰고 읽을 수 있다. QEMU 로
커널을 부팅하지 않고도 NyxisOS 이미지를 준비/검사/수정할 때 쓴다.

## 구성

- `mkfs.nyfs` - 이미지 파일 또는 블록 장치에 NyFS 볼륨을 만든다.
- `nyfs-fuse` - FUSE3 로 NyFS 볼륨을 마운트한다(유저스페이스, 커널 모듈
  설치/서명 불필요).
- `libnyfscore`(`nyfs_core.c`) - 위 둘이 공유하는 온디스크 포맷 로직.
  커널 `nyfs.c` 를 그대로 이식한 것 - 비트맵 할당자, inode 테이블, extent
  체인, 디렉터리 해시, UNIX+NTFS ACE 권한 검사를 바이트 단위로 동일하게
  구현했다. **이 두 도구로 만들거나 손댄 볼륨은 커널이 그대로 마운트할 수
  있고, 커널이 만든 볼륨도 이 도구들로 그대로 읽고 쓸 수 있다.**

## 범위(커널과 의도적으로 동일하게 제한함)

2026-09-26 기준 커널 `nyfs.c` 가 아직 구현하지 않은 기능은 이 도구들도
구현하지 않는다 - 그래야 온디스크 포맷이 갈라지지 않는다:

- extent depth 는 0(직접 리스트)만 지원 - 매우 큰 파일에서 간접 블록으로
  들어가는 경우는 다루지 않는다
- 저널 없음
- "." / ".." 디렉터리 엔트리는 온디스크에 존재하지 않는다(FUSE 쪽에서만
  화면 표시용으로 얹는다 - 커널 `nyfs_readdir` 과 동일)

rename 과 ACL 상속은 커널이 구현한 뒤(2026-09-26) 이 도구들에도 그대로
포팅했다:

- **rename**: 같은 디렉터리 안 이름 변경과 다른 디렉터리로의 이동 모두
  지원(같은 볼륨 안에서만 - 이 도구는 볼륨을 하나만 다루므로 자동으로
  성립). 덮어쓰기 rename 은 미지원(대상 이름이 이미 있으면 `EEXIST`).
  저널이 없어 완전한 롤백은 못 하고 최선을 다해 복구를 시도한다.
  디렉터리를 자기 자신 바로 위로 옮기는 가장 자명한 순환만 막는다 - 더
  깊은 순환(자기 하위 디렉터리 밑으로 옮기기)은 inode 에 부모 포인터가
  없어 이 계층만으로는 못 잡는다(커널과 동일한 알려진 한계 - FUSE/커널
  둘 다 상위의 dentry 캐시가 한 번 더 잡아주는 것에 의존).
- **ACL 상속**: `create`/`mkdir` 시 부모 디렉터리의 `INHERIT_FILE`/
  `INHERIT_DIR` ACE 를 자식에게 복사한다(NTFS 관례대로 `INHERIT_ONLY`/
  `NO_PROPAGATE` 처리). 상속받은 ACE 가 하나라도 있으면 그 순간부터
  자식의 권한 판정은 UNIX 모드가 아니라 ACL 로만 이뤄진다. 단, 이
  저장소에는 아직 ACL 을 "설정"하는 진입점(파일 API 로 ACE 를 추가하는
  syscall/함수)이 없어 - 커널 쪽도 마찬가지다 - 상속 자체는 코드 회귀
  테스트(`make test`)로만 검증했고, 실제로 ACE 를 붙인 파일을 대상으로
  한 실사용 시나리오는 아직 없다.

chmod/chown/utimens(`nyfs_op_setattr`) 은 예외다 - 커널에 아직 그 시스템
콜이 없지만, 다루는 필드(unix_mode/uid/gid/mtime/atime)는 이미 온디스크
inode 구조체에 있고 커널도 그대로 읽는 필드라 포맷을 바꾸지 않는다.

## 빌드

```sh
sudo apt install build-essential libfuse3-dev pkg-config
make            # build/mkfs.nyfs, build/nyfs-fuse
make test       # FUSE 마운트 없이 코어 라이브러리만 회귀 테스트
```

빌드 규칙(저장소 최상위 지침과 동일한 엄격도):

- `nyfs_core.c`, `mkfs_nyfs.c`: `-std=c89 -pedantic -Wall -Wextra -Werror
  -Wstrict-prototypes -Wmissing-prototypes -Wshadow
  -Wdeclaration-after-statement`
- `nyfs_fuse.c` 만 예외: libfuse3 헤더 자체가 C99 API 라 `-std=gnu99` 로
  빌드한다(`nxkernel/Makefile` 의 `NYX_HOST_TEST` 호스트 테스트 빌드와
  같은 선례를 따름). 실제 온디스크 로직은 전부 C89 코어 쪽에 있고, 이
  파일은 "FUSE 콜백 <-> libnyfs 호출"만 잇는 얇은 글루 코드다.

## 사용법

```sh
# 16MiB 이미지 파일을 새로 만들어 포맷
./build/mkfs.nyfs -s 16M -L mylabel disk.img

# 실제 블록 장치(예: USB, 루프 장치)에 포맷 - 장치 크기를 그대로 쓴다
sudo ./build/mkfs.nyfs -f /dev/sdX

# 마운트(포그라운드, Ctrl-C 로 종료 또는 다른 터미널에서 언마운트)
mkdir -p /mnt/nyfs
./build/nyfs-fuse disk.img /mnt/nyfs -f

# 다른 터미널에서 평범한 파일처럼 사용
echo hello > /mnt/nyfs/a.txt
mkdir /mnt/nyfs/sub
cat /mnt/nyfs/a.txt

# 언마운트
fusermount3 -u /mnt/nyfs
```

읽기 전용으로 마운트하려면 `-o ro` 를 붙인다(`fuse_main` 표준 옵션).

## 알려진 한계

- truncate 로 파일을 "늘리는" 것은 지원하지 않는다(구멍을 만들려면 extent
  할당이 필요한데, 읽었을 때 항상 0 이 나오는 것을 보장하는 안전한 구현이
  아직 없어 명시적으로 거부한다 - 조용히 틀린 내용을 주는 것보다 안전).
  줄이는 truncate 는 지원한다.
- rename 은 `RENAME_EXCHANGE`/`RENAME_NOREPLACE` 같은 리눅스 전용 플래그를
  지원하지 않는다(플래그가 오면 `ENOTSUP`) - 순수 이동/이름변경만 된다.
- ACL 을 실제로 설정하는 API 가 아직 없다(커널도 동일) - 위 "ACL 상속"
  항목 참고.
- `nyfs-fuse` 는 프로세스 전체에서 볼륨 하나만 다룬다(멀티 마운트 불가) -
  개발용 도구로는 충분하지만 프로덕션 용도가 아니다.

## 라이선스

nyxis-os 저장소 전체와 동일하게 GPL-3.0-or-later. `nyfs_disk.h`,
`nyfs_core.c` 는 `drivers/filesystem/nyfs/nyfs.c`/`nyfs_disk.h` 의 온디스크
포맷 로직을 그대로 이식한 것이다.
