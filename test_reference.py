#!/usr/bin/env python3
"""
Randomized move-generation validation for RookZero.

python-chess is the arbiter. Random legal games are played, and at every
position in every game the engine's legal move list is compared against the
reference.

The engine must never offer a move python-chess rejects (an illegal move), and
must never omit one python-chess allows (a missed move, which silently
undermines search quality).

Usage: test_reference.py [games] [plies] [seed]
"""

import random
import subprocess
import sys

try:
    import chess
except ImportError:
    sys.exit("python-chess is required: pip install python-chess")


class Engine:
    """Drives the engine over stdin/stdout."""

    def __init__(self, path="./rookzero"):
        self.proc = subprocess.Popen(
            [path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def read_line(self):
        line = self.proc.stdout.readline()
        if line == "":
            raise RuntimeError("engine closed stdout unexpectedly")
        return line.strip()

    def legal_moves(self, fen):
        """Return the engine's legal move list for a FEN."""
        self.send("position fen " + fen)
        self.send("lm")
        moves = []
        while True:
            line = self.read_line()
            if line == "end":
                return moves
            moves.append(line)

    def perft(self, depth):
        self.send("perft %d" % depth)
        line = self.read_line()
        if not line.startswith("perft"):
            raise RuntimeError("unexpected: %r" % line)
        return int(line.split(":")[1])

    def bestmove(self, fen, depth=4):
        self.send("position fen " + fen)
        self.send("go depth %d" % depth)
        while True:
            line = self.read_line()
            if line.startswith("bestmove"):
                return line.split()[1]

    def close(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def main():
    games = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    plies = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 20261019

    rng = random.Random(seed)
    engine = Engine()
    checked = 0
    failures = 0

    try:
        for g in range(games):
            board = chess.Board()
            for ply in range(plies):
                ref = {m.uci() for m in board.legal_moves}
                got = set(engine.legal_moves(board.fen()))
                checked += 1

                extra = got - ref
                missing = ref - got
                if extra or missing:
                    failures += 1
                    print("MISMATCH game %d ply %d" % (g, ply))
                    print("  fen: %s" % board.fen())
                    if extra:
                        print("  ILLEGAL moves offered: %s" % sorted(extra))
                    if missing:
                        print("  legal moves omitted:  %s" % sorted(missing))
                    if failures >= 10:
                        sys.exit(1)

                if not ref:
                    break
                board.push_uci(rng.choice(sorted(ref)))

        # Every move the engine actually plays must be legal too.
        print("checking engine bestmoves are legal...")
        for g in range(min(games, 15)):
            board = chess.Board()
            for ply in range(30):
                mv = engine.bestmove(board.fen())
                if mv == "0000":
                    break
                if mv not in {m.uci() for m in board.legal_moves}:
                    failures += 1
                    print("ILLEGAL BESTMOVE game %d ply %d: %s" % (g, ply, mv))
                    print("  fen: %s" % board.fen())
                    sys.exit(1)
                board.push_uci(mv)
    finally:
        engine.close()

    print("positions compared: %d" % checked)
    if failures:
        print("FAILURES: %d" % failures)
        sys.exit(1)
    print("move generation matches the reference on every position.")


if __name__ == "__main__":
    main()