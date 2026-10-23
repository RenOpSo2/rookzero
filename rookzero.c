#define _POSIX_C_SOURCE 199309L
/*
 * RookZero - a small from-scratch chess engine prototype in C.
 * Features: UCI, legal move generation (including castling, en passant,
 * promotions), negamax alpha-beta, iterative deepening, quiescence search,
 * basic material/positional evaluation, and perft.
 *
 * Prototype, not yet strength-tested or production-ready.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>

#include "rookzero.h"

static uint64_t zobrist_piece[13][64], zobrist_castle[16], zobrist_ep[8], zobrist_side;
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t tt_key[1<<20];
static Move tt_move[1<<20];
static int tt_depth[1<<20], tt_score[1<<20], tt_flag[1<<20];
static Move killer[MAX_PLY][2];
static int history_heur[2][64][64];
static long long nodes;
static atomic_int stop_search;
static atomic_int search_running;
static pthread_t search_thread;
static int search_thread_started;
static Position active_position;
static int requested_depth = 10;
static long long search_time_ms = 0;
static long long search_start_ms = 0;
static int search_infinite = 0;
static int piece_value[13] = {0,100,320,330,500,900,0,100,320,330,500,900,0};

static int score_to_tt(int score, int ply) {
    if (score > MATE - MAX_PLY) return score + ply;
    if (score < -MATE + MAX_PLY) return score - ply;
    return score;
}
static int score_from_tt(int score, int ply) {
    if (score > MATE - MAX_PLY) return score - ply;
    if (score < -MATE + MAX_PLY) return score + ply;
    return score;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static uint64_t rand64(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ULL;
}
int file_of(int sq) {
    return sq & 7;
}
int rank_of(int sq) {
    return sq >> 3;
}
int on_board(int sq) {
    return sq >= 0 && sq < 64;
}
int color_of(int p) {
    return p >= BP ? BLACK : WHITE;
}
int is_piece_color(int p, int side) {
    return p != EMPTY && color_of(p) == side;
}
int piece_type(int p) {
    return p > 6 ? p-6 : p;
}
int make_piece(int type, int side) {
    return type + (side == BLACK ? 6 : 0);
}

static uint64_t hash_position(const Position *p) {
    uint64_t h = 0;
    for (int sq=0; sq<64; sq++) if (p->board[sq]) h ^= zobrist_piece[p->board[sq]][sq];
    if (p->side == BLACK) h ^= zobrist_side;
    h ^= zobrist_castle[p->castle & 15];
    if (p->ep >= 0) h ^= zobrist_ep[file_of(p->ep)];
    return h;
}
void init_zobrist(void) {
    for (int p=0; p<13; p++) for (int s=0; s<64; s++) zobrist_piece[p][s]=rand64();
    for (int i=0; i<16; i++) zobrist_castle[i]=rand64();
    for (int i=0; i<8; i++) zobrist_ep[i]=rand64();
    zobrist_side=rand64();
}
void clear_position(Position *p) {
    memset(p,0,sizeof(*p));
    p->side=WHITE;
    p->ep=-1;
    p->castle=0;
    p->halfmove=0;
    p->fullmove=1;
}
void set_startpos(Position *p) {
    clear_position(p);
    /* Explicit setup avoids relying on enum conversion. */
    int white_back[8]= {WR,WN,WB,WQ,WK,WB,WN,WR};
    int black_back[8]= {BR,BN,BB,BQ,BK,BB,BN,BR};
    for(int f=0; f<8; f++) {
        p->board[f]=white_back[f];
        p->board[8+f]=WP;
        p->board[48+f]=BP;
        p->board[56+f]=black_back[f];
    }
    p->castle=WKCA|WQCA|BKCA|BQCA;
    p->key=hash_position(p);
}
int square_attacked(const Position *p, int sq, int by_side) {
    int f=file_of(sq), r=rank_of(sq);
    /* Pawn attacks */
    int pawn = by_side==WHITE ? WP : BP;
    int pr = r + (by_side==WHITE ? -1 : 1);
    if(pr>=0 && pr<8) {
        if(f>0 && p->board[pr*8+f-1]==pawn) return 1;
        if(f<7 && p->board[pr*8+f+1]==pawn) return 1;
    }
    static const int knight_delta[8][2]= {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
    int knight=by_side==WHITE?WN:BN;
    for(int i=0; i<8; i++) {
        int nf=f+knight_delta[i][0],nr=r+knight_delta[i][1];
        if(nf>=0&&nf<8&&nr>=0&&nr<8&&p->board[nr*8+nf]==knight)return 1;
    }
    static const int dirs[8][2]= {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    /*
     * Compare against the king piece TYPE. Comparing t (a type, 1..6) against
     * the full piece code (BK == 12) never matches, which silently made the
     * engine blind to an enemy king on an adjacent square.
     */
    const int KING_TYPE = piece_type(WK);
    for(int d=0; d<8; d++) {
        int nf=f+dirs[d][0],nr=r+dirs[d][1],dist=1;
        while(nf>=0&&nf<8&&nr>=0&&nr<8) {
            int pc=p->board[nr*8+nf];
            if(pc) {
                if(color_of(pc)==by_side) {
                    int t=piece_type(pc);
                    if(t==KING_TYPE&&dist==1)return 1;
                    if(d<4&&(t==WR||t==WQ||t==BR||t==BQ))return 1;
                    if(d>=4&&(t==WB||t==WQ||t==BB||t==BQ))return 1;
                }
                break;
            }
            nf+=dirs[d][0];
            nr+=dirs[d][1];
            dist++;
        }
    }
    return 0;
}
static int king_square(const Position *p, int side) {
    int k=side==WHITE?WK:BK;
    for(int s=0; s<64; s++)if(p->board[s]==k)return s;
    return -1;
}
static int in_check(const Position *p, int side) {
    int k=king_square(p,side);
    return k<0 || square_attacked(p,k,side^1);
}
static void add_move(MoveList *l,int from,int to,int promo,int flags) {
    if(l->count<MAX_MOVES)l->moves[l->count++]=(Move) {
        from,to,promo,flags
    };
}

static void generate_pseudo(const Position *p, MoveList *l) {
    l->count=0;
    int side=p->side;
    static const int nd[8][2]= {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
    static const int dirs[8][2]= {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    for(int from=0; from<64; from++) {
        int pc=p->board[from];
        if(!is_piece_color(pc,side))continue;
        int type=piece_type(pc), f=file_of(from),r=rank_of(from);
        if(type==WP) {
            int step=side==WHITE?8:-8, start=side==WHITE?1:6, promo_rank=side==WHITE?6:1;
            int one=from+step;
            if(on_board(one)&&p->board[one]==EMPTY) {
                if(r==promo_rank) {
                    int promos[4]= {WQ,WR,WB,WN};
                    for(int j=0; j<4; j++)add_move(l,from,one,make_piece(promos[j],side),MF_PROMO);
                }
                else {
                    add_move(l,from,one,0,0);
                    int two=from+2*step;
                    if(r==start&&p->board[two]==EMPTY)add_move(l,from,two,0,MF_DOUBLE);
                }
            }
            for(int df=-1; df<=1; df+=2) {
                int nf=f+df,nr=r+(side==WHITE?1:-1);
                if(nf<0||nf>7||nr<0||nr>7)continue;
                int to=nr*8+nf;
                if(p->board[to]&&color_of(p->board[to])!=side) {
                    if(r==promo_rank) {
                        int promos[4]= {WQ,WR,WB,WN};
                        for(int j=0; j<4; j++)add_move(l,from,to,make_piece(promos[j],side),MF_CAPTURE|MF_PROMO);
                    }
                    else add_move(l,from,to,0,MF_CAPTURE);
                }
                else if(to==p->ep)add_move(l,from,to,0,MF_CAPTURE|MF_EP);
            }
        } else if(type==WN) {
            for(int i=0; i<8; i++) {
                int nf=f+nd[i][0],nr=r+nd[i][1];
                if(nf<0||nf>7||nr<0||nr>7)continue;
                int to=nr*8+nf;
                if(!is_piece_color(p->board[to],side))add_move(l,from,to,0,p->board[to]?MF_CAPTURE:0);
            }
        } else if(type==WK) {
            for(int d=0; d<8; d++) {
                int nf=f+dirs[d][0],nr=r+dirs[d][1];
                if(nf<0||nf>7||nr<0||nr>7)continue;
                int to=nr*8+nf;
                if(!is_piece_color(p->board[to],side))add_move(l,from,to,0,p->board[to]?MF_CAPTURE:0);
            }
            if(side==WHITE&&from==4&&!in_check(p,WHITE)) {
                if((p->castle&WKCA)&&p->board[5]==EMPTY&&p->board[6]==EMPTY&&p->board[7]==WR&&!square_attacked(p,5,BLACK)&&!square_attacked(p,6,BLACK))add_move(l,4,6,0,MF_CASTLE);
                if((p->castle&WQCA)&&p->board[3]==EMPTY&&p->board[2]==EMPTY&&p->board[1]==EMPTY&&p->board[0]==WR&&!square_attacked(p,3,BLACK)&&!square_attacked(p,2,BLACK))add_move(l,4,2,0,MF_CASTLE);
            }
            if(side==BLACK&&from==60&&!in_check(p,BLACK)) {
                if((p->castle&BKCA)&&p->board[61]==EMPTY&&p->board[62]==EMPTY&&p->board[63]==BR&&!square_attacked(p,61,WHITE)&&!square_attacked(p,62,WHITE))add_move(l,60,62,0,MF_CASTLE);
                if((p->castle&BQCA)&&p->board[59]==EMPTY&&p->board[58]==EMPTY&&p->board[57]==EMPTY&&p->board[56]==BR&&!square_attacked(p,59,WHITE)&&!square_attacked(p,58,WHITE))add_move(l,60,58,0,MF_CASTLE);
            }
        } else {
            int first=type==WB?4:(type==WR?0:0), last=type==WB?8:(type==WR?4:8);
            for(int d=first; d<last; d++) {
                int nf=f+dirs[d][0],nr=r+dirs[d][1];
                while(nf>=0&&nf<8&&nr>=0&&nr<8) {
                    int to=nr*8+nf;
                    if(is_piece_color(p->board[to],side))break;
                    add_move(l,from,to,0,p->board[to]?MF_CAPTURE:0);
                    if(p->board[to])break;
                    nf+=dirs[d][0];
                    nr+=dirs[d][1];
                }
            }
        }
    }
}
int make_move(Position *p, Move m, Undo *u) {
    int pc=p->board[m.from], side=p->side;
    /*
     * dest holds whatever was on the destination square before the move. For a
     * normal move that is the captured piece; for an en passant capture it is
     * EMPTY, because the pawn actually taken sits on a different square. The
     * rollback path must restore dest, not the captured piece, otherwise a
     * rejected en passant leaves a phantom piece on the board.
     */
    int dest=p->board[m.to];
    int captured=dest;
    *u=(Undo) {
        m,captured,p->castle,p->ep,p->halfmove,p->fullmove,p->side,p->key
    };
    p->ep=-1;
    if(m.flags&MF_EP) {
        captured=p->board[m.to+(side==WHITE?-8:8)];
        u->captured=captured;
        p->board[m.to+(side==WHITE?-8:8)]=EMPTY;
    }
    p->board[m.from]=EMPTY;
    p->board[m.to]=(m.flags&MF_PROMO)?m.promo:pc;
    if(m.flags&MF_CASTLE) {
        if(m.to==6) {
            p->board[7]=EMPTY;
            p->board[5]=WR;
        }
        else if(m.to==2) {
            p->board[0]=EMPTY;
            p->board[3]=WR;
        }
        else if(m.to==62) {
            p->board[63]=EMPTY;
            p->board[61]=BR;
        }
        else if(m.to==58) {
            p->board[56]=EMPTY;
            p->board[59]=BR;
        }
    }
    if (pc == WK) p->castle &= ~(WKCA|WQCA);
    if (pc == BK) p->castle &= ~(BKCA|BQCA);
    if (m.from == 0 || m.to == 0) p->castle &= ~WQCA;
    if (m.from == 7 || m.to == 7) p->castle &= ~WKCA;
    if (m.from == 56 || m.to == 56) p->castle &= ~BQCA;
    if (m.from == 63 || m.to == 63) p->castle &= ~BKCA;
    if (m.flags & MF_DOUBLE) p->ep = m.from + (side == WHITE ? 8 : -8);
    if (piece_type(pc) == WP || captured != EMPTY) p->halfmove = 0;
    else p->halfmove++;
    if (side == BLACK) p->fullmove++;
    p->side ^= 1;
    p->key = hash_position(p);
    if(in_check(p,side)) { /* illegal: restore immediately */
        p->board[m.from]=pc;
        p->board[m.to]=dest;
        if(m.flags&MF_EP)p->board[m.to+(side==WHITE?-8:8)]=u->captured;
        if(m.flags&MF_CASTLE) {
            if(m.to==6) {
                p->board[7]=WR;
                p->board[5]=EMPTY;
            }
            else if(m.to==2) {
                p->board[0]=WR;
                p->board[3]=EMPTY;
            }
            else if(m.to==62) {
                p->board[63]=BR;
                p->board[61]=EMPTY;
            }
            else if(m.to==58) {
                p->board[56]=BR;
                p->board[59]=EMPTY;
            }
        }
        p->castle=u->castle;
        p->ep=u->ep;
        p->halfmove=u->halfmove;
        p->fullmove=u->fullmove;
        p->side=u->side;
        p->key=u->key;
        return 0;
    }
    return 1;
}
void unmake_move(Position *p, Undo *u) {
    Move m=u->move;
    int side=u->side, pc=p->board[m.to];
    p->side=side;
    p->castle=u->castle;
    p->ep=u->ep;
    p->halfmove=u->halfmove;
    p->fullmove=u->fullmove;
    p->key=u->key;
    if(m.flags&MF_CASTLE) {
        if(m.to==6) {
            p->board[7]=WR;
            p->board[5]=EMPTY;
        }
        else if(m.to==2) {
            p->board[0]=WR;
            p->board[3]=EMPTY;
        }
        else if(m.to==62) {
            p->board[63]=BR;
            p->board[61]=EMPTY;
        }
        else if(m.to==58) {
            p->board[56]=BR;
            p->board[59]=EMPTY;
        }
    }
    p->board[m.from]=(m.flags&MF_PROMO)?make_piece(WP,side):pc;
    p->board[m.to]=u->captured;
    if(m.flags&MF_EP) {
        p->board[m.to]=EMPTY;
        p->board[m.to+(side==WHITE?-8:8)]=u->captured;
    }
}
int generate_legal(const Position *p, MoveList *out) {
    MoveList pseudo;
    generate_pseudo(p,&pseudo);
    out->count=0;
    Position copy=*p;
    for(int i=0; i<pseudo.count; i++) {
        Undo u;
        if(make_move(&copy,pseudo.moves[i],&u)) {
            out->moves[out->count++]=pseudo.moves[i];
            unmake_move(&copy,&u);
        }
    }
    return out->count;
}
static int pst_bonus(int type,int sq,int side) {
    int r=rank_of(sq), f=file_of(sq);
    if(side==BLACK)r=7-r;
    int center=6-(abs(2*f-7)+abs(2*r-7))/2;
    switch(type) {
    case WP:
        return r*7+center*2;
    case WN:
        return center*8;
    case WB:
        return center*4;
    case WR:
        return r*2;
    case WQ:
        return center*2;
    case WK:
        return r<2?-center*2:center*3;
    default:
        return 0;
    }
}
static int evaluate(const Position *p) {
    int score=0;
    for(int sq=0; sq<64; sq++) {
        int pc=p->board[sq];
        if(!pc)continue;
        int side=color_of(pc),type=piece_type(pc);
        int v=piece_value[pc]+pst_bonus(type,sq,side);
        score+=(side==WHITE?v:-v);
    }
    return p->side==WHITE?score:-score;
}
static int move_score(const Position *p, Move m, Move tt, int ply) {
    if(m.from==tt.from&&m.to==tt.to&&m.promo==tt.promo)return 2000000;
    int victim=p->board[m.to];
    if(m.flags&MF_EP)victim=make_piece(WP,p->side^1);
    if(m.flags&MF_CAPTURE)return 1000000+piece_value[victim]*10-piece_value[p->board[m.from]];
    if(m.flags&MF_PROMO)return 900000+piece_value[m.promo];
    if(ply < MAX_PLY && m.from==killer[ply][0].from&&m.to==killer[ply][0].to)return 800000;
    if(ply < MAX_PLY && m.from==killer[ply][1].from&&m.to==killer[ply][1].to)return 700000;
    return history_heur[p->side][m.from][m.to];
}
static void sort_moves(const Position *p, MoveList *l, Move tt, int ply) {
    int scores[MAX_MOVES];
    for(int i=0; i<l->count; i++)scores[i]=move_score(p,l->moves[i],tt,ply);
    for(int i=0; i<l->count; i++) {
        int best=i;
        for(int j=i+1; j<l->count; j++)if(scores[j]>scores[best])best=j;
        if(best!=i) {
            int ts=scores[i];
            scores[i]=scores[best];
            scores[best]=ts;
            Move tm=l->moves[i];
            l->moves[i]=l->moves[best];
            l->moves[best]=tm;
        }
    }
}
static int quiescence(Position *p,int alpha,int beta,int ply) {
    nodes++;
    if ((nodes & 2047LL) == 0 && search_time_ms > 0 && !search_infinite && now_ms() - search_start_ms >= search_time_ms) atomic_store(&stop_search, 1);
    if (atomic_load(&stop_search)) return 0;
    if (ply >= MAX_PLY-1) return evaluate(p);
    int checked = in_check(p, p->side);
    int stand = -INF;
    if (!checked) {
        stand = evaluate(p);
        if (stand >= beta) return beta;
        if (stand > alpha) alpha = stand;
    }
    MoveList all, candidates;
    generate_legal(p, &all);
    if (all.count == 0) return checked ? -MATE + ply : 0;
    candidates.count = 0;
    for (int i=0; i<all.count; i++) {
        if (checked || (all.moves[i].flags & (MF_CAPTURE|MF_PROMO)))
            candidates.moves[candidates.count++] = all.moves[i];
    }
    sort_moves(p, &candidates, (Move) {
        -1,-1,0,0
    }, ply);
    for (int i=0; i<candidates.count; i++) {
        Undo u;
        if (!make_move(p, candidates.moves[i], &u)) continue;
        int score = -quiescence(p, -beta, -alpha, ply+1);
        unmake_move(p, &u);
        if (atomic_load(&stop_search)) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}
static int negamax(Position *p,int depth,int alpha,int beta,int ply,Move *best_out) {
    if (atomic_load(&stop_search)) return 0;
    if(ply>=MAX_PLY-1)return evaluate(p);
    nodes++;
    if ((nodes & 2047LL) == 0 && search_time_ms > 0 && !search_infinite && now_ms() - search_start_ms >= search_time_ms) atomic_store(&stop_search, 1);
    if (atomic_load(&stop_search)) return 0;
    int orig_alpha=alpha;
    uint64_t key=p->key;
    unsigned idx=(unsigned)(key&((1<<20)-1));
    Move tt= {-1,-1,0,0};
    if(tt_key[idx]==key) {
        tt=tt_move[idx];
        if(tt_depth[idx]>=depth&&ply>0) {
            int s=score_from_tt(tt_score[idx],ply);
            if(tt_flag[idx]==0)return s;
            if(tt_flag[idx]==1&&s<=alpha)return alpha;
            if(tt_flag[idx]==2&&s>=beta)return beta;
        }
    }
    if(depth<=0)return quiescence(p,alpha,beta,ply);
    MoveList list;
    generate_legal(p,&list);
    if(list.count==0)return in_check(p,p->side)?-MATE+ply:0;
    sort_moves(p,&list,tt,ply);
    int best=-INF;
    Move bestmove=list.moves[0];
    for(int i=0; i<list.count; i++) {
        Undo u;
        if(!make_move(p,list.moves[i],&u))continue;
        int score=-negamax(p,depth-1,-beta,-alpha,ply+1,NULL);
        unmake_move(p,&u);
        if (atomic_load(&stop_search)) return 0;
        if(score>best) {
            best=score;
            bestmove=list.moves[i];
        }
        if(score>alpha) {
            alpha=score;
            if(ply==0&&best_out)*best_out=list.moves[i];
        }
        if(alpha>=beta) {
            if(!(list.moves[i].flags&MF_CAPTURE)) {
                killer[ply][1]=killer[ply][0];
                killer[ply][0]=list.moves[i];
                history_heur[p->side][list.moves[i].from][list.moves[i].to]+=depth*depth;
            }
            break;
        }
    }
    tt_key[idx]=key;
    tt_move[idx]=bestmove;
    tt_depth[idx]=depth;
    tt_score[idx]=score_to_tt(best,ply);
    tt_flag[idx]=(best<=orig_alpha)?1:((best>=beta)?2:0);
    if(best_out)*best_out=bestmove;
    return best;
}
void move_to_uci(Move m,char *buf) {
    if(m.from<0||m.to<0) {
        strcpy(buf,"0000");
        return;
    }
    buf[0]='a'+file_of(m.from);
    buf[1]='1'+rank_of(m.from);
    buf[2]='a'+file_of(m.to);
    buf[3]='1'+rank_of(m.to);
    int n=4;
    if(m.flags&MF_PROMO) {
        int t=piece_type(m.promo);
        buf[n++]=t==WQ?'q':t==WR?'r':t==WB?'b':'n';
    }
    buf[n]='\0';
}
static int parse_square(const char *s) {
    if(!s||s[0]<'a'||s[0]>'h'||s[1]<'1'||s[1]>'8')return -1;
    return (s[1]-'1')*8+(s[0]-'a');
}
static int parse_uci_move(Position *p,const char *s,Move *out) {
    MoveList l;
    generate_legal(p,&l);
    for(int i=0; i<l.count; i++) {
        char b[8];
        move_to_uci(l.moves[i],b);
        if(!strcmp(b,s)) {
            *out=l.moves[i];
            return 1;
        }
    }
    return 0;
}
int parse_fen(Position *p, const char *fen) {
    clear_position(p);
    int rank = 7, file = 0;
    const char *s = fen;
    while (*s && *s != ' ') {
        if (*s == '/') {
            rank--;
            file = 0;
        }
        else if (isdigit((unsigned char)*s)) file += *s - '0';
        else {
            int side = isupper((unsigned char)*s) ? WHITE : BLACK;
            char c = (char)tolower((unsigned char)*s);
            int t = c == 'p' ? WP : c == 'n' ? WN : c == 'b' ? WB :
                    c == 'r' ? WR : c == 'q' ? WQ : c == 'k' ? WK : 0;
            if (!t || rank < 0 || rank > 7 || file < 0 || file > 7) return 0;
            p->board[rank * 8 + file++] = make_piece(t, side);
        }
        if (file > 8) return 0;
        s++;
    }
    if (*s != ' ') return 0;
    s++;
    p->side = *s == 'b' ? BLACK : WHITE;
    while (*s && *s != ' ') s++;
    if (*s != ' ') return 0;
    s++;
    p->castle = 0;
    if (*s == '-') s++;
    else {
        while (*s && *s != ' ') {
            if (*s == 'K') p->castle |= WKCA;
            if (*s == 'Q') p->castle |= WQCA;
            if (*s == 'k') p->castle |= BKCA;
            if (*s == 'q') p->castle |= BQCA;
            s++;
        }
    }
    while (*s == ' ') s++;
    p->ep = -1;
    if (*s != '-' && *s) {
        p->ep = parse_square(s);
        if (p->ep < 0) return 0;
        while (*s && *s != ' ') s++;
    } else if (*s == '-') s++;
    while (*s == ' ') s++;
    if (*s) p->halfmove = atoi(s);
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    if (*s) p->fullmove = atoi(s);
    p->key = hash_position(p);
    return 1;
}
static void print_board(const Position *p) {
    for(int r=7; r>=0; r--) {
        printf("%d ",r+1);
        for(int f=0; f<8; f++) {
            int pc=p->board[r*8+f];
            char c='.';
            if(pc) {
                const char *names=" PNBRQK";
                int t=piece_type(pc);
                c=names[t];
                if(color_of(pc)==BLACK)c=(char)tolower(c);
            }
            printf("%c ",c);
        }
        printf("\n");
    }
    printf("  a b c d e f g h\n");
}
static void do_position(Position *p,char *line) {
    char *tok=strtok(line," \t\r\n");
    if(!tok)return;
    tok=strtok(NULL," \t\r\n");
    if(!tok)return;
    if(!strcmp(tok,"startpos")) {
        set_startpos(p);
        tok=strtok(NULL," \t\r\n");
    }
    else if(!strcmp(tok,"fen")) {
        char fen[256]="";
        size_t used=0;
        for(int i=0; i<6; i++) {
            char *part=strtok(NULL," \t\r\n");
            if(!part)return;
            int n=snprintf(fen+used,sizeof(fen)-used,"%s%s",i?" ":"",part);
            if(n<0||(size_t)n>=sizeof(fen)-used)return;
            used+=(size_t)n;
        }
        if(!parse_fen(p,fen))set_startpos(p);
        tok=strtok(NULL," \t\r\n");
    }
    else return;
    if(tok&&!strcmp(tok,"moves")) {
        while((tok=strtok(NULL," \t\r\n"))) {
            Move m;
            if(parse_uci_move(p,tok,&m)) {
                Undo u;
                make_move(p,m,&u);
            }
        }
    }
}
static void go_search(Position *p, int max_depth) {
    nodes = 0;
    atomic_store(&stop_search, 0);
    Move best = {-1,-1,0,0};
    int bestscore = 0;
    search_start_ms = now_ms();
    for (int d=1; d<=max_depth && d<MAX_PLY; d++) {
        Move candidate = best;
        int score = negamax(p, d, -INF, INF, 0, &candidate);
        if (atomic_load(&stop_search)) break;
        if (candidate.from >= 0) best = candidate;
        bestscore = score;
        char b[8];
        move_to_uci(best,b);
        if (bestscore > MATE - MAX_PLY) printf("info depth %d score mate %d nodes %lld time %lld pv %s\n", d, (MATE-bestscore+1)/2, nodes, now_ms()-search_start_ms, b);
        else if (bestscore < -MATE + MAX_PLY) printf("info depth %d score mate %d nodes %lld time %lld pv %s\n", d, -((MATE+bestscore+1)/2), nodes, now_ms()-search_start_ms, b);
        else printf("info depth %d score cp %d nodes %lld time %lld pv %s\n", d, bestscore, nodes, now_ms()-search_start_ms, b);
        fflush(stdout);
    }
    char b[8];
    move_to_uci(best,b);
    printf("bestmove %s\n",b);
    fflush(stdout);
}
static void *search_worker(void *unused) {
    (void)unused;
    go_search(&active_position, requested_depth);
    atomic_store(&search_running, 0);
    return NULL;
}
static void stop_and_join(void) {
    if (search_thread_started) {
        atomic_store(&stop_search, 1);
        pthread_join(search_thread, NULL);
        search_thread_started = 0;
        atomic_store(&search_running, 0);
    }
}
static void parse_go(const char *line, const Position *p) {
    int depth = MAX_PLY-2;
    long long movetime = 0, wtime = -1, btime = -1, winc = 0, binc = 0;
    int movestogo = 30, infinite = 0;
    char copy[4096];
    snprintf(copy, sizeof(copy), "%s", line);
    char *tok = strtok(copy, " \t\r\n");
    while ((tok = strtok(NULL, " \t\r\n")) != NULL) {
        if (!strcmp(tok,"depth")) {
            tok=strtok(NULL," \t\r\n");
            if(tok) depth=atoi(tok);
        }
        else if (!strcmp(tok,"movetime")) {
            tok=strtok(NULL," \t\r\n");
            if(tok) movetime=atoll(tok);
        }
        else if (!strcmp(tok,"wtime")) {
            tok=strtok(NULL," \t\r\n");
            if(tok) wtime=atoll(tok);
        }
        else if (!strcmp(tok,"btime")) {
            tok=strtok(NULL," \t\r\n");
            if(tok) btime=atoll(tok);
        }
        else if (!strcmp(tok,"winc")) {
            tok=strtok(NULL," \t\r\n");
            if(tok) winc=atoll(tok);
        }
        else if (!strcmp(tok,"binc")) {
            tok=strtok(NULL," \t\r\n");
            if(tok) binc=atoll(tok);
        }
        else if (!strcmp(tok,"movestogo")) {
            tok=strtok(NULL," \t\r\n");
            if(tok) movestogo=atoi(tok);
        }
        else if (!strcmp(tok,"infinite")) infinite=1;
    }
    if (depth < 1) depth=1;
    if (depth > MAX_PLY-2) depth=MAX_PLY-2;
    search_time_ms = 0;
    search_infinite = infinite;
    if (movetime > 0) search_time_ms = movetime > 20 ? movetime - 10 : movetime;
    else if (!infinite && (wtime >= 0 || btime >= 0)) {
        long long remain = p->side == WHITE ? wtime : btime;
        long long inc = p->side == WHITE ? winc : binc;
        if (remain >= 0) {
            long long budget = remain / (movestogo > 0 ? movestogo : 30) + inc * 3 / 4;
            long long cap = remain > 30 ? remain / 2 : (remain > 5 ? remain - 2 : 1);
            if (budget > cap) budget = cap;
            if (budget < 1) budget = 1;
            search_time_ms = budget;
        }
    }
    requested_depth = depth;
    active_position = *p;
    atomic_store(&stop_search, 0);
    atomic_store(&search_running, 1);
    /*
     * Alpine/musl may give new pthreads a relatively small default stack.
     * Quiescence search keeps two large MoveList objects on each recursive
     * frame, so depth-heavy searches can exhaust that stack. Give the worker
     * an explicit 4 MiB stack.
     */
    pthread_attr_t attr;
    int attr_ok = pthread_attr_init(&attr) == 0;
    if (attr_ok && pthread_attr_setstacksize(&attr, 4U * 1024U * 1024U) != 0) {
        pthread_attr_destroy(&attr);
        attr_ok = 0;
    }
    int create_rc = pthread_create(&search_thread, attr_ok ? &attr : NULL,
                                   search_worker, NULL);
    if (attr_ok) pthread_attr_destroy(&attr);
    if (create_rc != 0) {
        atomic_store(&search_running, 0);
        go_search(&active_position, requested_depth);
    } else {
        search_thread_started = 1;
    }
}
static unsigned long long perft(Position *p,int depth) {
    if(depth==0)return 1;
    MoveList l;
    generate_legal(p,&l);
    unsigned long long n=0;
    for(int i=0; i<l.count; i++) {
        Undo u;
        if(!make_move(p,l.moves[i],&u))continue;
        n+=perft(p,depth-1);
        unmake_move(p,&u);
    }
    return n;
}

/* Matches a whole leading token, so "ucix" or "gogo" are not mistaken for
 * "uci" or "go". The token must be followed by end-of-line or whitespace. */
static int cmd_is(const char *line, const char *cmd) {
    size_t n = strlen(cmd);
    if (strncmp(line, cmd, n) != 0) return 0;
    char c = line[n];
    return c == '\0' || c == '\n' || c == '\r' || c == ' ' || c == '\t';
}

/* ROOKZERO_NO_MAIN lets the test harnesses link against this file without
 * pulling in the UCI main loop. */
#ifndef ROOKZERO_NO_MAIN
int main(void) {
    init_zobrist();
    Position pos;
    set_startpos(&pos);
    char line[4096];
    while (fgets(line,sizeof(line),stdin)) {
        if (cmd_is(line,"uci")) {
            printf("id name RookZero " VERSION "\nid author RookZero contributors\nuciok\n");
            fflush(stdout);
        } else if (cmd_is(line,"isready")) {
            printf("readyok\n");
            fflush(stdout);
        } else if (cmd_is(line,"stop")) {
            stop_and_join();
        } else if (cmd_is(line,"quit")) {
            stop_and_join();
            break;
        } else if (cmd_is(line,"ucinewgame")) {
            stop_and_join();
            set_startpos(&pos);
            memset(tt_key,0,sizeof(tt_key));
            memset(history_heur,0,sizeof(history_heur));
            memset(killer,0,sizeof(killer));
        } else if (cmd_is(line,"position")) {
            stop_and_join();
            do_position(&pos,line);
        } else if (cmd_is(line,"go")) {
            stop_and_join();
            parse_go(line,&pos);
        } else if (cmd_is(line,"perft")) {
            stop_and_join();
            int d=atoi(line+5);
            if(d<0)d=0;
            if(d>MAX_PERFT_DEPTH)d=MAX_PERFT_DEPTH;
            printf("perft %d: %llu\n",d,perft(&pos,d));
            fflush(stdout);
        } else if (cmd_is(line,"d")) {
            stop_and_join();
            print_board(&pos);
            fflush(stdout);
        } else if (cmd_is(line,"lm")) {
            stop_and_join();
            MoveList l;
            generate_legal(&pos,&l);
            for(int i=0; i<l.count; i++) {
                char b[8];
                move_to_uci(l.moves[i],b);
                printf("%s\n",b);
            }
            printf("end\n");
            fflush(stdout);
        } else if (cmd_is(line,"echo")) {
            stop_and_join();
            char arg[1024];
            snprintf(arg, sizeof(arg), "%s", line + 4);
            char *a = arg;
            while (*a == ' ' || *a == '\t') a++;
            a[strcspn(a, "\r\n")] = '\0';
            printf("%s\n", a);
            fflush(stdout);
        }
    }
    stop_and_join();
    return 0;
}
#endif /* ROOKZERO_NO_MAIN */
