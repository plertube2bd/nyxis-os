#!/usr/bin/env python3
"""
e2e_login_test.py - NyxisOS 종단 간 검사 (make test-e2e)

SELFTEST 이미지를 QEMU 로 부팅해 콘솔로 실제 사용 흐름을 흉내 낸다:
  selftest 통과 -> init 이 띄운 login -> 틀린 비밀번호 거부 -> test 계정 로그인(비밀번호 에코 없음)
  -> nysh 에서 whoami / 없는 명령 -> poweroff -> init 이 요청자 uid 를 확인하고 ACPI S5 로 전원 끔(QEMU 종료)

  python3 tools/e2e_login_test.py <out 디렉터리> [serial|ps2]
    serial: COM1 로 입력 (IRQ4),  ps2: QEMU 모니터 sendkey 로 PS/2 키보드 입력 (IRQ1)
"""
import socket, subprocess, sys, time, os, shutil, tempfile
out = sys.argv[1]; mode = sys.argv[2] if len(sys.argv) > 2 else "serial"
S = tempfile.mkdtemp(prefix="nyx-e2e-")
ser, mon = f"{S}/ser.sock", f"{S}/mon.sock"
for p in (ser, mon):
    if os.path.exists(p): os.unlink(p)
pw = [l.split(":",1)[1].strip() for l in open(f"{out}/test-password") if l.startswith("test:")][0]
shutil.copy("/usr/share/OVMF/OVMF_VARS_4M.fd", f"{out}/OVMF_VARS.fd")
q = subprocess.Popen(["qemu-system-x86_64","-machine","q35","-m","1024",
  "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
  "-drive",f"if=pflash,format=raw,file={out}/OVMF_VARS.fd","-drive",f"file={out}/esp.img,format=raw",
  "-vga","std","-display","none","-no-reboot",
  "-serial",f"unix:{ser},server=on,wait=on","-monitor",f"unix:{mon},server=on,wait=off"],
  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.5)
s = socket.socket(socket.AF_UNIX); s.connect(ser); s.settimeout(0.2)
buf = b""
def pump(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: return False
            buf += d
        except socket.timeout: pass
    return True
def wait_for(token, t=60, start=0):
    end = time.time() + t
    while time.time() < end:
        if token.encode() in buf[start:]: return True
        if not pump(0.3): return token.encode() in buf[start:]
    return False
def send(text):
    if mode == "ps2":
        m = socket.socket(socket.AF_UNIX); m.connect(mon)
        keymap = {" ":"spc", "\r":"ret", ":":"shift-semicolon", "-":"minus", "_":"shift-minus", ".":"dot", "/":"slash"}
        for ch in text:
            if ch.isupper(): k = "shift-" + ch.lower()
            else: k = keymap.get(ch, ch)
            m.sendall(f"sendkey {k}\n".encode()); time.sleep(0.05)
        m.close()
    else:
        s.sendall(text.encode())
steps = []
def step(name, ok): steps.append((name, ok)); print(("PASS " if ok else "FAIL ") + name, flush=True)
step("selftest passed", wait_for("SELFTEST PASSED", 240))
step("login prompt", wait_for("nyxis os login: ", 30))
mark = len(buf); send("test\r"); step("password prompt", wait_for("password: ", 10))
send("wrong-pass\r"); step("wrong password rejected", wait_for("Login incorrect", 15))
step("prompt again", wait_for("nyxis os login: ", 10) and buf.count(b"nyxis os login: ") >= 2)
mark = len(buf); send("test\r"); wait_for("password: ", 10, mark); time.sleep(0.3); before = len(buf)
send(pw + "\r")
step("shell prompt as test", wait_for("test@nyxis$ ", 15))
step("password not echoed", pw.encode() not in buf[before:])
send("whoami\r"); pump(1.5); step("whoami prints test", b"\ntest\r\n" in buf[-200:].replace(b"\r\r", b"\r") or b"whoami\r\ntest" in buf)
send("nosuchcmd\r"); step("unknown command", wait_for("nosuchcmd: command not found", 10))
send("poweroff\r")
step("init got request from uid 1000", wait_for("init: power off requested by uid 1000", 15))
try:
    q.wait(timeout=20); step("QEMU powered off (ACPI S5)", True)
except subprocess.TimeoutExpired:
    q.kill(); step("QEMU powered off (ACPI S5)", False)
open(f"{out}/e2e.log","wb").write(buf)
sys.exit(0 if all(ok for _, ok in steps) else 1)
