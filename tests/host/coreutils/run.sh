#!/bin/sh
# run.sh - coreutils 호스트 검사 (make test-coreutils)
#   sh run.sh <명령 디렉터리> <작업 디렉터리>
# 명령들은 nlibc(플랫폼 계층만 Linux)로 링크된 것이다. 작업 디렉터리는 매번 새로 만든다.
B=$1
W=$2
fails=0
n=0

[ -n "$B" ] && [ -n "$W" ] || { echo "usage: run.sh <bin> <work>"; exit 2; }
B=$(cd -- "$B" && pwd) || exit 1
rm -rf -- "${W:?}"
mkdir -p -- "$W" || exit 1
cd -- "$W" || exit 1

# check <이름> <기대값> <실제값>
check() {
    n=$((n + 1))
    if [ "$2" != "$3" ]; then
        fails=$((fails + 1))
        printf 'FAIL %s\n  expected: [%s]\n  actual:   [%s]\n' "$1" "$2" "$3"
    fi
}
# status <이름> <기대 종료 코드> 명령...
status() {
    name=$1; want=$2; shift 2
    "$@" >/dev/null 2>&1
    check "$name (exit)" "$want" "$?"
}

# echo
check "echo" "hello world" "$($B/echo hello world)"
check "echo -n" "ab" "$($B/echo -n a; $B/echo -n b)"
check "echo -n -n" "x" "$($B/echo -n -n x)"
check "echo -x is text" "-x y" "$($B/echo -x y)"
check "echo none" "" "$($B/echo)"
check "echo \\n" "a
b" "$($B/echo 'a\nb')"
check "echo \\t" "$(printf 'a\tb')" "$($B/echo 'a\tb')"
check "echo \\\\" 'a\b' "$($B/echo 'a\\b')"
check "echo \\c" "ab" "$($B/echo 'ab\cxyz' more)"
check "echo \\e" "$(printf '\033[31mR')" "$($B/echo '\e[31mR')"
check "echo \\0101" "A" "$($B/echo '\0101')"
check "echo \\x41" "AB" "$($B/echo '\x41\x42')"
check "echo \\x no hex" '\xg' "$($B/echo '\xg')"
check "echo unknown \\q" '\q' "$($B/echo '\q')"
check "echo -E literal" 'a\nb' "$($B/echo -E 'a\nb')"
check "echo -e" "a
b" "$($B/echo -e 'a\nb')"
check "echo -nE" 'x\n' "$($B/echo -nE 'x\n'; $B/echo -n)"
check "echo -ne" "x" "$($B/echo -ne 'x')"
check "echo -nx is text" "-nx y" "$($B/echo -nx y)"
check "echo to file" "a
b" "$($B/echo 'a\nb' > esc.txt; cat esc.txt)"

# clear
check "clear" "$(printf '\033[H\033[2J')" "$($B/clear)"
status "clear extra arg" 2 $B/clear x

# touch / cat
status "touch new" 0 $B/touch a.txt
check "touch makes empty" "0" "$(wc -c < a.txt | tr -d ' ')"
status "touch -c missing" 0 $B/touch -c nope.txt
check "touch -c no create" "no" "$([ -e nope.txt ] && echo yes || echo no)"
printf 'one\ntwo\nthree\n' > t.txt
status "touch existing keeps" 0 $B/touch t.txt
check "cat file" "one
two
three" "$($B/cat t.txt)"
check "cat two files" "one
two
three
one
two
three" "$($B/cat t.txt t.txt)"
check "cat stdin" "piped" "$(echo piped | $B/cat)"
check "cat - in middle" "one
two
three
mid" "$(echo mid | $B/cat t.txt -)"
check "cat -n" "     1	one
     2	two
     3	three" "$($B/cat -n t.txt)"
status "cat missing" 1 $B/cat nope.txt
check "cat error text" "cat: nope.txt: No such file or directory" "$($B/cat nope.txt 2>&1)"
status "cat dir" 1 $B/cat .

# head / tail / wc
seq 1 20 > n.txt
check "head default" "$(seq 1 10)" "$($B/head n.txt)"
check "head -n 3" "1
2
3" "$($B/head -n 3 n.txt)"
check "head -n3" "1
2
3" "$($B/head -n3 n.txt)"
check "head -n 0" "" "$($B/head -n 0 n.txt)"
check "head two files" "==> t.txt <==
one

