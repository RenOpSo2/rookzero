#!/usr/bin/env python3
"""
Self-play test for RookZero chess engine.
Plays a game against itself and outputs the PGN.
"""

import subprocess
import sys
import re
from typing import List, Tuple, Optional

class UCIClient:
    """Simple UCI client for communicating with the chess engine."""
    
    def __init__(self, engine_path: str):
        self.process = subprocess.Popen(
            [engine_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1
        )
        self._send("uci")
        self._wait_for("uciok")
        
    def _send(self, command: str):
        """Send a command to the engine."""
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        
    def _read_line(self) -> str:
        """Read a line from the engine."""
        line = self.process.stdout.readline()
        return line.strip()
        
    def _wait_for(self, expected: str, timeout: int = 30) -> str:
        """Wait for a specific response from the engine."""
        while True:
            line = self._read_line()
            if expected in line:
                return line
            if not line:
                raise TimeoutError(f"Timeout waiting for '{expected}'")
                
    def new_game(self):
        """Start a new game."""
        self._send("ucinewgame")
        
    def set_position(self, moves: List[str]):
        """Set the position with a list of moves."""
        cmd = "position startpos"
        if moves:
            cmd += " moves " + " ".join(moves)
        self._send(cmd)
        
    def go(self, depth: int = 8) -> str:
        """Start searching and return the best move."""
        self._send(f"go depth {depth}")
        while True:
            line = self._read_line()
            if line.startswith("bestmove"):
                move = line.split()[1]
                return move
            if not line:
                raise TimeoutError("Timeout waiting for bestmove")
                
    def quit(self):
        """Close the engine."""
        self._send("quit")
        self.process.wait()


def coordinate_to_algebraic(move: str, moves: List[str]) -> str:
    """
    Convert UCI coordinate notation (e.g., "e2e4") to PGN algebraic notation (e.g., "e4").
    This is a simplified version that handles basic moves.
    """
    if not move or move == "(none)":
        return ""
    
    from_sq = move[:2]
    to_sq = move[2:4]
    promotion = move[4:] if len(move) > 4 else ""
    
    # Simplified conversion - just return the destination for pawn moves
    # This is not full PGN standard but sufficient for basic display
    return to_sq + promotion


def play_game(engine_path: str, max_moves: int = 200, depth: int = 8) -> Tuple[List[str], str]:
    """
    Play a game between two instances of the engine.
    Returns (moves, result)
    """
    print(f"Starting self-play game (depth={depth}, max_moves={max_moves})...")
    
    white = UCIClient(engine_path)
    black = UCIClient(engine_path)
    
    white.new_game()
    black.new_game()
    
    moves: List[str] = []
    current_white = white
    current_black = black
    
    for move_num in range(1, max_moves + 1):
        # White's move
        current_white.set_position(moves)
        white_move = current_white.go(depth)
        
        if white_move == "(none)" or white_move == "0000":
            result = "1/2-1/2"  # Stalemate or no legal moves
            print(f"Game ended: {result}")
            break
            
        moves.append(white_move)
        algebraic = coordinate_to_algebraic(white_move, moves)
        print(f"{move_num}. {algebraic}", end=" ")
        
        # Check for game end (very basic check)
        if len(moves) >= max_moves:
            result = "1/2-1/2"  # Draw by move limit
            print(f"\nGame ended: {result}")
            break
        
        # Black's move
        current_black.set_position(moves)
        black_move = current_black.go(depth)
        
        if black_move == "(none)" or black_move == "0000":
            result = "1-0"  # Black has no legal moves
            print(f"{algebraic}\nGame ended: {result}")
            break
            
        moves.append(black_move)
        black_algebraic = coordinate_to_algebraic(black_move, moves)
        print(f"{black_algebraic}")
        
        # Simple game end detection (mate in 3 moves or less remaining)
        if "0000" in [white_move, black_move]:
            result = "1/2-1/2"
            print(f"Game ended: {result}")
            break
    
    if len(moves) >= max_moves:
        result = "1/2-1/2"
    elif len(moves) % 2 == 1:
        result = "1-0"  # White moved last, black couldn't move
    else:
        result = "1/2-1/2"  # Default to draw
    
    white.quit()
    black.quit()
    
    return moves, result


def format_pgn(moves: List[str], result: str) -> str:
    """Format the game as PGN."""
    pgn = ['[Event "RookZero Self-Play"]']
    pgn.append('[Site "Local"]')
    pgn.append('[Date "2026.10.18"]')
    pgn.append('[White "RookZero"]')
    pgn.append('[Black "RookZero"]')
    pgn.append('[Result "' + result + '"]')
    pgn.append('')
    
    # Format moves in PGN style
    move_text = []
    for i in range(0, len(moves), 2):
        move_num = i // 2 + 1
        white_algebraic = coordinate_to_algebraic(moves[i], moves[:i+1])
        if i + 1 < len(moves):
            black_algebraic = coordinate_to_algebraic(moves[i+1], moves[:i+2])
            move_text.append(f"{move_num}. {white_algebraic} {black_algebraic}")
        else:
            move_text.append(f"{move_num}. {white_algebraic}")
    
    pgn.append(" ".join(move_text))
    pgn.append(result)
    
    return "\n".join(pgn)


def main():
    engine_path = "./rookzero"
    
    # Check if engine exists
    import os
    if not os.path.exists(engine_path):
        print(f"Error: Engine '{engine_path}' not found.")
        print("Please build the engine first with: ./build.sh")
        sys.exit(1)
    
    # Play the game
    depth = 4  # Search depth
    max_moves = 30  # Maximum moves per side
    
    moves, result = play_game(engine_path, max_moves=max_moves, depth=depth)
    
    # Display PGN
    print("\n" + "="*60)
    print("PGN OUTPUT")
    print("="*60)
    pgn = format_pgn(moves, result)
    print(pgn)
    print("="*60)
    
    # Also save to file
    with open("self_play.pgn", "w") as f:
        f.write(pgn)
    print(f"\nPGN saved to: self_play.pgn")


if __name__ == "__main__":
    main()
