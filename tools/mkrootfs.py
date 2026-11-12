#!/usr/bin/env python3
"""
mkrootfs.py - NyxisOS 루트 파일시스템(fs:/) 매니페스트를 만든다 (tools/nyfs-utils 의 nyfs-pack 입력)

  python3 tools/mkrootfs.py --users <db> --bin <dir> --roi <dir> [--app <dir>] [--file <이미지경로>=<호스트파일>[:<모드>]]... > manifest

만드는 구조:
  /                0755 root      fs:/
  /usr             0755 root      app:/   <- --bin 디렉터리의 *.run (0755)
  /usr/<이름>.run/ 0755 root      --app <dir>/<이름>/ 이 있는 앱: 디렉터리 오브젝트 = ELF 를 내용으로, 그 아래에
                                  기본 설정 (app:/<이름>.run/<키>, 디렉터리 0755, 파일 0644, root 소유: 누구나 읽기만).
                                  nyfs 의 노드는 파일/디렉터리 구분 없는 오브젝트라 디렉터리도 내용을 가진다.
                                  사용자 값은 roi:/<uid>/app/<이름>/<키> 에 두고 기본값보다 먼저 읽는다 (nyxapp.h).
  /roi             0711 root      roi:/   (들어가기만 허용, 목록은 root 만)
  /roi/users       0600 root      사용자 DB (커널 전용: 실행 중에는 아무도 열 수 없다)
  /roi/hostname    0644 root      <roi>/hostname   (info:/hostname 으로 공개)
  /roi/timezone    0644 root      <roi>/timezone   (info:/timezone 으로 공개)
  /roi/app/        <roi>/app/ 통째로, root 전용 (디렉터리 0700, 파일 0600). 시스템 프로그램 설정
                   (예: /roi/app/0/init/sudoers.dat = sudo 를 쓸 수 있는 uid)
  /roi/default/    <roi>/default/ 틀. "새 사용자 소유" 항목은 소유자 NX_UID_TEMPLATE
  /roi/<uid>/      DB 의 각 사용자(root 포함): 틀을 복사하고 NX_UID_TEMPLATE 을 그 uid/gid 로 바꾼다
  /home            0755 root
  /home/<이름>     0700 <uid>     face:/  (DB 의 각 사용자)
--template-file <틀 안의 경로>=<호스트 파일> 로 틀(<roi>/default/)의 파일 내용을 바꿔 넣는다 (모든 사용자에게 복사된다).
예: SELFTEST 이미지는 테스트 하네스가 셸 명령을 치므로 shell 을 app:/nysh.run 으로 바꾼다.
--file 로 추가 파일을 넣는다 (예: 테스트 이미지의 /roi/test_password). 모드 기본값 0644, 소유자 root.
--tree <이미지경로>=<호스트 디렉터리> 로 디렉터리를 통째로 넣는다 (디렉터리 0755, 파일 0644 — 실행 비트가
있는 파일과 *.run 은 0755, 소유자 root). 예: nlcc 의 헤더/라이브러리(/usr/lib), 부트스트랩용 소스(/usr/src).

틀의 모드/소유자는 <roi>/default.owners 에 적는다 (형식은 그 파일 머리말). 복사 규칙
"소유자가 NX_UID_TEMPLATE 이면 새 uid/gid, 아니면 그대로" 는 나중에 실행 중 사용자 생성도 똑같이 따른다.
"""
import os
import sys

UID_TEMPLATE = 0xFFFFFFFE        # include/nyx_abi.h 의 NX_UID_TEMPLATE
UID_RESERVED_MIN = 0x80000000    # NX_UID_RESERVED_MIN
IGNORED = {".keep"}              # 빈 디렉터리를 git 에 남기려는 파일


def load_owners(path):
    table = {}
    if not os.path.exists(path):
        return table
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 3 or parts[2] not in ("root", "user"):
                sys.exit(f"{path}:{n}: expected '<path> <mode> root|user'")
            rel = os.path.normpath(parts[0])
            table[rel] = (int(parts[1], 8), parts[2])
    return table


def template_entries(default_dir, owners):
    """틀의 항목을 (상대 경로, 종류 'd'|'f', 모드, root|user, 호스트 원본) 으로 (부모가 먼저)"""
    def attrs(rel, is_dir):
        if rel in owners:
            return owners[rel]
        return (0o700 if is_dir else 0o644, "user")

    mode, who = attrs(".", True)
    out = [(".", "d", mode, who, None)]
    for root, dirs, files in os.walk(default_dir):
        dirs.sort()
        rel_root = os.path.relpath(root, default_dir)
        for d in dirs:
            rel = os.path.normpath(os.path.join(rel_root, d))
            mode, who = attrs(rel, True)
            out.append((rel, "d", mode, who, None))
        for name in sorted(files):
            if name in IGNORED:
                continue
            rel = os.path.normpath(os.path.join(rel_root, name))
            mode, who = attrs(rel, False)
            out.append((rel, "f", mode, who, os.path.abspath(os.path.join(root, name))))
    known = {e[0] for e in out}
    for rel in owners:
        if rel not in known:
            sys.exit(f"default.owners: '{rel}' is not in {default_dir}")
    return out


def emit_template(out, entries, dest, uid, gid):
    for rel, kind, mode, who, src in entries:
        path = dest if rel == "." else f"{dest}/{rel}"
        u, g = (uid, gid) if who == "user" else (0, 0)
        line = f"{kind} {path} {mode:04o} {u} {g}"
        out.append(line + (f" {src}" if kind == "f" else ""))