==> n.txt <==
1" "$($B/head -n 1 t.txt n.txt)"
check "tail default" "$(seq 11 20)" "$($B/tail n.txt)"
check "tail -n 2" "19
20" "$($B/tail -n 2 n.txt)"
check "tail -n 0" "" "$($B/tail -n 0 n.txt)"
check "tail more than file" "one
two
three" "$($B/tail -n 50 t.txt)"
printf 'a\nb' > nonl.txt
check "tail no final newline" "b" "$($B/tail -n 1 nonl.txt)"
check "tail stdin" "3" "$(printf '1\n2\n3\n' | $B/tail -n 1)"
status "head bad -n" 2 $B/head -n x n.txt
check "wc" "      3       3      14 t.txt" "$($B/wc t.txt)"
check "wc -l" "     20 n.txt" "$($B/wc -l n.txt)"
check "wc -w stdin" "      4" "$(echo 'a b  c
d' | $B/wc -w)"
check "wc total" "      3 t.txt
     20 n.txt
     23 total" "$($B/wc -l t.txt n.txt)"

# mkdir / rmdir
status "mkdir" 0 $B/mkdir d1
check "mkdir made dir" "yes" "$([ -d d1 ] && echo yes)"
status "mkdir exists" 1 $B/mkdir d1
status "mkdir -p exists" 0 $B/mkdir -p d1
status "mkdir no parent" 1 $B/mkdir x/y
status "mkdir -p deep" 0 $B/mkdir -p p1/p2/p3/
check "mkdir -p made all" "yes" "$([ -d p1/p2/p3 ] && echo yes)"
status "mkdir -m" 0 $B/mkdir -m 700 m1
check "mkdir -m mode" "drwx------" "$(ls -ld m1 | cut -c1-10)"
status "mkdir bad mode" 2 $B/mkdir -m 999 m2
status "rmdir non-empty" 1 $B/rmdir p1
status "rmdir" 0 $B/rmdir p1/p2/p3
check "rmdir removed" "no" "$([ -e p1/p2/p3 ] && echo yes || echo no)"
status "rmdir file" 1 $B/rmdir t.txt

# ls
mkdir -p L/sub
: > L/b
: > L/a
: > L/.hidden
check "ls sorted, dirs marked" "a
b
sub/" "$($B/ls L)"
check "ls -a" ".hidden
a
b
sub/" "$($B/ls -a L)"
check "ls file" "L/a" "$($B/ls L/a)"
check "ls -d" "L/" "$($B/ls -d L)"
check "ls two dirs" "L/sub:

d1:" "$($B/ls L/sub d1)"
check "ls file then dir" "t.txt

L/sub:" "$($B/ls t.txt L/sub)"
chmod 640 L/a
check "ls -l file" "-rw-r----- $(id -u) $(id -g) 0 a" "$($B/ls -l L | head -n 1 | tr -s ' ')"
check "ls -l dir" "d" "$($B/ls -l L | tail -n 1 | cut -c1)"
status "ls missing" 1 $B/ls nope
check "ls cwd" "yes" "$($B/ls | grep -qx 't.txt' && echo yes)"

# cp
status "cp file" 0 $B/cp t.txt c.txt
check "cp content" "$(cat t.txt)" "$(cat c.txt)"
chmod 600 t.txt
$B/cp t.txt c2.txt
check "cp keeps mode" "-rw-------" "$(ls -l c2.txt | cut -c1-10)"
echo old > c3.txt
$B/cp t.txt c3.txt
check "cp overwrite" "$(cat t.txt)" "$(cat c3.txt)"
printf 'ro\n' > ro.txt
chmod 444 ro.txt
status "cp read-only file" 0 $B/cp ro.txt ro2.txt
check "cp read-only content and mode" "ro -r--r--r--" "$(cat ro2.txt) $(ls -l ro2.txt | cut -c1-10)"
status "cp into dir" 0 $B/cp t.txt n.txt d1
check "cp into dir" "n.txt t.txt" "$(ls d1 | tr '\n' ' ' | sed 's/ $//')"
status "cp dir without -r" 1 $B/cp L L2
status "cp -r" 0 $B/cp -r L L2
check "cp -r tree" ".hidden a b sub" "$(ls -A L2 | tr '\n' ' ' | sed 's/ $//')"
check "cp -r subdir" "yes" "$([ -d L2/sub ] && echo yes)"
status "cp -r into itself" 1 $B/cp -r L L/sub/x
status "cp many to file" 1 $B/cp t.txt n.txt c.txt
status "cp missing" 1 $B/cp nope.txt x.txt

