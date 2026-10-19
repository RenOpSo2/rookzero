/*
 * RookZero move-generation regression tests.
 *
 * Covers the invariants that the 1.2.0 fixes restored:
 *   1. make_move()/unmake_move() must be exact inverses, including for
 *      en passant, promotions and castling.
 *   2. A rejected en passant capture must not corrupt the position. In 1.1.1
 *      the rollback wrote the captured pawn onto the (empty) en passant
 *      target square, which then made every later move in the same
 *      generate_legal() call look illegal.
 *   3. square_attacked() must see an enemy king on an adjacent square.
 *      In 1.1.1 it compared a piece *type* against the black king's full
 *      piece code (12), so adjacent-king attacks were invisible and the
 *      engine offered illegal king moves.
 *   4. generate_legal() must not mutate the position it is given.
 *
 * Built by test.sh via -DROOKZERO_NO_MAIN so the UCI main loop is excluded.
 */
#include "rookzero.c"
#include <stdarg.h>

static int failures = 0;

static void fail(const char *fmt, ...) {
    va_list ap;
    fputs("  FAIL: ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    failures++;
}

/* Board/clock/rights equality, excluding the derived zobrist key. */
static int state_equal(const Position *a, const Position *b) {
    if (a->side != b->side || a->castle != b->castle || a->ep != b->ep ||
        a->halfmove != b->halfmove || a->fullmove != b->fullmove)
        return 0;
    for (int s = 0; s < 64; s++)
        if (a->board[s] != b->board[s]) return 0;
    return 1;
}

static int load(Position *p, const char *fen) {
    if (!parse_fen(p, fen)) {
        fail("could not parse FEN: %s", fen);
        return 0;
    }
    return 1;
}

static int count_legal(const Position *p) {
    MoveList l;
    generate_legal(p, &l);
    return l.count;
}

/* Does generate_legal() offer this move? */
static int offers(const Position *p, const char *uci) {
    MoveList l;
    generate_legal(p, &l);
    for (int i = 0; i < l.count; i++) {
        char b[8];
        move_to_uci(l.moves[i], b);
        if (!strcmp(b, uci)) return 1;
    }
    return 0;
}

/* ---- targeted regressions -------------------------------------------- */

static void test_adjacent_king(void) {
    /*
     * White king on h2, black king on h4. The king on h4 attacks g3, h3 and
     * g4, so h2g3 and h2h3 are illegal. 1.1.1 offered both as legal king
     * moves because it never detected an adjacent enemy king.
     */
    static const char *fen = "8/8/8/8/7k/8/7K/8 w - - 0 1";
    Position p;
    if (!load(&p, fen)) return;

    int n = count_legal(&p);
    if (n != 3) fail("adjacent king: expected 3 legal moves, got %d", n);
    if (offers(&p, "h2g3")) fail("adjacent king: h2g3 is illegal but offered");
    if (offers(&p, "h2h3")) fail("adjacent king: h2h3 is illegal but offered");

    /* The attack queries themselves. Black king is on h4 (31). */
    if (!square_attacked(&p, 22, BLACK))   /* g3 */
        fail("square_attacked(g3, black) should be true");
    if (!square_attacked(&p, 23, BLACK))   /* h3 */
        fail("square_attacked(h3, black) should be true");
    if (!square_attacked(&p, 30, BLACK))   /* g4 */
        fail("square_attacked(g4, black) should be true");
    if (square_attacked(&p, 14, BLACK))    /* g2 */
        fail("square_attacked(g2, black) should be false");
    if (square_attacked(&p, 15, BLACK))    /* h2 */
        fail("square_attacked(h2, black) should be false");

    /* Black king adjacency from the other side. */
    Position q;
    if (load(&q, "8/8/8/8/7K/8/7k/8 b - - 0 1"))
        if (count_legal(&q) != 3)
            fail("adjacent king (black): expected 3 legal moves, got %d",
                 count_legal(&q));
}

static void test_rejected_ep_keeps_board_clean(void) {
    /*
     * f4xg3 e.p. is illegal here: the white pawn on g4 disappears and the
     * capturing black pawn lands on g3, where it would give check to nothing
     * but it does expose the black king on h4 to the pawn on g3. The engine
     * must reject it. Critically, rejecting it must leave the position
     * untouched so the other 12 black moves stay available.
     */
    static const char *fen = "8/2p5/3p4/KP5r/1R3pPk/8/4P3/8 b - g3 0 1";
    Position p;
    if (!load(&p, fen)) return;

    if (offers(&p, "f4g3"))
        fail("rejected en passant: f4g3 should be illegal here");

    int n = count_legal(&p);
    if (n != 17)
        fail("rejected en passant: expected 17 legal moves after the "
             "rejected capture, got %d (board corrupted?)", n);

    /* Every one of the following must survive the rejected capture. */
    static const char *must_have[] = {
        "h5b5", "h5c5", "h5d5", "h5e5", "h5f5", "h5g5",
        "h5h6", "h5h7", "h5h8", "d6d5", "c7c6", "c7c5"
    };
    for (size_t i = 0; i < sizeof(must_have) / sizeof(*must_have); i++)
        if (!offers(&p, must_have[i]))
            fail("rejected en passant: %s went missing", must_have[i]);
}

static void test_ep_is_generated_when_legal(void) {
    /* The other direction: a legal en passant capture must be offered. */
    static const char *fen = "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1";
    Position p;
    if (!load(&p, fen)) return;
    if (!offers(&p, "e5d6"))
        fail("legal en passant capture e5d6 was not offered");
}

/* ---- make/unmake round-trips ----------------------------------------- */

static void test_roundtrip_position(Position *p, const char *label) {
    Position snapshot = *p;

    MoveList l;
    generate_legal(p, &l);
    if (!state_equal(p, &snapshot)) {
        fail("%s: generate_legal mutated the position", label);
        return;
    }

    for (int i = 0; i < l.count; i++) {
        Position before = *p;
        Undo u;
        char b[8];
        move_to_uci(l.moves[i], b);
        if (!make_move(p, l.moves[i], &u)) {
            fail("%s: generate_legal returned illegal move %s", label, b);
            continue;
        }
        unmake_move(p, &u);
        if (!state_equal(p, &before))
            fail("%s: make/unmake not inverse for %s", label, b);
        if (p->key != before.key)
            fail("%s: zobrist key not restored for %s", label, b);
    }
}

/* Recursively round-trip every move down to `depth`. */
static int test_tree(Position *p, int depth) {
    int f = 0;
    MoveList l;
    generate_legal(p, &l);
    for (int i = 0; i < l.count; i++) {
        Position before = *p;
        Undo u;
        char b[8];
        move_to_uci(l.moves[i], b);
        if (!make_move(p, l.moves[i], &u)) {
            fail("tree: illegal move %s reported legal", b);
            f++;
            continue;
        }
        if (depth > 0) f += test_tree(p, depth - 1);
        unmake_move(p, &u);
        if (!state_equal(p, &before)) {
            fail("tree: state drift after unmake of %s", b);
            if (++f > 4) return f;
        }
    }
    return f;
}

static void test_roundtrips(void) {
    static const char *fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1",
        "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R b KQkq - 0 1",
        "r3k2r/1P6/8/8/8/8/6p1/R3K2R w KQkq - 0 1",
        "r3k2r/6P1/8/8/8/8/1p6/R3K2R b KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "8/8/8/8/k1pP3R/8/8/4K3 b - d3 0 1",
        "8/8/8/2k5/3Pp3/8/8/4K3 b - d3 0 1",
        "r3k2r/8/8/8/8/8/8/R3K2R w Kkq - 0 1",
        "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1",
        "k7/8/1Q6/8/8/8/8/7K b - - 0 1",
        "8/8/8/8/8/5K1k/8/7R w - - 0 1",
        NULL
    };
    for (int i = 0; fens[i]; i++) {
        Position p;
        if (!load(&p, fens[i])) continue;
        test_roundtrip_position(&p, "fixed FEN");
    }

    /* Recursive round-trips over the perft positions that used to break. */
    static const struct { const char *fen; int depth; } trees[] = {
        { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 3 },
        { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 2 },
        { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 2 },
        { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 2 },
        { NULL, 0 }
    };
    for (int i = 0; trees[i].fen; i++) {
        Position p;
        if (!load(&p, trees[i].fen)) continue;
        test_tree(&p, trees[i].depth);
    }
}

