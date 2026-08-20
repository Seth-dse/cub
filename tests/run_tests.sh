#!/bin/sh
# Cub test runner.
#
#   tests/run/*.cub      compile, run, and compare stdout with the .out file
#   tests/errors/*.cub   must fail to compile; .err lines must all appear
#   tests/runtime/*.cub  must compile, then fail at runtime; .err lines must appear
set -u
cd "$(dirname "$0")/.."
CUBC=./cubc
TMP=tests/tmp
rm -rf $TMP && mkdir -p $TMP
pass=0; fail=0
red=""; green=""; off=""
if [ -t 1 ]; then red="\033[31m"; green="\033[32m"; off="\033[0m"; fi

report_fail() {
    printf "${red}FAIL${off} %s\n" "$1"
    shift
    for line in "$@"; do printf "     %s\n" "$line"; done
    fail=$((fail + 1))
}

for src in tests/run/*.cub; do
    [ -e "$src" ] || break
    name=$(basename "$src" .cub)
    exp="tests/run/$name.out"
    if ! $CUBC "$src" -o "$TMP/$name" > "$TMP/$name.build" 2>&1; then
        report_fail "$name (did not compile)" "$(head -5 "$TMP/$name.build")"
        continue
    fi
    "$TMP/$name" > "$TMP/$name.got" 2>&1
    if [ ! -f "$exp" ]; then
        report_fail "$name (no expected output file)"
        continue
    fi
    if diff -u "$exp" "$TMP/$name.got" > "$TMP/$name.diff" 2>&1; then
        pass=$((pass + 1))
    else
        report_fail "$name (wrong output)" "$(head -20 "$TMP/$name.diff")"
    fi
done

check_messages() {
    # $1 = file with actual output, $2 = file with required fragments
    while IFS= read -r want; do
        [ -z "$want" ] && continue
        grep -qF "$want" "$1" || { echo "missing: $want"; return 1; }
    done < "$2"
    return 0
}

for src in tests/errors/*.cub; do
    [ -e "$src" ] || break
    name=$(basename "$src" .cub)
    if $CUBC --check "$src" > "$TMP/$name.got" 2>&1; then
        report_fail "$name (compiled, but should not have)"
        continue
    fi
    miss=$(check_messages "$TMP/$name.got" "tests/errors/$name.err")
    if [ -z "$miss" ]; then
        pass=$((pass + 1))
    else
        report_fail "$name (wrong message)" "$miss" "$(head -4 "$TMP/$name.got")"
    fi
done

for src in tests/runtime/*.cub; do
    [ -e "$src" ] || break
    name=$(basename "$src" .cub)
    if ! $CUBC "$src" -o "$TMP/$name" > "$TMP/$name.build" 2>&1; then
        report_fail "$name (did not compile)" "$(head -5 "$TMP/$name.build")"
        continue
    fi
    "$TMP/$name" > "$TMP/$name.got" 2>&1
    code=$?
    if [ $code -eq 0 ]; then
        report_fail "$name (ran fine, but should have stopped)"
        continue
    fi
    miss=$(check_messages "$TMP/$name.got" "tests/runtime/$name.err")
    if [ -z "$miss" ]; then
        pass=$((pass + 1))
    else
        report_fail "$name (wrong message)" "$miss" "$(head -4 "$TMP/$name.got")"
    fi
done

echo
printf "${green}%d passed${off}, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