def main():
    users_db, bin_dir, roi_dir, app_dir, extra, trees, overrides = None, None, None, None, [], [], {}
    args = sys.argv[1:]
    while args:
        a = args.pop(0)
        if a == "--users":
            users_db = args.pop(0)
        elif a == "--bin":
            bin_dir = args.pop(0)
        elif a == "--roi":
            roi_dir = args.pop(0)
        elif a == "--app":
            app_dir = args.pop(0)
        elif a == "--template-file":
            rel, src = args.pop(0).split("=", 1)
            overrides[os.path.normpath(rel)] = os.path.abspath(src)
        elif a == "--file":
            extra.append(args.pop(0))
        elif a == "--tree":
            trees.append(args.pop(0))
        else:
            sys.exit(__doc__)
    if not users_db or not bin_dir or not roi_dir:
        sys.exit(__doc__)

    users = []
    with open(users_db) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, uid, gid = line.split(":")[:3]
            uid, gid = int(uid), int(gid)
            if uid >= UID_RESERVED_MIN or gid >= UID_RESERVED_MIN:
                sys.exit(f"{users_db}: uid/gid of '{name}' is in the reserved range (>= 2^31)")
            users.append((name, uid, gid))

    out = ["d / 0755 0 0", "d /usr 0755 0 0"]
    runs = sorted(n for n in os.listdir(bin_dir) if n.endswith(".run"))
    defaults = set()
    if app_dir and os.path.isdir(app_dir):
        defaults = {n for n in os.listdir(app_dir) if os.path.isdir(os.path.join(app_dir, n))}
        unknown = sorted(defaults - {r[:-4] for r in runs})
        if unknown:
            sys.exit(f"--app: no program for default settings: {', '.join(unknown)} (expected <name>.run in {bin_dir})")
    for name in runs:
        elf = os.path.abspath(os.path.join(bin_dir, name))
        app = name[:-4]
        if app not in defaults:
            out.append(f"f /usr/{name} 0755 0 0 {elf}")
            continue
        out.append(f"d /usr/{name} 0755 0 0 {elf}")        # 오브젝트: 내용 = ELF, 자식 = 기본 설정
        root = os.path.join(app_dir, app)
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            rel = os.path.relpath(dirpath, root)
            base = f"/usr/{name}" if rel == "." else f"/usr/{name}/{rel}"
            if rel != ".":
                out.append(f"d {base} 0755 0 0")
            for fn in sorted(filenames):
                if fn in IGNORED:
                    continue
                if any(c.isspace() for c in fn):
                    sys.exit(f"--app: whitespace in setting name: {os.path.join(dirpath, fn)}")
                out.append(f"f {base}/{fn} 0644 0 0 {os.path.abspath(os.path.join(dirpath, fn))}")

    out += ["d /roi 0711 0 0", f"f /roi/users 0600 0 0 {os.path.abspath(users_db)}"]
    for value in ("hostname", "timezone"):          # 커널이 info:/<이름> 으로 공개하는 값
        src = os.path.join(roi_dir, value)
        if os.path.exists(src):
            out.append(f"f /roi/{value} 0644 0 0 {os.path.abspath(src)}")

    app_dir = os.path.join(roi_dir, "app")          # root 전용 시스템 설정
    for root, dirs, files in os.walk(app_dir):
        dirs.sort()
        rel = os.path.relpath(root, app_dir)
        base = "/roi/app" if rel == "." else f"/roi/app/{rel}"
        out.append(f"d {base} 0700 0 0")
        for name in sorted(files):
            if name not in IGNORED:
                out.append(f"f {base}/{name} 0600 0 0 {os.path.abspath(os.path.join(root, name))}")

    entries = template_entries(os.path.join(roi_dir, "default"), load_owners(os.path.join(roi_dir, "default.owners")))
    for rel in overrides:
        if not any(e[0] == rel and e[1] == "f" for e in entries):
            sys.exit(f"--template-file: '{rel}' is not a file in the template")
    entries = [(r, k, m, w, overrides.get(r, s) if k == "f" else s) for r, k, m, w, s in entries]
    emit_template(out, entries, "/roi/default", UID_TEMPLATE, UID_TEMPLATE)
    for _, uid, gid in users:
        emit_template(out, entries, f"/roi/{uid}", uid, gid)

    out.append("d /home 0755 0 0")
    for name, uid, gid in users:
        out.append(f"d /home/{name} 0700 {uid} {gid}")

    made = {"/", "/usr", "/roi", "/home"}
    for spec in trees:
        dest, src = spec.split("=", 1)
        dest = dest.rstrip("/")
        parts = dest.strip("/").split("/")
        for k in range(1, len(parts) + 1):            # 위쪽 디렉터리
            d = "/" + "/".join(parts[:k])
            if d not in made:
                out.append(f"d {d} 0755 0 0")
                made.add(d)
        for root, dirs, files in os.walk(src):
            dirs.sort()
            rel = os.path.relpath(root, src)
            base = dest if rel == "." else f"{dest}/{rel}"
            if base not in made:
                out.append(f"d {base} 0755 0 0")
                made.add(base)
            for name in sorted(files):
                p = os.path.join(root, name)
                mode = "0755" if (os.access(p, os.X_OK) or name.endswith(".run")) else "0644"
                out.append(f"f {base}/{name} {mode} 0 0 {os.path.abspath(p)}")

    for spec in extra:
        dest, src = spec.split("=", 1)
        mode = "0644"
        if ":" in src and src.rsplit(":", 1)[1].isdigit():
            src, mode = src.rsplit(":", 1)
        out.append(f"f {dest} {mode} 0 0 {os.path.abspath(src)}")

    print("\n".join(out))


if __name__ == "__main__":
    main()
