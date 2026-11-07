#!/usr/bin/env python3
"""
mkrootfs.py - NyxisOS 루트 파일시스템(fs:/) 매니페스트를 만든다 (tools/nyfs-utils 의 nyfs-pack 입력)

  python3 tools/mkrootfs.py --users <db> --bin <dir> [--file <이미지경로>=<호스트파일>[:<모드>]]... > manifest

만드는 구조:
  /            0755 root      fs:/
  /usr         0755 root      app:/   <- --bin 디렉터리의 *.run (0755)
  /roi         0700 root      roi:/
  /roi/users   0600 root      사용자 DB (커널 전용: 실행 중에는 아무도 열 수 없다)
  /home        0755 root
  /home/<이름> 0700 <uid>     face:/  (DB 의 각 사용자)
--file 로 추가 파일을 넣는다 (예: 테스트 이미지의 /roi/test_password). 모드 기본값 0644, 소유자 root.
"""
import os
import sys


def main():
    users_db, bin_dir, extra = None, None, []
    args = sys.argv[1:]
    while args:
        a = args.pop(0)
        if a == "--users":
            users_db = args.pop(0)
        elif a == "--bin":
            bin_dir = args.pop(0)
        elif a == "--file":
            extra.append(args.pop(0))
        else:
            sys.exit(__doc__)
    if not users_db or not bin_dir:
        sys.exit(__doc__)

    out = ["d / 0755 0 0", "d /usr 0755 0 0"]
    for name in sorted(os.listdir(bin_dir)):
        if name.endswith(".run"):
            out.append(f"f /usr/{name} 0755 0 0 {os.path.abspath(os.path.join(bin_dir, name))}")

    out += ["d /roi 0700 0 0", f"f /roi/users 0600 0 0 {os.path.abspath(users_db)}", "d /home 0755 0 0"]
    with open(users_db) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, uid, gid = line.split(":")[:3]
            out.append(f"d /home/{name} 0700 {int(uid)} {int(gid)}")

    for spec in extra:
        dest, src = spec.split("=", 1)
        mode = "0644"
        if ":" in src and src.rsplit(":", 1)[1].isdigit():
            src, mode = src.rsplit(":", 1)
        out.append(f"f {dest} {mode} 0 0 {os.path.abspath(src)}")

    print("\n".join(out))


if __name__ == "__main__":
    main()
