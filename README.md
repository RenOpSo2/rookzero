# RookZero 1.2.0

RookZero is an experimental chess engine written in C. It uses a classical search/evaluation approach (no NNUE) and speaks the UCI protocol, so it can be loaded by UCI-compatible chess GUIs.

## Features

- UCI protocol: `uci`, `isready`, `ucinewgame`, `position`, `go`, `stop`, `quit`
- `go depth N`, `go movetime N`, and clock-based `go wtime ... btime ... winc ... binc ...`
- Asynchronous search so the GUI can send `stop`
- Legal move generation including castling, en passant, and promotion
- Negamax + alpha-beta, iterative deepening, quiescence search
- Transposition table, Zobrist hashing, killer moves, history heuristic
- Basic handcrafted material and piece-square evaluation
- `perft N`, `d`, `lm` and `echo` debug commands (optional extensions, not UCI standard)

The search worker uses an explicit 4 MiB pthread stack to avoid exhausting Alpine/musl's smaller default thread stack during recursive quiescence search. The POSIX feature macro is defined in the source for portable `clock_gettime` declarations.

This is still a prototype, not a tournament-tested engine. No Elo claim is made. Draw and repetition handling remain future work.

## Changes in 1.2.0

This release is a correctness and stability pass. It fixes two move-generation
bugs that could cause the engine to play illegal moves, plus a few robustness issues.

**Fixed: rejected en passant captures corrupted the position.** When
`make_move` rejected an illegal en passant capture, its rollback wrote the
captured pawn onto the en passant *target* square, which is empty by
definition. The stale piece then made every subsequent move generated in the
same call look illegal. This silently dropped large numbers of legal moves
and corrupted perft counts.

**Fixed: an enemy king on an adjacent square was never seen as an attacker.**
`square_attacked` compared a piece *type* against the black king's full piece
code, so the comparison never matched. The engine would offer king moves that
stepped next to the opposing king. Minimal reproduction, where only three
moves are legal:

```text
position fen 8/8/8/8/7k/8/7K/8 w - - 0 1
perft 1          ; 1.1.1 printed 5, 1.2.0 prints 3
```

**Fixed: unbounded `strcat` when assembling a FEN.** A long `position fen`
line could overflow a fixed buffer. The FEN is now assembled with bounded
`snprintf`.

**Fixed: loose command matching.** `ucix` and `gogo` were treated as `uci`
and `go`, because commands were matched by raw prefix. They are now matched as
whole tokens.

**Added: `lm` lists legal moves** (one per line, terminated by `end`), which
makes scripted validation against a reference generator straightforward.

**Added: `echo`,** which makes line-synchronised UCI testing reliable.

**Added: tests.** See below.

Perft counts for the standard test positions now match published values; see
the Tests section.

## Build on Linux / Termux

You need a C compiler (Clang or GCC) and pthread support.

```sh
chmod +x build.sh
./build.sh
```

Or compile manually:

```sh
cc -O3 -std=c11 -Wall -Wextra -pthread rookzero.c -o rookzero
```

## Quick UCI test

```sh
./rookzero
```

Then enter:

```text
uci
isready
position startpos
go depth 8
```

For a timed search:

```text
position startpos
go movetime 1000
```

To stop a running search, send `stop`. Finish with `quit`.

## Loading in a GUI

1. Build the executable on the same operating system/CPU architecture as the GUI host.
2. In a GUI that supports external UCI engines, choose **Add/Install Engine** (wording differs by GUI) and select the `rookzero` executable.
3. Configure it as a **UCI** engine, then start an analysis or game.

A Linux/desktop executable cannot be copied directly into an Android GUI. On Android, the GUI must support loading external engines and be able to execute a compatible binary; app sandbox restrictions may prevent this. Termux builds are for Termux and are not automatically usable by every Android chess app.

## Tests

Run the whole suite with:

```sh
./test.sh
```

It covers three layers:

**1. Perft.** The start position, plus the standard test positions covering
castling, en passant, promotions, and castling rights:

```text
position startpos
perft 4
```

| Position | Depth | Nodes |
|---|---:|---:|
| startpos | 4 | 197,281 |
| startpos | 5 | 4,865,609 |
| Kiwipete | 3 | 97,862 |
| En passant / pins | 5 | 674,624 |
| Promotions | 3 | 62,379 |
| Castling rights | 3 | 89,890 |

**2. Move-generation regression tests** (`test_roundtrip.c`). These assert the
specific invariants behind the 1.2.0 fixes: adjacent-king detection, that a
rejected en passant capture leaves the position untouched, that legal en
passant captures are still generated, and that `make_move`/`unmake_move` are
exact inverses including the zobrist key. They also run randomized
round-trips. These fail against the 1.1.1 source with 19 failures.

**3. Randomized reference validation** (`test_reference.py`). Plays random
legal games with [python-chess](https://pypi.org/project/python-chess/) as the
arbiter, comparing the engine's legal move list at every position, and
separately checking that every move the engine actually plays is legal.
Requires `pip install python-chess`; these tests are skipped if it is absent.

To run the randomized tests directly:

```sh
python3 test_reference.py        # games, plies, seed
python3 test_reference.py 100 120 42
```

**Self-play** (`self_play_test.py`) plays the engine against itself and writes
a PGN. It is slow at high depth; the defaults are tuned for a quick check.

```sh
python3 self_play_test.py
```