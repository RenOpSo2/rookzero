#!/bin/sh
set -eu
./build.sh >/dev/null
out=$(printf 'position startpos\nperft 1\nperft 2\nperft 3\nperft 4\nquit\n' | ./rookzero)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q 'perft 1: 20'
printf '%s\n' "$out" | grep -q 'perft 2: 400'
printf '%s\n' "$out" | grep -q 'perft 3: 8902'
printf '%s\n' "$out" | grep -q 'perft 4: 197281'
echo 'Perft start-position tests passed.'
