#!/usr/bin/env python3
"""
e2e_login_test.py - NyxisOS 종단 간 검사 (make test-e2e)

SELFTEST 이미지를 QEMU 로 부팅해 콘솔로 실제 사용 흐름을 흉내 낸다:
  selftest 통과 -> init 이 띄운 login -> 틀린 비밀번호 거부 -> test 계정 로그인(비밀번호 에코 없음)
  -> nysh 에서 whoami / 없는 명령 / nlibctest(C 라이브러리 검사) / fputest(FPU 상태) / sudo -> poweroff -> init 이 요청자 uid 를 확인하고 ACPI S5 로 전원 끔(QEMU 종료)

  python3 tools/e2e_login_test.py <out 디렉터리> [serial|ps2]
    serial: COM1 로 입력 (IRQ4),  ps2: QEMU 모니터 sendkey 로 PS/2 키보드 입력 (IRQ1)
"""
import re, socket, subprocess, sys, time, os, shutil, tempfile
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
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
def pump(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: return False
            buf += d
            buf = ANSI.sub(b"", buf)   # 프롬프트/로그의 색 시퀀스는 빼고 글자만 비교한다
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
        keymap = {" ":"spc", "\r":"ret", ":":"shift-semicolon", "-":"minus", "_":"shift-minus", ".":"dot", "/":"slash",
                  ";":"semicolon", "*":"shift-8", "~":"shift-grave_accent", ">":"shift-dot", "<":"shift-comma",
                  "|":"shift-backslash", "&":"shift-7", "$":"shift-4", "=":"equal", "\"":"shift-apostrophe",
                  "'":"apostrophe", "?":"shift-slash"}
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
step("shell prompt as test (~*)", wait_for("~* ", 15))
step("password not echoed", pw.encode() not in buf[before:])
send("whoami\r"); pump(1.5); step("whoami prints test", b"\ntest\r\n" in buf[-200:].replace(b"\r\r", b"\r") or b"whoami\r\ntest" in buf)
send("nosuchcmd\r"); step("unknown command", wait_for("nosuchcmd: command not found", 10))
mark = len(buf); send("cat info:/hostname roi:/1000/shell\r")
step("cat info:/hostname and roi:/1000/shell", wait_for("app:/nysh.run", 10, mark) and b"nyxis" in buf[mark:])
mark = len(buf); send("cat roi:/hostname\r"); step("roi:/hostname is root-only", wait_for("Permission denied", 10, mark))
mark = len(buf); send("ls info:/memory\r"); step("ls info:/memory", wait_for("free_kib", 10, mark))
def run(cmd, t=15, prompt="~* "):
    mark = len(buf); send(cmd + "\r"); wait_for("\n" + prompt, t, mark + len(cmd))
    return buf[mark:].replace(b"\r", b"")
# 명령어 (userland/coreutils, app:/<이름>.run). ps2 keymap 에 있는 글자만 쓴다
run("mkdir -p tmp:/cu/sub"); run("cp info:/hostname tmp:/cu/h")
step("coreutils: mkdir -p, cp, cat", b"\nnyxis\n" in run("cat tmp:/cu/h"))
step("coreutils: echo", b"\nhello nyxis\n" in run("echo hello nyxis"))
run("touch tmp:/cu/e"); run("mv tmp:/cu/h tmp:/cu/sub")
o = run("ls tmp:/cu"); o2 = run("ls tmp:/cu/sub")
step("coreutils: touch, mv, ls", b"\ne\nsub/\n" in o and b"\nh\n" in o2)
step("coreutils: wc", b"      1       1       6 tmp:/cu/sub/h" in run("wc tmp:/cu/sub/h"))
run("rm -r tmp:/cu")
o = run("nyfetch"); print(o.decode(errors="replace"), flush=True)
step("coreutils: nyfetch", b"\ntest@nyxis\n" in o and b"\nOS: Nyxis " in o and b"\nMemory: " in o and b"\nShell: nysh\n" in o)
step("coreutils: rm -r", b"ls: tmp:/cu: No such file or directory" in run("ls tmp:/cu"))
# 셸 (nysh): 프롬프트, cd, 리디렉션, 파이프, 변수, 글롭, 조건 실행
o = run("cd tmp:/", prompt="tmp:/* ")
step("nysh: cd tmp:/ -> prompt tmp:/*", b"\ntmp:/* " in o)
run("mkdir work; cd work", prompt="work* ")
o = run("echo hello > a.txt; echo world >> a.txt; cat a.txt | wc -l", prompt="work* ")
step("nysh: > >> and pipe", b"\n      2\n" in o)
o = run("X=nyxis; echo \"hi $X\" > b.txt; cat b.txt", prompt="work* ")
step("nysh: variables and quotes", b"\nhi nyxis\n" in o)
o = run("echo *.txt", prompt="work* ")
step("nysh: glob", b"\na.txt b.txt\n" in o)
o = run("cat nope.txt || echo failed", prompt="work* ")
step("nysh: || and error message", b"No such file or directory" in o and b"\nfailed\n" in o)
o = run("cd dev:/", prompt="dev:/* ")
o2 = run("ls", prompt="dev:/* ")
step("nysh: cd dev:/ -> prompt dev:/*, ls", b"\ndev:/* " in o and b"\nstdout\n" in o2 and b"\ntty\n" in o2)
o = run("cd", prompt="~* ")
step("nysh: cd -> ~*", b"\n~* " in o)
run("rm -r tmp:/work")
# Ctrl-C: 포그라운드 작업만 끝내고 셸과 백그라운드 작업은 남는다
def ctrl_c():
    if mode == "ps2":
        m = socket.socket(socket.AF_UNIX); m.connect(mon); m.sendall(b"sendkey ctrl-c\n"); m.close()
    else:
        s.sendall(b"\x03")
def ctrl_backslash():
    if mode == "ps2":
        m = socket.socket(socket.AF_UNIX); m.connect(mon); m.sendall(b"sendkey ctrl-backslash\n"); m.close()
    else:
        s.sendall(b"\x1c")
def interrupt(cmd, settle=1.5, key=None):
    """cmd 를 실행하고 잠시 뒤 Ctrl-C. 프롬프트가 다시 나오면 그 사이의 출력을 돌려준다 (안 나오면 None)"""
    mark = len(buf); send(cmd + "\r"); pump(settle); (key or ctrl_c)()
    ok = wait_for("\n~* ", 10, mark + len(cmd))
    return buf[mark:].replace(b"\r", b"") if ok else None
o = interrupt("sleep 100")
step("ctrl-c: kills sleep, prompt returns", o is not None and b"^C\n" in o)
step("ctrl-c: exit status 130, no message", b"\n130\n" in run("echo $?") and o is not None and b"Killed" not in o)
o = interrupt("cat | wc -l")
step("ctrl-c: kills a pipeline blocked on input", o is not None)
mark = len(buf); send("echo should-not-run"); pump(0.5); ctrl_c()
step("ctrl-c at prompt: discards the line", wait_for("\n~* ", 10, mark) and b"\nshould-not-run\n" not in run("echo after")
     and b"\nafter\n" in buf[mark:].replace(b"\r", b""))
run("sleep 3 &")
interrupt("sleep 100", 0.5)
pump(3.5)
step("ctrl-c: background job survives", b"(exit 0)" in run("echo bg"))
mark = len(buf); send("nysh\r"); pump(1.5); ctrl_c(); pump(1)
o = run("echo inner-alive; exit 7")
step("ctrl-c: nested shell at its prompt survives", b"\ninner-alive\n" in o and b"\n7\n" in run("echo $?"))
o = interrupt("sleep 100", key=ctrl_backslash)
step("ctrl-\\: SIGQUIT ends the job (Quit, 131)", o is not None and b"Quit" in o and b"\n131\n" in run("echo $?"))
if mode == "serial":
    # 신호 처리기: Ctrl-C 를 가로채고 계속 실행한다 (커널 신호, nlibc sigaction/pause)
    run("echo '#include <signal.h>\\n#include <stdio.h>\\n#include <unistd.h>\\n"
        "static volatile sig_atomic_t got;\\nstatic void h(int s){got=s;}\\n"
        "int main(void){signal(SIGINT,h);pause();printf(\"caught %d\\\\n\",(int)got);return 0;}' > tmp:/trap.c")
    run("nlcc -o tmp:/trap.run tmp:/trap.c", 60)
    o = interrupt("tmp:/trap.run")
    step("signals: a program catches Ctrl-C with a handler", o is not None and b"\ncaught 2\n" in o
         and b"\n0\n" in run("echo $?"))
    run("rm tmp:/trap.c tmp:/trap.run")
    # CPU 를 계속 쓰는(시스템 콜을 하지 않는) 프로그램도 끝난다
    run("echo 'int main(void){for(;;);}' > tmp:/spin.c")
    run("nlcc -o tmp:/spin.run tmp:/spin.c", 60)
    o = interrupt("tmp:/spin.run")
    step("ctrl-c: kills a busy loop", o is not None)
    run("rm tmp:/spin.c tmp:/spin.run")
# nlcc (Nyxis 안의 C 컴파일러 = TinyCC): 예제를 빌드해 실행하고, 자기 자신을 두 번 컴파일해 같은지 본다
o = run("nlcc -v")
step("nlcc: version", b"tcc version" in o and b"Nyxis" in o)
run("nlcc -o hello.run app:/src/examples/hello.c", 120)
o = run("./hello.run")
step("nlcc: build and run hello.c inside Nyxis", b"Hello from nlibc!" in o and b"sqrt(2) = 1.414213562373095" in o)
o = run("nlcc -DONE_SOURCE=1 -o tcc1.run app:/src/tcc/tcc.c && ./tcc1.run -DONE_SOURCE=1 -o tcc2.run app:/src/tcc/tcc.c && cmp tcc1.run tcc2.run && echo BOOTSTRAP-OK", 900)
step("nlcc: self-hosting bootstrap (stage1 == stage2)", b"\nBOOTSTRAP-OK\n" in o)
run("rm hello.run tcc1.run tcc2.run")
mark = len(buf); send("nlibctest\r")
ok = wait_for("atexit 2 1", 60, mark)
step("nlibc self-test (app:/nlibctest.run)", ok and b"checks passed" in buf[mark:] and b"FAIL" not in buf[mark:])
mark = len(buf); send("hello\r")
step("nlibc example built by nlcc (app:/hello.run)", wait_for("now: ", 20, mark) and b"Hello from nlibc!" in buf[mark:]
     and b"sqrt(2) = 1.414213562373095, pi = 3.1415926536" in buf[mark:])
mark = len(buf); send("fputest\r")
ok = wait_for("fputest: ", 90, mark) and wait_for("\n", 5, buf.index(b"fputest: ", mark))
step("per-process FPU/SSE state (app:/fputest.run)", b"fputest: OK" in buf[mark:])
# sudo: init 이 sudoers(roi:/app/0/init/sudoers.dat: 0, 1000)와 비밀번호를 확인하고 명령을 root 로 띄운다
def sudo(cmd, password, t=20):
    mark = len(buf); send(cmd + "\r")
    if not wait_for("password for test: ", 10, mark): return b""
    send(password + "\r"); wait_for("\n~* ", t, mark)
    return buf[mark:].replace(b"\r", b"")
o = sudo("sudo cat roi:/hostname", "wrong-password")
step("sudo: wrong password is refused", b"sudo: authentication failed" in o and b"\nnyxis\n" not in o)
o = sudo("sudo cat roi:/hostname", pw)
step("sudo: runs the command as root (reads roi:/hostname)", b"\nnyxis\n" in o and b"init: sudo by uid 1000: app:/cat.run" in o)
o = sudo("sudo false; echo rc=$?", pw)
step("sudo: passes the exit code back", b"\nrc=1\n" in o)
send("poweroff\r")
step("init got request from uid 1000", wait_for("init: power off requested by uid 1000", 15))
try:
    q.wait(timeout=20); step("QEMU powered off (ACPI S5)", True)
except subprocess.TimeoutExpired:
    q.kill(); step("QEMU powered off (ACPI S5)", False)
open(f"{out}/e2e.log","wb").write(buf)
sys.exit(0 if all(ok for _, ok in steps) else 1)
