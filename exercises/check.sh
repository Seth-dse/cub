#!/bin/sh
# Check your exercises against the expected output.
#
#   ./check.sh          every exercise
#   ./check.sh 3        just exercise 3
#   ./check.sh --solutions   confirm the worked answers still pass
set -u
cd "$(dirname "$0")"

CUBC=${CUBC:-cubc}
command -v "$CUBC" >/dev/null 2>&1 || CUBC=../cubc
if ! command -v "$CUBC" >/dev/null 2>&1 && [ ! -x "$CUBC" ]; then
    echo "cannot find cubc; build it with 'make' in the project root"
    exit 1
fi

dir="."
label="your answer"
if [ "${1:-}" = "--solutions" ]; then
    dir="solutions"
    label="worked answer"
    shift
fi

only="${1:-}"
red=""; green=""; dim=""; off=""
if [ -t 1 ]; then red="\033[31m"; green="\033[32m"; dim="\033[2m"; off="\033[0m"; fi

pass=0; todo=0; fail=0
mkdir -p .out

for src in "$dir"/[0-9][0-9]-*.cb; do
    [ -e "$src" ] || continue
    name=$(basename "$src" .cb)
    number=$(echo "$name" | cut -d- -f1)

    if [ -n "$only" ]; then
        want=$(printf "%02d" "$only" 2>/dev/null || echo "$only")
        [ "$number" = "$want" ] || continue
    fi

    exp="expected/$name.txt"
    [ -f "$exp" ] || continue

    if ! "$CUBC" run "$src" > ".out/$name.txt" 2>&1; then
        printf "${red}error${off}  %s  (it does not compile)\n" "$name"
        sed 's/^/       /' ".out/$name.txt" | head -4
        fail=$((fail + 1))
        continue
    fi

    if diff -q "$exp" ".out/$name.txt" >/dev/null 2>&1; then
        printf "${green}pass${off}   %s\n" "$name"
        pass=$((pass + 1))
    elif grep -q "TODO" "$src" 2>/dev/null; then
        printf "${dim}todo${off}   %s\n" "$name"
        todo=$((todo + 1))
    else
        printf "${red}wrong${off}  %s\n" "$name"
        printf "${dim}"
        diff "$exp" ".out/$name.txt" | sed 's/^/       /' | head -12
        printf "${off}"
        fail=$((fail + 1))
    fi
done

echo
printf "%d passed" "$pass"
[ "$todo" -gt 0 ] && printf ", %d not started" "$todo"
[ "$fail" -gt 0 ] && printf ", ${red}%d to fix${off}" "$fail"
echo
[ "$fail" -eq 0 ]