# mv
status "mv rename" 0 $B/mv c.txt m.txt
check "mv moved" "no yes" "$([ -e c.txt ] && echo yes || echo no) $([ -e m.txt ] && echo yes)"
echo keep > m2.txt
status "mv overwrite" 0 $B/mv m.txt m2.txt
check "mv overwrote" "$(cat t.txt)" "$(cat m2.txt)"
status "mv into dir" 0 $B/mv m2.txt c2.txt d1
check "mv into dir" "c2.txt m2.txt n.txt t.txt" "$(ls d1 | tr '\n' ' ' | sed 's/ $//')"
status "mv dir" 0 $B/mv L2 L3
check "mv dir moved" "yes" "$([ -d L3/sub ] && echo yes)"
status "mv missing" 1 $B/mv nope.txt x.txt

# chmod
: > p.txt
status "chmod octal" 0 $B/chmod 640 p.txt
check "chmod 640" "-rw-r-----" "$(ls -l p.txt | cut -c1-10)"
$B/chmod u+x,g-r p.txt
check "chmod u+x,g-r" "-rwx------" "$(ls -l p.txt | cut -c1-10)"
$B/chmod a=r p.txt
check "chmod a=r" "-r--r--r--" "$(ls -l p.txt | cut -c1-10)"
$B/chmod go+w p.txt
check "chmod go+w" "-r--rw-rw-" "$(ls -l p.txt | cut -c1-10)"
$B/chmod -w p.txt
check "chmod -w (umask)" "-r--rw-rw-" "$(umask 022; ls -l p.txt | cut -c1-10)"
$B/chmod 644 p.txt
(umask 022; $B/chmod +x p.txt)
check "chmod +x respects umask" "-rwxr-xr-x" "$(ls -l p.txt | cut -c1-10)"
status "chmod bad mode" 2 $B/chmod 8 p.txt
status "chmod bad symbolic" 2 $B/chmod u+q p.txt
status "chmod missing file" 1 $B/chmod 644 nope.txt

# rm
status "rm file" 0 $B/rm a.txt
check "rm removed" "no" "$([ -e a.txt ] && echo yes || echo no)"
status "rm missing" 1 $B/rm a.txt
status "rm -f missing" 0 $B/rm -f a.txt
status "rm -f no operand" 0 $B/rm -f
status "rm dir without -r" 1 $B/rm d1
status "rm -r" 0 $B/rm -r L3
check "rm -r removed" "no" "$([ -e L3 ] && echo yes || echo no)"
mkdir -p big
i=0
while [ $i -lt 40 ]; do : > big/f$i; i=$((i + 1)); done
status "rm -rf many" 0 $B/rm -rf big
check "rm -rf removed all" "no" "$([ -e big ] && echo yes || echo no)"
status "rm refuses ." 1 $B/rm -rf .
status "rm refuses .." 1 $B/rm -rf d1/..

# cmp
printf 'abc\ndef\n' > c1.txt; printf 'abc\ndxf\n' > c2.txt; cp c1.txt c3.txt
status "cmp same" 0 $B/cmp c1.txt c3.txt
status "cmp differ" 1 $B/cmp c1.txt c2.txt
check "cmp message" "c1.txt c2.txt differ: byte 6, line 2" "$($B/cmp c1.txt c2.txt)"
check "cmp -s quiet" "" "$($B/cmp -s c1.txt c2.txt)"
printf 'abc' > c4.txt
status "cmp shorter" 1 $B/cmp c1.txt c4.txt
status "cmp missing" 2 $B/cmp c1.txt nope.txt

# 기타
check "pwd" "$(pwd -P)" "$(PWD=$(pwd -P) $B/pwd)"
status "true" 0 $B/true
status "false" 1 $B/false
status "sleep 0.1" 0 $B/sleep 0.1
status "sleep bad" 2 $B/sleep x
status "unknown option" 2 $B/ls -z

# nyfetch (호스트에는 info:/ 가 없다: 제목줄과 구분선, 환경 변수 항목만)
out=$(USER=u LOGNAME=u SHELL=app:/nysh.run TERM=dumb $B/nyfetch)
check "nyfetch status" 0 "$?"
check "nyfetch shell" "Shell: nysh" "$(printf '%s\n' "$out" | grep '^Shell:')"
check "nyfetch term" "Terminal: dumb" "$(printf '%s\n' "$out" | grep '^Terminal:')"
check "nyfetch no color" "" "$(printf '%s\n' "$out" | grep -c "$(printf '\033')" | grep -v '^0$')"
status "nyfetch usage" 2 $B/nyfetch x

echo "coreutils: $((n - fails))/$n checks passed"
[ $fails -eq 0 ] && echo "[test-coreutils] OK"
[ $fails -eq 0 ]
