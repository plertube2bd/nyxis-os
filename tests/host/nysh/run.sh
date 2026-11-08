#!/bin/sh
# run.sh - nysh 호스트 검사 (make test-coreutils 의 두 번째 단계)
#   sh run.sh <명령 디렉터리(nysh 와 coreutils)> <작업 디렉터리>
# nysh 는 이름만 준 명령을 NL_HOST_PATH 디렉터리에서 찾는다 (호스트 플랫폼 계층의 검사용 규칙).
B=$1
W=$2
fails=0
n=0

[ -n "$B" ] && [ -n "$W" ] || { echo "usage: run.sh <bin> <work>"; exit 2; }
B=$(cd -- "$B" && pwd) || exit 1
rm -rf -- "${W:?}"
mkdir -p -- "$W" || exit 1
cd -- "$W" || exit 1
W=$(pwd -P)
# 검사 줄에서 쓰는 호스트 도구 (nysh 가 찾을 수 있도록 같은 디렉터리에 링크)
for t in sh tr sed grep cut printf; do
    for d in /usr/bin /bin; do
        if [ -x "$d/$t" ]; then ln -sf "$d/$t" "$B/$t"; break; fi
    done
done
export NL_HOST_PATH="$B"
export HOME="$W"
NY="$B/nysh"

check() {
    n=$((n + 1))
    if [ "$2" != "$3" ]; then
        fails=$((fails + 1))
        printf 'FAIL %s\n  expected: [%s]\n  actual:   [%s]\n' "$1" "$2" "$3"
    fi
}
# sh_out <이름> <기대 출력> <nysh 명령줄>
sh_out() {
    check "$1" "$2" "$("$NY" -c "$3" 2>&1)"
}
# sh_rc <이름> <기대 종료 코드> <nysh 명령줄>
sh_rc() {
    "$NY" -c "$3" >/dev/null 2>&1
    check "$1 (exit)" "$2" "$?"
}

# 단어와 따옴표
sh_out "simple" "a b c" 'echo a   b c'
sh_out "single quotes" 'a  $X "q"' "echo 'a  \$X \"q\"'"
sh_out "double quotes" "a  b" 'echo "a  b"'
sh_out "backslash" 'a b$' 'echo a\ b\$'
sh_out "empty arg" "[] 1" 'echo [""] 1'
sh_out "adjacent quotes" "abc" "echo a'b'\"c\""
sh_out "comment" "x" 'echo x # not this'
sh_out "unterminated" "nysh: syntax: unterminated '" "echo 'abc"

# 변수
sh_out "assign and use" "hello world" 'X=hello; echo $X world'
sh_out "braces" "abcdef" 'X=abc; echo ${X}def'
sh_out "in double quotes" "v=1 2" 'X="1 2"; echo "v=$X"'
sh_out "unset var empty" "[]" 'echo [$NO_SUCH_VAR_XYZ]'
sh_out "dollar alone" '$ a$' 'echo $ a$'
sh_out "status" "1 0" 'false; A=$?; true; echo $A $?'
sh_out "export to child" "val" 'export NY_T=val; sh -c "echo \$NY_T"'
sh_out "prefix assign only child" "inner
|" 'NY_P=inner sh -c "echo \$NY_P"; echo "|$NY_P"'
sh_out "unset" "[]" 'export NY_U=1; unset NY_U; echo [$NY_U]'
sh_out "env lists" "NY_E=5" 'export NY_E=5; env | grep NY_E='
sh_out "tilde" "$W $W/x" 'echo ~ ~/x'
sh_out "tilde quoted" "~" "echo '~'"

# 목록과 조건
sh_out "semicolon" "1
2" 'echo 1; echo 2'
sh_out "and" "ok" 'true && echo ok'
sh_out "and skip" "" 'false && echo no'
sh_out "or" "alt" 'false || echo alt'
sh_out "or skip" "" 'true || echo no'
sh_out "chain" "b
c" 'false && echo a || echo b; echo c'
sh_rc "exit code" 3 'exit 3'
sh_rc "last status" 1 'true; false'
sh_rc "not found" 127 'no_such_command_xyz'
sh_out "not found message" "nysh: no_such_command_xyz: command not found" 'no_such_command_xyz'
sh_rc "syntax error" 2 'echo a |'

