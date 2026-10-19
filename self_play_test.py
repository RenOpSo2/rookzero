#!/usr/bin/env python3
"""
Self-play test mode for RookZero.

Plays the engine against itself, then prints the game as valid PGN so it can
be reviewed by eye. python-chess is used as the referee: it validates that
every move the engine returns is legal, tracks the game state, and produces
standard algebraic notation.

Two engines play each other, each with its own process, so they do not share
a transposition table or search state.

Examples:
    python3 self_play_test.py                       # one quick game
    python3 self_play_test.py --games 5 --depth 6
    python3 self_play_test.py --movetime 200        # fixed time per move
    python3 self_play_test.py --pgn games.pgn       # also write to a file

Requires python-chess. Set ROOKZERO to point at a different binary.
"""

import argparse
import datetime
import os
import subprocess
import sys
from typing import List, Optional

try:
    import chess
    import chess.pgn
except ImportError:
    sys.exit("python-chess is required: pip install python-chess")


class UCIError(RuntimeError):
    pass


class Engine:
    """Minimal UCI client that validates every move against a reference board."""

    def __init__(self, path: str, name: str):
        self.name = name
        self.proc = subprocess.Popen(
            [path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
        self._send("uci")
        line = self._read_until("uciok")
        self.id_name = self._parse_id(line)

    @staticmethod
    def _parse_id(handshake: str) -> str:
        # The id line is the line before uciok.
        for line in reversed(handshake.splitlines()):
            if line.startswith("id name"):
                return line[len("id name"):].strip()
        return "RookZero"

    def _send(self, command: str) -> None:
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()

    def _read_until(self, token: str, limit: int = 200) -> str:
        seen = []
        for _ in range(limit):
            line = self.proc.stdout.readline()
            if line == "":
                raise UCIError("%s closed stdout" % self.name)
            line = line.rstrip("\r\n")
            seen.append(line)
            if token in line:
                return "\n".join(seen)
        raise UCIError("%s never sent %r" % (self.name, token))

    def new_game(self) -> None:
        self._send("ucinewgame")
        self._send("isready")
        self._read_until("readyok")

    def bestmove(self, board: chess.Board, depth: Optional[int],
                 movetime: Optional[int]) -> chess.Move:
        """Ask for a move in the given position and return it.

        Raises UCIError if the engine returns something that is not legal in
        the reference position.
        """
        # Set the position explicitly; each engine only ever knows the position
        # it was last given.
        self._send("position fen " + board.fen())
        if depth:
            self._send("go depth %d" % depth)
        elif movetime:
            self._send("go movetime %d" % movetime)
        else:
            self._send("go depth 4")

        while True:
            line = self.proc.stdout.readline()
            if line == "":
                raise UCIError("%s closed stdout during search" % self.name)
            line = line.strip()
            if not line.startswith("bestmove"):
                continue
            uci = line.split()[1] if len(line.split()) > 1 else "0000"
            if uci in ("0000", "(none)", "null"):
                raise UCIError("%s returned no move" % self.name)
            try:
                move = chess.Move.from_uci(uci)
            except ValueError:
                raise UCIError("%s returned unparseable move %r"
                                % (self.name, uci))
            if move not in board.legal_moves:
                raise UCIError(
                    "%s played ILLEGAL move %s in position %s"
                    % (self.name, uci, board.fen())
                )
            return move

    def close(self) -> None:
        try:
            self._send("quit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


TERMINATION_LABELS = {
    "checkmate": "checkmate",
    "stalemate": "stalemate",
    "insufficient_material": "insufficient material",
    "seventyfive_moves": "seventy-move rule",
    "fivefold_repetition": "fivefold repetition",
    "max_plies": "move limit",
}


def adjudicate(board: chess.Board, plies: int, max_plies: int) -> Optional[str]:
    """Return the game result if the game is over, else None."""
    if board.is_checkmate():
        return "0-1" if board.turn == chess.WHITE else "1-0"
    if board.is_stalemate():
        return "1/2-1/2"
    if board.is_insufficient_material():
        return "1/2-1/2"
    if board.is_seventyfive_moves():
        return "1/2-1/2"
    if board.is_fivefold_repetition():
        return "1/2-1/2"
    if plies >= max_plies:
        return "1/2-1/2"
    return None


def describe_termination(board: chess.Board, plies: int, max_plies: int) -> str:
    if board.is_checkmate():
        return "checkmate"
    if board.is_stalemate():
        return "stalemate"
    if board.is_insufficient_material():
        return "insufficient material"
    if board.is_seventyfive_moves():
        return "seventy-move rule"
    if board.is_fivefold_repetition():
        return "fivefold repetition"
    if plies >= max_plies:
        return "move limit"
    return "unfinished"


def play_game(engine_path: str, depth: Optional[int], movetime: Optional[int],
              max_plies: int) -> chess.pgn.Game:
    """Play one game between two engine instances and return the PGN game."""
    white = Engine(engine_path, "white")
    black = Engine(engine_path, "black")
    board = chess.Board()
    result = "*"
    termination = "unfinished"

    try:
        white.new_game()
        black.new_game()

        plies = 0
        while True:
            decided = adjudicate(board, plies, max_plies)
            if decided:
                result = decided
                termination = describe_termination(board, plies, max_plies)
                break

            side = white if board.turn == chess.WHITE else black
            move = side.bestmove(board, depth, movetime)
            board.push(move)
            plies += 1
    finally:
        white.close()
        black.close()

    # Build the PGN from the move stack, which replays every move into
    # standard algebraic notation.
    game = chess.pgn.Game.from_board(board)
    game.headers["Event"] = "RookZero self-play"
    game.headers["Site"] = "local"
    game.headers["Date"] = datetime.date.today().strftime("%Y.%m.%d")
    game.headers["White"] = white.id_name
    game.headers["Black"] = black.id_name
    game.headers["Result"] = result
    game.headers["Termination"] = TERMINATION_LABELS.get(termination,
                                                          termination)
    game.headers["Plies"] = str(board.ply())
    return game


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Play RookZero against itself and print the PGN."
    )
    parser.add_argument("--engine", default=os.environ.get("ROOKZERO", "./rookzero"),
                        help="path to the engine binary (default: ./rookzero)")
    parser.add_argument("--games", type=int, default=1,
                        help="number of self-play games (default: 1)")
    parser.add_argument("--depth", type=int, default=None,
                        help="fixed search depth per move")
    parser.add_argument("--movetime", type=int, default=None,
                        help="fixed milliseconds per move; overrides --depth")
    parser.add_argument("--max-plies", type=int, default=200,
                        help="stop a game after this many half-moves "
                             "(default: 200)")
    parser.add_argument("--pgn", metavar="FILE",
                        help="also write the games to FILE")
    parser.add_argument("--quiet", action="store_true",
                        help="only print the result line per game")
    args = parser.parse_args()

    if not os.path.exists(args.engine):
        sys.exit("engine not found at %s; run ./build.sh first" % args.engine)

    if args.movetime is None and args.depth is None:
        args.depth = 4

    limit = ""
    if args.movetime:
        limit = "%dms/move" % args.movetime
    elif args.depth:
        limit = "depth %d" % args.depth

    print("RookZero self-play: %d game(s), %s, limit %d plies"
          % (args.games, limit, args.max_plies))

    exported: List[str] = []
    for i in range(1, args.games + 1):
        try:
            game = play_game(args.engine, args.depth, args.movetime,
                             args.max_plies)
        except UCIError as exc:
            print("\nFAILED in game %d: %s" % (i, exc))
            return 1

        pgn_text = str(game)
        exported.append(pgn_text)

        if not args.quiet:
            print("\n" + "=" * 62)
            print("GAME %d" % i)
            print("=" * 62)
            print(pgn_text)
        print("game %d: %s (%s, %s plies)"
              % (i, game.headers["Result"], game.headers["Termination"],
                 game.headers["Plies"]))

    if args.pgn:
        with open(args.pgn, "w") as handle:
            handle.write("\n\n".join(exported) + "\n")
        print("\nwrote %s" % args.pgn)

    return 0


if __name__ == "__main__":
    sys.exit(main())