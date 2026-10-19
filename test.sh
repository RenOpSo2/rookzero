#!/bin/sh
# RookZero test suite: perft correctness, make/unmake round-trips, and
# randomized move-generation validation against python-chess.
set -eu

cd "$(dirname "$0")"
./build.sh >/dev/null

PY=python3
if ! $PY -c 'import chess' >/dev/null 2>&1; then
    for cand in python3 /usr/bin/python3 /data/data/com.termux/files/usr/bin/python3; do
        if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'import chess' >/dev/null 2>&1; then
            PY=$cand
            break
        fi
    done
fi
if ! $PY -c 'import chess' >/dev/null 2>&1; then
    echo "SKIP: python-chess not available; reference tests skipped." >&2
    PY=
fi

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

echo "== perft (start position) =="
out=$(printf 'position startpos\nperft 1\nperft 2\nperft 3\nperft 4\nquit\n' | ./rookzero)
printf '%s\n' "$out" | sed 's/^/  /'
printf '%s\n' "$out" | grep -q 'perft 1: 20$'      || fail 'startpos perft 1'
printf '%s\n' "$out" | grep -q 'perft 2: 400$'     || fail 'startpos perft 2'
printf '%s\n' "$out" | grep -q 'perft 3: 8902$'    || fail 'startpos perft 3'
printf '%s\n' "$out" | grep -q 'perft 4: 197281$'  || fail 'startpos perft 4'
echo "  ok"

echo "== perft (standard test positions) =="
# fen <expected-depth>:<expected-nodes> ...
check_perft() {
    fen=$1
    shift
    for spec in "$@"; do
        d=${spec%%:*}
        want=${spec#*:}
        got=$(printf 'position fen %s\nperft %s\nquit\n' "$fen" "$d" | ./rookzero |
              sed -n 's/^perft [0-9]*: //p')
        printf '  depth %-2s %-10s %s\n' "$d" "$got" \
            "$( [ "$got" = "$want" ] && echo ok || echo "FAIL (want $want)")"
        [ "$got" = "$want" ] || fail "perft $d for $fen"
    done
}
# Kiwipete
check_perft 'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1' 1:48 2:2039 3:97862
# En passant / pins
check_perft '8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1' 1:14 2:191 3:2812 4:43238 5:674624
# Promotions
check_perft 'r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1' 1:6 2:264 3:9467
check_perft 'rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8' 1:44 2:1486 3:62379
# Castling rights
check_perft 'r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10' 1:46 2:2079 3:89890
echo "  ok"

echo "== make/unmake round-trips =="
cc -O2 -std=c11 -Wall -pthread -DROOKZERO_NO_MAIN test_roundtrip.c -o test_roundtrip \
    2>/dev/null || fail 'could not build test_roundtrip'
./test_roundtrip | sed 's/^/  /' || fail 'make/unmake round-trip'
rm -f test_roundtrip

if [ -n "$PY" ]; then
    echo "== randomized reference validation =="
    $PY test_reference.py 40 80 1 | sed 's/^/  /' || fail 'reference validation (seed 1)'
    $PY test_reference.py 25 60 7 | sed 's/^/  /' || fail 'reference validation (seed 7)'
else
    echo "== randomized reference validation: skipped =="
fi

echo
echo "All tests passed."