# 파이프
sh_out "pipe" "3" 'printf "a\nb\nc\n" | wc -l | tr -d " "'
sh_out "three stage" "B" 'printf "a\nb\n" | tail -n 1 | tr a-z A-Z'
sh_out "pipe status is last" "0" 'false | true; echo $?'
sh_out "builtin in pipe" "$W" 'pwd | cat'

# 리디렉션
sh_out "out and in" "hi" 'echo hi > f.txt; cat < f.txt'
sh_out "truncate" "2" 'echo 1 > t.txt; echo 2 > t.txt; cat t.txt'
sh_out "append" "1
2" 'echo 1 > a.txt; echo 2 >> a.txt; cat a.txt'
sh_out "stderr to file" "" 'cat no_such_file 2> e.txt'
check "stderr file content" "cat: no_such_file: No such file or directory" "$(cat e.txt)"
sh_out "2>&1 into pipe" "1" 'cat no_such_file 2>&1 | wc -l | tr -d " "'
check ">&2 goes to stderr" "" "$("$NY" -c 'echo err >&2' 2>/dev/null)"
check ">&2 visible on stderr" "err" "$("$NY" -c 'echo err >&2' 2>&1 >/dev/null)"
sh_out "builtin redirect restored" "after" 'pwd > p.txt; echo after'
check "builtin redirect content" "$W" "$(cat p.txt)"
sh_out "redirect missing file" "nysh: nope.txt: No such file or directory" 'cat < nope.txt'
sh_out "missing redirect target" "nysh: syntax: missing file name after redirection" 'echo >'

# cd / pwd
mkdir -p d1/d2
sh_out "cd and pwd" "$W/d1/d2" 'cd d1/d2; pwd'
sh_out "cd home" "$W" 'cd d1; cd; pwd'
sh_out "cd -" "$W" 'cd d1; cd -'
sh_out "cd passes PWD to child" "$W/d1" 'cd d1; sh -c "echo \$PWD"'
sh_out "cd relative child" "x" 'echo x > d1/x.txt; cd d1; cat x.txt'
sh_rc "cd missing" 1 'cd no_such_dir'
sh_out "cd error" "nysh: cd: no_such_dir: No such file or directory" 'cd no_such_dir'

# 글롭
mkdir g
: > g/a.txt; : > g/b.txt; : > g/c.log; : > g/.h.txt
sh_out "glob star" "g/a.txt g/b.txt" 'echo g/*.txt'
sh_out "glob question" "g/a.txt" 'echo g/?.txt | cut -d" " -f1'
sh_out "glob no match literal" "g/*.zzz" 'echo g/*.zzz'
sh_out "glob quoted" "g/*.txt" 'echo "g/*.txt"'
sh_out "glob hidden" "g/.h.txt" 'echo g/.*.txt'
sh_out "glob cwd" "a.txt b.txt c.log" 'cd g; echo *'
sh_out "glob in rm" "c.log" 'cd g; rm *.txt; ls'

# 백그라운드
sh_out "background" "fg" 'sleep 0.2 & echo fg'

# source
printf 'SRC_V=from_file\necho sourced\n' > s.nysh
sh_out "source" "sourced
from_file" 'source s.nysh; echo $SRC_V'
sh_out "dot" "sourced" '. s.nysh'

# 스크립트 파일과 표준 입력
printf 'echo line1\nX=2\necho line$X\n' > script.nysh
check "script file" "line1
line2" "$("$NY" script.nysh 2>&1)"
check "stdin script" "piped" "$(echo 'echo piped' | "$NY" 2>&1)"
check "no prompt when not a tty" "" "$(printf '' | "$NY" 2>&1)"

# 내장
sh_out "whoami" "1" 'whoami | wc -l | tr -d " "'
sh_out "help" "1" 'help | head -n 1 | wc -l | tr -d " "'

echo "nysh: $((n - fails))/$n checks passed"
[ $fails -eq 0 ] && echo "[test-nysh] OK"
[ $fails -eq 0 ]
