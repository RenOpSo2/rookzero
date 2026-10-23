/*
 * RookZero - a small from-scratch chess engine prototype in C.
 * Header file for public API and shared structures.
 */

#ifndef ROOKZERO_H
#define ROOKZERO_H

#include <stdint.h>

/* Version string */
#define VERSION "1.2.0"

/* Constants */
#define MAX_MOVES 256
#define INF 32000
#define MATE 30000
#define MAX_PLY 128
#define MAX_PERFT_DEPTH 8

/* Piece enums */
enum {
    EMPTY = 0,
    WP = 1, WN, WB, WR, WQ, WK,
    BP, BN, BB, BR, BQ, BK
};

/* Color enums */
enum {
    WHITE = 0,
    BLACK = 1
};

/* Castling rights */
enum {
    WKCA = 1,
    WQCA = 2,
    BKCA = 4,
    BQCA = 8
};

/* Move flags */
enum {
    MF_CAPTURE = 1,
    MF_EP = 2,
    MF_CASTLE = 4,
    MF_PROMO = 8,
    MF_DOUBLE = 16
};

/* Structures */
typedef struct {
    int from, to, promo, flags;
} Move;

typedef struct {
    int board[64];
    int side, castle, ep, halfmove, fullmove;
    uint64_t key;
} Position;

typedef struct {
    Move move;
    int captured, castle, ep, halfmove, fullmove, side;
    uint64_t key;
} Undo;

typedef struct {
    Move moves[MAX_MOVES];
    int count;
} MoveList;

/* Public API functions */

/* Initialize Zobrist hashing tables */
void init_zobrist(void);

/* Clear position to initial empty state */
void clear_position(Position *p);

/* Set position to standard starting position */
void set_startpos(Position *p);

/* Parse FEN string and load into position */
int parse_fen(Position *p, const char *fen);

/* Generate all legal moves for a position */
int generate_legal(const Position *p, MoveList *out);

/* Make a move on the position, return 1 if legal, 0 if illegal */
int make_move(Position *p, Move m, Undo *u);

/* Unmake a move using the undo structure */
void unmake_move(Position *p, Undo *u);

/* Convert move to UCI string format (e.g., "e2e4", "e7e8q") */
void move_to_uci(Move m, char *buf);

/* Check if a square is attacked by a given side */
int square_attacked(const Position *p, int sq, int by_side);

/* Utility functions */
int file_of(int sq);
int rank_of(int sq);
int on_board(int sq);
int color_of(int p);
int is_piece_color(int p, int side);
int piece_type(int p);
int make_piece(int type, int side);

#endif /* ROOKZERO_H */
