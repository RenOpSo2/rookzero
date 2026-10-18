# RookZero 1.1.1

RookZero is an experimental chess engine written in C. It uses a classical search/evaluation approach (no NNUE) and speaks the UCI protocol, so it can be loaded by UCI-compatible chess GUIs.

## Features

- UCI protocol: `uci`, `isready`, `ucinewgame`, `position`, `go`, `stop`, `quit`
- `go depth N`, `go movetime N`, and clock-based `go wtime ... btime ... winc ... binc ...`
- Asynchronous search so the GUI can send `stop`
- Legal move generation including castling, en passant, and promotion
- Negamax + alpha-beta, iterative deepening, quiescence search
- Transposition table, Zobrist hashing, killer moves, history heuristic
- Basic handcrafted material and piece-square evaluation
- `perft N` debugging command and `d` board display command (debug commands are optional extensions, not UCI standard)

The search worker uses an explicit 4 MiB pthread stack to avoid exhausting Alpine/musl’s smaller default thread stack during recursive quiescence search. The POSIX feature macro is defined in the source for portable `clock_gettime` declarations.

This is an early prototype, not a tournament-tested engine. No Elo claim is made. Draw/repetition handling and extensive edge-case validation remain future work.

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

The initial position should produce these standard perft counts:

| Depth | Nodes |
|---:|---:|
| 1 | 20 |
| 2 | 400 |
| 3 | 8,902 |
| 4 | 197,281 |

Run the engine, then enter `perft 4` after `position startpos` to check the move generatI**e