/* ---- randomized self-consistency ------------------------------------ */

static void test_random_games(void) {
    unsigned long long seed = 12345;
    int games = 40, plies = 60;
    for (int g = 0; g < games; g++) {
        Position p;
        set_startpos(&p);
        for (int ply = 0; ply < plies; ply++) {
            Position before = p;
            MoveList l;
            generate_legal(&p, &l);
            for (int s = 0; s < 64; s++)
                if (p.board[s] != before.board[s]) {
                    fail("random: generate_legal mutated the board");
                    return;
                }
            if (l.count == 0) break;
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            int idx = (int)((seed >> 33) % (unsigned long long)l.count);
            Undo u;
            if (!make_move(&p, l.moves[idx], &u)) {
                fail("random: legal move rejected");
                return;
            }
            unmake_move(&p, &u);
            if (!state_equal(&p, &before) || p.key != before.key) {
                fail("random: make/unmake drift (game %d ply %d)", g, ply);
                return;
            }
            if (!make_move(&p, l.moves[idx], &u)) continue;
        }
    }
}

int main(void) {
    init_zobrist();

    test_adjacent_king();
    test_rejected_ep_keeps_board_clean();
    test_ep_is_generated_when_legal();
    test_roundtrips();
    test_random_games();

    if (failures) {
        printf("%d failure(s).\n", failures);
        return 1;
    }
    printf("all move-generation regression tests passed.\n");
    return 0;
}