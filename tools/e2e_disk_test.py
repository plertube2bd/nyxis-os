#!/usr/bin/env python3
"""
e2e_disk_test.py - disk:/ (SATA 디스크의 nyfs) 가 재부팅 뒤에도 남는지 확인한다 (make test-disk)

새 디스크 이미지를 만들어(nyfs-pack, 뿌리 0777) SELFTEST 이미지와 함께 두 번 부팅한다:
  1회차: test 로 로그인 -> disk:/ 에 작은 파일, 디렉터리, 큰 파일(중간 버퍼 64KiB 를 넘는 크기) 쓰기 -> poweroff
  2회차: 같은 디스크로 다시 부팅 -> 내용이 그대로인지 (cat, cmp) -> poweroff

  python3 tools/e2e_disk_test.py <out 디렉터리> <nyfs-pack 경로>
"""
import re, socket, subprocess, sys, time, os, shutil, tempfile

out, pack = sys.argv[1], sys.argv[2]
S = tempfile.mkdtemp(prefix="nyx-disk-")
disk = f"{S}/disk.img"
open(f"{S}/disk.manifest", "w").write("d / 0777 0 0\n")
subprocess.run([pack, "-s", "16M", "-o", disk, f"{S}/disk.manifest"], check=True, stdout=subprocess.DEVNULL)
pw = [l.split(":", 1)[1].strip() for l in open(f"{out}/test-password") if l.startswith("test:")][0]
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
steps = []

def step(name, ok):
    steps.append((name, ok)); print(("PASS " if ok else "FAIL ") + name, flush=True)

def boot(n):
    """QEMU 를 띄우고 test 로 로그인한다. (run, qemu, buf 접근자) 를 돌려준다"""
    ser = f"{S}/ser{n}.sock"
    shutil.copy("/usr/share/OVMF/OVMF_VARS_4M.fd", f"{S}/OVMF_VARS.fd")
    q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-m", "1024",
        "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
        "-drive", f"if=pflash,format=raw,file={S}/OVMF_VARS.fd",
        "-drive", f"file={out}/esp.img,format=raw",
        "-drive", f"file={disk},format=raw",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-serial", f"unix:{ser},server=on,wait=on"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.connect(ser); s.settimeout(0.2)
    st = {"buf": b""}

    def pump(t):
        end = time.time() + t
        while time.time() < end:
            try:
                d = s.recv(4096)
                if not d: return False
                st["buf"] = ANSI.sub(b"", st["buf"] + d)
            except socket.timeout: pass
        return True

    def wait_for(token, t=60, start=0):
        end = time.time() + t
        while time.time() < end:
            if token.encode() in st["buf"][start:]: return True
            if not pump(0.3): return token.encode() in st["buf"][start:]
        return False

    def run(cmd, t=30):
        mark = len(st["buf"]); s.sendall((cmd + "\r").encode())
        wait_for("\n~* ", t, mark + len(cmd))
        return st["buf"][mark:].replace(b"\r", b"")

    step(f"boot {n}: disk:/ mounted", wait_for("Persistent storage: disk:/", 240))
    wait_for("nyxis os login: ", 60)
    s.sendall(b"test\r"); wait_for("password: ", 10); time.sleep(0.3)
    s.sendall((pw + "\r").encode())
    step(f"boot {n}: logged in", wait_for("~* ", 15))
    return run, q, st

def power_off(n, run, q):
    run("poweroff", 5)
    try:
        q.wait(timeout=30); step(f"boot {n}: powered off", True)
    except subprocess.TimeoutExpired:
        q.kill(); step(f"boot {n}: powered off", False)

# 1회차: 쓰기
run, q, st = boot(1)
run("echo persist-123 > disk:/note.txt")
run("mkdir disk:/d")
run("cp app:/nysh.run disk:/d/big.run", 60)
o = run("ls disk:/")
step("boot 1: files written", b"\nd/\n" in o and b"\nnote.txt\n" in o)
step("boot 1: big file copied", b"\nSAME\n" in run("cmp app:/nysh.run disk:/d/big.run && echo SAME", 60))
power_off(1, run, q)
open(f"{out}/e2e-disk1.log", "wb").write(st["buf"])

# 2회차: 그대로 있는지
run, q, st = boot(2)
step("boot 2: small file survived reboot", b"\npersist-123\n" in run("cat disk:/note.txt"))
step("boot 2: big file survived reboot", b"\nSAME\n" in run("cmp app:/nysh.run disk:/d/big.run && echo SAME", 60))
run("rm -r disk:/d")
step("boot 2: rm works", b"No such file or directory" in run("ls disk:/d"))
power_off(2, run, q)
open(f"{out}/e2e-disk2.log", "wb").write(st["buf"])

shutil.rmtree(S, ignore_errors=True)
sys.exit(0 if all(ok for _, ok in steps) else 1)
