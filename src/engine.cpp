#include "engine.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int INF = 1000000;
constexpr int MATE = 100000;
static_assert(MATE == MATE_SCORE, "the two mate scales must agree");

const int PIECE_VALUE[7] = {0, 100, 320, 330, 500, 900, 0};

// Piece-square tables, white perspective, rank 8 printed first.
const int PST_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0};

const int PST_KNIGHT[64] = {
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50};

const int PST_BISHOP[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -20,-10,-10,-10,-10,-10,-10,-20};

const int PST_ROOK[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0};

const int PST_QUEEN[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5,  5,  5,  5,  0,-10,
    -5,  0,  5,  5,  5,  5,  0, -5,
     0,  0,  5,  5,  5,  5,  0, -5,
   -10,  5,  5,  5,  5,  5,  0,-10,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20};

const int PST_KING_MID[64] = {
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -10,-20,-20,-20,-20,-20,-20,-10,
    20, 20,  0,  0,  0,  0, 20, 20,
    20, 30, 10,  0,  0, 10, 30, 20};

const int PST_KING_END[64] = {
   -50,-40,-30,-20,-20,-30,-40,-50,
   -30,-20,-10,  0,  0,-10,-20,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-30,  0,  0,  0,  0,-30,-30,
   -50,-30,-30,-30,-30,-30,-30,-50};

const int* tableFor(PieceType t) {
    switch (t) {
        case PAWN:   return PST_PAWN;
        case KNIGHT: return PST_KNIGHT;
        case BISHOP: return PST_BISHOP;
        case ROOK:   return PST_ROOK;
        case QUEEN:  return PST_QUEEN;
        default:     return PST_KING_MID;
    }
}

// --------------------------------------------------------------------------
// Transposition table
//
// One table per thread (each thread drives its own Engine instance, so no
// locking is needed), addressed by the Zobrist key the position already
// carries. Entries hold at most a lower bound or an exact score -- a
// fail-low position has no sound value to keep -- and never a mate score,
// whose distance depends on the ply it was found at.
//
// Two flavours of "exact": scores written by the playing search may carry
// the small approximations of null-move pruning and late move reductions,
// while scores written by exact mode (assess, rankMoves -- the badge and
// hint paths) never do. Each side therefore reads only what it can trust.
constexpr int TT_BITS = 19;
constexpr size_t TT_SIZE = size_t{1} << TT_BITS;
constexpr uint8_t TT_LOWER = 1;      // true value >= score (sound to cut with)
constexpr uint8_t TT_EXACT_J = 2;    // exact score from exact mode
constexpr uint8_t TT_EXACT_P = 3;    // exact score from the playing search

struct TTEntry {
    uint64_t key = 0;
    Move move;           // the move that caused the stored score
    int32_t score = 0;
    int16_t depth = 0;   // depth it was searched to
    uint8_t flag = 0;    // 0 = empty, or one of the three above
};

std::vector<TTEntry>& ttTable() {
    static thread_local std::vector<TTEntry> table(TT_SIZE);
    return table;
}

// Does `side` still own anything beyond pawns and the king? Null-move
// pruning is unsound when it does not: in a bare endgame, passing the move
// can be exactly what loses.
bool hasNonPawn(const Position& pos, Color side) {
    for (int sq = 0; sq < 64; ++sq) {
        const Piece p = pos.board[sq];
        if (isEmpty(p) || colorOf(p) != side) continue;
        const PieceType t = typeOf(p);
        if (t != PAWN && t != KING) return true;
    }
    return false;
}

// Mobility step tables: knights jump, bishops and rooks slide until something
// stops them. (Kings and pawns are left out -- their tables already encode
// where they want to stand, and pawn mobility is just "may push".)
const int KNIGHT_D[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
const int DIAG_D[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
const int ORTHO_D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

}  // namespace

int Engine::evaluate(const Position& pos) {
    int white = 0, black = 0;
    int nonPawn[2] = {0, 0};
    int bishops[2] = {0, 0};

    // Pawn counts and ranks per file, gathered in the piece pass and used by
    // the structure terms below. Small fixed arrays: eval is the hottest
    // function in the engine and must never allocate.
    int pawnCount[2][8] = {};
    uint8_t pawnRank[2][8][8] = {};

    for (int sq = 0; sq < 64; ++sq) {
        Piece p = pos.board[sq];
        if (isEmpty(p)) continue;
        PieceType t = typeOf(p);
        int val = PIECE_VALUE[t] + tableFor(t)[pstIndex(sq)];
        if (colorOf(p) == WHITE) {
            white += val;
            if (t == BISHOP) ++bishops[0];
            if (t != PAWN && t != KING) nonPawn[0] += PIECE_VALUE[t];
        } else {
            // Mirror the table for black.
            black += PIECE_VALUE[t] + tableFor(t)[pstIndex(sq ^ 56)];
            if (t == BISHOP) ++bishops[1];
            if (t != PAWN && t != KING) nonPawn[1] += PIECE_VALUE[t];
        }
        if (t == PAWN) {
            const int c = colorOf(p);
            const int f = fileOf(sq);
            pawnRank[c][f][pawnCount[c][f]++] = (uint8_t)rankOf(sq);
        }
    }

    // Bishop pair bonus
    if (bishops[0] >= 2) white += 30;
    if (bishops[1] >= 2) black += 30;

    // Pawn structure. Doubled and isolated pawns cost; passed pawns pay by
    // how far they have advanced -- the biggest structural term in an
    // otherwise material-driven eval. Values follow the classical simplified
    // conventions. The starting position still evaluates to zero: no pawn is
    // doubled, isolated or passed, and every rook stands behind its own pawn.
    static constexpr int PASSED[8] = {0, 5, 10, 20, 40, 70, 110, 0};
    for (int c = 0; c < 2; ++c) {
        int bonus = 0, penalty = 0;
        for (int f = 0; f < 8; ++f) {
            const int n = pawnCount[c][f];
            if (n > 1) penalty += 14 * (n - 1);  // doubled
            for (int i = 0; i < n; ++i) {
                const int r = pawnRank[c][f][i];
                const bool isolated =
                    (f == 0 || pawnCount[c][f - 1] == 0) && (f == 7 || pawnCount[c][f + 1] == 0);
                if (isolated) penalty += 14;
                // Passed: no enemy pawn on this file or the neighbours at any
                // rank ahead of this one.
                bool passed = true;
                for (int nf = std::max(0, f - 1); nf <= std::min(7, f + 1) && passed; ++nf)
                    for (int j = 0; j < pawnCount[1 - c][nf]; ++j) {
                        const int er = pawnRank[1 - c][nf][j];
                        if (c == WHITE ? er > r : er < r) passed = false;
                    }
                if (passed) bonus += PASSED[c == WHITE ? r : 7 - r];
            }
        }
        if (c == WHITE) {
            white += bonus;
            black += penalty;
        } else {
            black += bonus;
            white += penalty;
        }
    }

    // Rooks behind no pawn of their own: an open file is worth 25, a
    // half-open one (only the enemy has pawns there) 12.
    for (int sq = 0; sq < 64; ++sq) {
        const Piece p = pos.board[sq];
        if (isEmpty(p) || typeOf(p) != ROOK) continue;
        const int c = colorOf(p), f = fileOf(sq);
        if (pawnCount[c][f] > 0) continue;
        const int value = pawnCount[1 - c][f] > 0 ? 12 : 25;
        if (c == WHITE)
            white += value;
        else
            black += value;
    }

    // Mobility: the squares each piece can actually step or slide to. A
    // knight with three squares out earns 12, a bishop staring at its own
    // pawn nothing -- the term rewards pieces that came out to play, and is
    // symmetric in the starting position (still a net zero there).
    int mob[2] = {0, 0};
    for (int sq = 0; sq < 64; ++sq) {
        const Piece p = pos.board[sq];
        if (isEmpty(p)) continue;
        const PieceType t = typeOf(p);
        const int f = fileOf(sq), r = rankOf(sq);
        if (t == KNIGHT) {
            for (const auto& d : KNIGHT_D) {
                const int nf = f + d[0], nr = r + d[1];
                if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
                const Piece q = pos.board[makeSquare(nf, nr)];
                if (isEmpty(q) || colorOf(q) != colorOf(p)) mob[colorOf(p)] += 4;
            }
        } else if (t != PAWN && t != KING) {
            // Sliders: walk each ray until the first piece; its square counts
            // only when it belongs to the other side.
            auto slide = [&](const int (*ds)[2], int gain) {
                for (int i = 0; i < 4; ++i) {
                    int nf = f + ds[i][0], nr = r + ds[i][1];
                    while (nf >= 0 && nf <= 7 && nr >= 0 && nr <= 7) {
                        const Piece q = pos.board[makeSquare(nf, nr)];
                        if (isEmpty(q)) {
                            mob[colorOf(p)] += gain;
                        } else {
                            if (colorOf(q) != colorOf(p)) mob[colorOf(p)] += gain;
                            break;
                        }
                        nf += ds[i][0];
                        nr += ds[i][1];
                    }
                }
            };
            if (t == BISHOP)
                slide(DIAG_D, 5);
            else if (t == ROOK)
                slide(ORTHO_D, 2);
            else {  // queen: both, at the low per-square rate
                slide(DIAG_D, 1);
                slide(ORTHO_D, 1);
            }
        }
    }
    white += mob[0];
    black += mob[1];

    // Use the endgame king table when most material has left the board.
    bool endgame = nonPawn[0] + nonPawn[1] <= 2500;
    if (endgame) {
        for (int sq = 0; sq < 64; ++sq) {
            Piece p = pos.board[sq];
            if (isEmpty(p) || typeOf(p) != KING) continue;
            int delta = PST_KING_END[pstIndex(sq)] - PST_KING_MID[pstIndex(sq)];
            if (colorOf(p) == WHITE)
                white += delta;
            else
                black += delta;
        }
    }

    int score = white - black;
    return pos.side == WHITE ? score : -score;
}

bool Engine::timeUp() {
    if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) {
        stopped = true;
        return true;
    }
    if (Clock::now() >= deadline) {
        stopped = true;
        return true;
    }
    return false;
}

void Engine::orderMoves(const Position& pos, std::vector<Move>& moves, int ply) {
    // Alpha-beta gets its speed from ordering: the transposition table's move
    // first, then the big captures, then the quiet moves that have cut off
    // here before (two killers per ply), then the history of quiet successes
    // from-to. Ordering never changes a score -- it only decides how soon the
    // search finds the line it needs.
    constexpr int TT_BONUS = 1 << 25, CAP_BONUS = 1 << 24, PROMO_BONUS = 1 << 23;
    constexpr int KILLER_BONUS = 1 << 22;
    std::vector<std::pair<int, Move>> scored;
    scored.reserve(moves.size());
    for (const Move& m : moves) {
        int s = 0;
        Piece victim = pos.board[m.to];
        if (!isEmpty(victim))
            s += CAP_BONUS + 10 * PIECE_VALUE[typeOf(victim)] - PIECE_VALUE[typeOf(pos.board[m.from])];
        else if (m.enPassant)
            s += CAP_BONUS + 9 * PIECE_VALUE[PAWN];
        else if (ply >= 0 && ply < MAX_PLY && m == killers[0][ply])
            s += KILLER_BONUS;
        else if (ply >= 0 && ply < MAX_PLY && m == killers[1][ply])
            s += KILLER_BONUS - 256;
        else
            s += historyHeuristic[m.from][m.to];
        if (m.promotion != NO_PIECE) s += PROMO_BONUS + 9 * PIECE_VALUE[m.promotion];
        if (m == ttMove) s += TT_BONUS;
        scored.emplace_back(s, m);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const std::pair<int, Move>& a, const std::pair<int, Move>& b) { return a.first > b.first; });
    for (size_t i = 0; i < moves.size(); ++i) moves[i] = scored[i].second;
}

int Engine::quiesce(Position& pos, int alpha, int beta, int ply, int qply) {
    if (stopped) return 0;
    if ((++nodes & 2047) == 0 && timeUp()) return 0;
    if (ply > 0 && (pos.isRepetitionDraw() || pos.halfmoveClock >= 100)) return 0;

    int stand = evaluate(pos);
    if (noise > 0) stand += int(rng() % unsigned(2 * noise + 1)) - noise;

    // In check there is no standing pat: the side to move has to move, and
    // every move is in scope, not only the captures. Without this the search
    // could "evaluate" a position it is not allowed to simply pass in -- the
    // classic way a bot ends a checking chain on a raw number.
    const bool check = pos.inCheck(pos.side);
    if (!check) {
        if (stand >= beta) return beta;
        if (stand > alpha) alpha = stand;
        if (qply >= 6) return stand;  // quiet: six plies of captures is plenty
    }

    std::vector<Move> all = pos.legalMoves();
    // No legal move at all: the horizon contained a mate (in check that is
    // checkmate -- score it as one, not as a static number) or, on the quiet
    // side, a stalemate.
    if (all.empty()) return check ? -MATE + ply : stand;
    if (check && qply >= 12) return stand;  // a bounded check chain: take the score
    if (!check) {
        std::vector<Move> caps;
        caps.reserve(all.size());
        for (const Move& m : all) {
            if (!isEmpty(pos.board[m.to]) || m.enPassant || m.promotion != NO_PIECE) caps.push_back(m);
        }
        if (caps.empty()) return stand;
        all.swap(caps);
    }
    orderMoves(pos, all, ply);

    int best = check ? -INF : stand;
    for (const Move& m : all) {
        // Delta pruning: a capture that could not raise alpha even if it
        // went through completely free is not worth playing out. An
        // approximation, so -- like the other pruning -- the playing search
        // only; exact mode always looks.
        if (!check && !exactMode) {
            int gain = m.enPassant ? PIECE_VALUE[PAWN] : PIECE_VALUE[typeOf(pos.board[m.to])];
            if (m.promotion != NO_PIECE) gain += PIECE_VALUE[m.promotion] - PIECE_VALUE[PAWN];
            if (stand + gain + 150 < alpha) continue;
        }
        UndoInfo u;
        pos.doMove(m, u);
        int s = -quiesce(pos, -beta, -alpha, ply + 1, qply + 1);
        pos.undoMove(m, u);
        if (stopped) return 0;
        if (s > best) best = s;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    return best;
}

int Engine::negamax(Position& pos, int depth, int alpha, int beta, int ply) {
    if (stopped) return 0;
    if ((++nodes & 2047) == 0 && timeUp()) return 0;
    if (ply > 0 && (pos.isRepetitionDraw() || pos.halfmoveClock >= 100)) return 0;
    if (ply >= MAX_PLY) return quiesce(pos, alpha, beta, ply, 0);

    // Never stop the search on a check: extend the node instead, so a
    // forcing line is always read until it runs out of checks.
    if (pos.inCheck(pos.side)) ++depth;
    if (depth <= 0) return quiesce(pos, alpha, beta, ply, 0);

    // Transposition table. The remembered move feeds the ordering; the score
    // is used only when it is deep enough and of a flavour this mode may
    // trust (exact mode reads nothing that pruning could have bent). The move
    // is kept in a local: the null-move probe below recurses first, and those
    // children would overwrite the member with their own table moves.
    std::vector<TTEntry>& tt = ttTable();
    TTEntry& entry = tt[pos.key & (TT_SIZE - 1)];
    Move ttBest;
    if (entry.key == pos.key && entry.flag != 0) {
        ttBest = entry.move;  // ordering aid: always safe
        const bool scoreUsable =
            entry.depth >= depth &&
            (entry.flag == TT_EXACT_J || (!exactMode && (entry.flag == TT_EXACT_P || entry.flag == TT_LOWER)));
        if (scoreUsable) {
            if (entry.flag != TT_LOWER) return entry.score;
            if (entry.score >= beta) return beta;
        }
    }

    // Null-move pruning: when the static score already says we stand at
    // least as high as beta, hand the move over and look from the other
    // side. If the side that just passed is still fine, the position is not
    // worse than beta. Off in exact mode (the returned bound is a guess),
    // off in check, and off in bare endgames, where passing can be losing.
    if (!exactMode && depth >= 3 && !pos.inCheck(pos.side) && beta < MATE_BOUND &&
        hasNonPawn(pos, pos.side) && evaluate(pos) >= beta) {
        const int R = 2 + depth / 8;
        UndoInfo u;
        pos.doNullMove(u);
        const int s = -negamax(pos, depth - 1 - R, -beta, -beta + 1, ply + 1);
        pos.undoNullMove(u);
        if (stopped) return 0;
        if (s >= beta) return beta;
    }

    std::vector<Move> moves = pos.legalMoves();
    if (moves.empty()) return pos.inCheck(pos.side) ? -MATE + ply : 0;

    ttMove = ttBest;  // ordering aid for exactly this node
    orderMoves(pos, moves, ply);

    const int alpha0 = alpha;
    int best = -INF;
    Move bestMove = moves[0];

    for (size_t i = 0; i < moves.size(); ++i) {
        const Move m = moves[i];
        const bool quiet = isEmpty(pos.board[m.to]) && !m.enPassant && m.promotion == NO_PIECE;
        UndoInfo u;
        pos.doMove(m, u);

        // One re-search budget for the two speed tricks, and both keep the
        // score exact the moment it proves interesting:
        //  - reductions: a quiet move far down a well-ordered list is looked
        //    at with one ply less (two past move six at depth six);
        //  - principal variation search: everything after the first move is
        //    first asked the narrow question "better than alpha?" and only
        //    searched in full when the answer could be yes.
        // Captures and moves giving check keep full depth, and exact mode
        // keeps full windows throughout.
        int s;
        if (i == 0 || exactMode) {
            s = -negamax(pos, depth - 1, -beta, -alpha, ply + 1);
        } else {
            int r = 0;
            if (quiet && depth >= 3 && i >= 3 && !pos.inCheck(pos.side))
                r = (depth >= 6 && i >= 6) ? 2 : 1;
            s = -negamax(pos, depth - 1 - r, -alpha - 1, -alpha, ply + 1);
            if (s > alpha && !stopped) s = -negamax(pos, depth - 1, -beta, -alpha, ply + 1);
        }

        pos.undoMove(m, u);
        if (stopped) return 0;
        if (s > best) {
            best = s;
            bestMove = m;
        }
        if (s > alpha) alpha = s;
        if (alpha >= beta) {
            // A quiet move cut the search off: it is worth trying first here
            // next time (killer) and it marks this from-to as promising
            // (history).
            if (quiet && ply < MAX_PLY) {
                if (!(m == killers[0][ply])) {
                    killers[1][ply] = killers[0][ply];
                    killers[0][ply] = m;
                }
                int& h = historyHeuristic[m.from][m.to];
                h = std::min(h + depth * depth, 1 << 21);
            }
            break;
        }
    }

    // Remember the result. Fail-lows are skipped: their `best` is a bound,
    // not a value. Mate scores are skipped too: they are ply-relative and
    // would be read back at the wrong distance.
    if (!stopped && best > -INF && std::abs(best) < MATE_BOUND) {
        if (best >= beta || best > alpha0) {
            TTEntry& store = tt[pos.key & (TT_SIZE - 1)];
            if (store.key != pos.key || store.depth <= depth) {
                store.key = pos.key;
                store.move = bestMove;
                store.score = (int32_t)best;
                store.depth = (int16_t)depth;
                store.flag = (uint8_t)(best >= beta ? TT_LOWER : (exactMode ? TT_EXACT_J : TT_EXACT_P));
            }
        }
    }

    return best;
}

SearchResult Engine::search(Position root, int maxDepth, double timeLimit, int noiseLevel, uint64_t seed) {
    stopped = false;
    nodes = 0;
    noise = noiseLevel;
    exactMode = false;  // playing: every strength heuristic is allowed
    rng.seed(seed ? seed : (uint64_t)0x123456789ABCDEFULL);
    deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeLimit));

    SearchResult res;
    std::vector<Move> moves = root.legalMoves();
    if (moves.empty()) return res;

    res.found = true;
    res.move = moves[0];
    std::shuffle(moves.begin(), moves.end(), rng);
    if (noise == 0) orderMoves(root, moves, 0);  // deterministic play: start ordered too

    int bestScore = 0;
    int completedDepth = 0;

    for (int d = 1; d <= maxDepth; ++d) {
        const bool fullWindow = (noise > 0);

        // Aspiration windows: from depth 3 on, search around the previous
        // depth's score instead of the whole range. When the true score
        // falls outside the window the root loop notices -- a move reaches
        // the top edge (fail high) or nothing escapes the bottom one (fail
        // low) -- and the depth is re-searched with a wider window, up to
        // the full range after the ladder runs out.
        constexpr int ASP[3] = {50, 150, 450};
        int attempt = 0;
        if (fullWindow || d < 3 || completedDepth != d - 1 ||
            std::abs(bestScore) >= MATE_BOUND)
            attempt = 3;  // not eligible: plain full-window search

        std::vector<int> scores(moves.size(), -INF);
        std::vector<char> exact(moves.size(), 0);
        bool complete = true;
        int best = -INF;
        while (true) {
            const int lo = attempt == 3 ? -INF : bestScore - ASP[attempt];
            const int hi = attempt == 3 ? INF : bestScore + ASP[attempt];
            int alpha = lo;
            bool failHigh = false;
            complete = true;
            std::fill(scores.begin(), scores.end(), -INF);

            for (size_t i = 0; i < moves.size(); ++i) {
                int window = fullWindow ? -INF : alpha;
                UndoInfo u;
                root.doMove(moves[i], u);
                int s = -negamax(root, d - 1, -hi, -window, 1);
                root.undoMove(moves[i], u);
                if (stopped) {
                    complete = false;
                    break;
                }
                scores[i] = s;
                // A move that fails low returns a bound, not its true score, so it must
                // never win the pick even if the bound ties the best value.
                exact[i] = fullWindow || s > alpha;
                if (s >= hi) {  // the window blew at the top: widen and redo the depth
                    failHigh = true;
                    break;
                }
                if (!fullWindow && s > alpha) alpha = s;
            }
            if (!complete) break;

            best = -INF;
            for (int s : scores)
                if (s > best) best = s;

            if (attempt < 3 && (failHigh || best <= lo)) {
                ++attempt;  // the score slipped out of the window: widen it
                continue;
            }
            break;  // the window held (or the full range was used)
        }
        if (!complete) break;

        // With noise enabled, pick randomly among moves close to the best score.
        std::vector<size_t> candidates;
        for (size_t i = 0; i < scores.size(); ++i)
            if (exact[i] && scores[i] >= best - noise) candidates.push_back(i);
        size_t pick = candidates.empty() ? 0 : candidates[rng() % candidates.size()];

        res.move = moves[pick];
        bestScore = best;
        completedDepth = d;

        // Root the next iteration in this one's results: try the moves that
        // just scored highest first. Fail-low moves keep only their bound,
        // which is still a usable hint, and the stable sort leaves equal
        // scores in their shuffled order.
        std::vector<std::pair<int, size_t>> by;
        by.reserve(moves.size());
        for (size_t i = 0; i < moves.size(); ++i) by.emplace_back(scores[i], i);
        std::stable_sort(by.begin(), by.end(),
                         [](const std::pair<int, size_t>& a, const std::pair<int, size_t>& b) {
                             return a.first > b.first;
                         });
        std::vector<Move> ordered(moves.size());
        for (size_t i = 0; i < by.size(); ++i) ordered[i] = moves[by[i].second];
        moves.swap(ordered);

        if (std::abs(best) >= MATE_BOUND) break;
    }

    res.score = bestScore;
    res.depth = completedDepth;
    res.nodes = nodes;
    res.mate = std::abs(bestScore) >= MATE_BOUND;
    res.cancelled = stopped;
    return res;
}

// --------------------------------------------------------------------------
// What does the move leave lying on the target square?
//
// A static exchange evaluation on the square *after* our piece has landed
// there: they take (that is what being offered a piece means), and from then
// on each side may either capture with its cheapest legal attacker or decline,
// keeping whichever option leaves it better off -- the mover maximises the
// balance, the opponent minimises it. `net` is the mover's material from the
// whole exchange: what they took, minus what they lose, plus what they win
// back. 0 means an even trade or a free capture, ~230 a bishop offered for a
// pawn, ~220 the queen sac the mate test below is built around. Recapturing
// into a defended square is never forced, so an even trade stays at ~0 even
// when one side has a pointless recapture available.
namespace {

// Cheapest piece `side` can legally capture on `sq` with, or a null move
// (from == to, which no legal move ever has) when the square cannot be taken.
Move cheapestCapture(Position& root, int sq, Color side) {
    if (isEmpty(root.board[sq])) return Move();  // only real captures continue it
    Move best;
    bool have = false;
    int bestValue = 0;
    for (const Move& m : root.legalMoves()) {
        if (m.to != sq) continue;
        const Piece p = root.board[m.from];
        if (isEmpty(p) || colorOf(p) != side) continue;
        int v = PIECE_VALUE[typeOf(p)];
        if (m.promotion != NO_PIECE) v += PIECE_VALUE[m.promotion] - PIECE_VALUE[PAWN];
        if (!have || v < bestValue) {
            best = m;
            bestValue = v;
            have = true;
        }
    }
    return have ? best : Move();
}

// Play the exchange out from here, letting both sides decline. `net` is the
// mover's material so far; `side` is to move.
int settle(Position& root, int sq, Color mover, Color side, int net, int depth) {
    const Move cap = cheapestCapture(root, sq, side);
    if (cap.from == cap.to || depth <= 0) return net;  // nobody takes: stop here
    const int worth = PIECE_VALUE[typeOf(root.board[sq])];
    UndoInfo u;
    root.doMove(cap, u);
    const int after = settle(root, sq, mover, ~side, side == mover ? net + worth : net - worth,
                             depth - 1);
    root.undoMove(cap, u);
    return side == mover ? std::max(net, after) : std::min(net, after);
}

}  // namespace

int sacrificeNet(const Position& before, const Move& played) {
    const Piece taken = before.board[played.to];
    Position root = before;
    UndoInfo u;
    root.doMove(played, u);

    int net = isEmpty(taken) ? 0 : PIECE_VALUE[typeOf(taken)];
    const Piece sitting = root.board[played.to];
    if (isEmpty(sitting) || colorOf(sitting) != before.side) {  // nothing of ours there
        root.undoMove(played, u);
        return 0;
    }

    // The offer itself is not optional: this is what "hanging" means. Their
    // cheapest legal capture starts the chain, and everything after it is up
    // to whoever thinks they come out ahead.
    const Move first = cheapestCapture(root, played.to, ~before.side);
    if (first.from == first.to) {  // nothing can take it: no sacrifice
        root.undoMove(played, u);
        return 0;
    }
    UndoInfo u2;
    root.doMove(first, u2);
    net -= PIECE_VALUE[typeOf(sitting)];
    net = settle(root, played.to, before.side, before.side, net, 39);
    root.undoMove(first, u2);

    root.undoMove(played, u);
    return std::max(-net, 0);  // only material actually given away counts
}

// --------------------------------------------------------------------------
// Move assessment
//
// Scores every root move so the played one can be judged against the strongest
// alternative. The played move is searched *first*: with the window still wide
// open it can never fail low, so its score is always exact and no second pass
// is needed. Scores of the remaining moves are exact when they raise alpha and
// an upper bound when they do not, which is all the classifier needs.
Assessment Engine::assess(const Position& before, const Move& played, int maxDepth, double timeLimit) {
    Assessment a;
    stopped = false;
    nodes = 0;
    noise = 0;
    exactMode = true;  // judging: scores must be exact, no pruning approximations
    rng.seed(0xA11CE5EEDULL);
    deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeLimit));

    Position root = before;
    std::vector<Move> moves = root.legalMoves();
    if (moves.empty()) return a;
    a.legalCount = (int)moves.size();

    size_t playedAt = moves.size();
    for (size_t i = 0; i < moves.size(); ++i)
        if (moves[i] == played) {
            playedAt = i;
            break;
        }
    if (playedAt == moves.size()) return a;
    std::swap(moves[0], moves[playedAt]);

    std::vector<int> scores(moves.size(), -INF);
    bool haveScores = false;

    for (int d = 1; d <= maxDepth; ++d) {
        int alpha = -INF;
        std::vector<int> pass(moves.size(), -INF);
        bool complete = true;
        for (size_t i = 0; i < moves.size(); ++i) {
            UndoInfo u;
            root.doMove(moves[i], u);
            int s = -negamax(root, d - 1, -INF, -alpha, 1);
            root.undoMove(moves[i], u);
            if (stopped) {
                complete = false;
                break;
            }
            pass[i] = s;
            if (s > alpha) alpha = s;
        }
        if (!complete) break;
        scores.swap(pass);
        haveScores = true;
        if (std::abs(alpha) >= MATE_BOUND) break;  // nothing deeper can change the verdict
    }
    if (!haveScores) return a;

    a.ok = true;
    a.played = scores[0];
    a.best = *std::max_element(scores.begin(), scores.end());
    a.secondUpper = -INF;
    for (size_t i = 1; i < scores.size(); ++i)
        if (scores[i] > a.secondUpper) a.secondUpper = scores[i];
    if (a.secondUpper == -INF) a.secondUpper = a.best;  // only one legal move
    for (size_t i = 0; i < moves.size(); ++i)
        if (scores[i] == a.best) {
            a.bestMove = moves[i];
            break;
        }

    a.sacNet = sacrificeNet(before, played);
    return a;
}

// Score every legal move of `root` in one rooted iterative-deepening search and
// hand them back strongest first. Each root move is searched with a wide-open
// window, so -- unlike a move that merely fails low -- every score is exact.
// This is what draws the best-move hints when there is no outside engine.
std::vector<ScoredMove> Engine::rankMoves(Position root, int maxDepth, double timeLimit) {
    std::vector<ScoredMove> out;
    stopped = false;
    nodes = 0;
    noise = 0;
    exactMode = true;  // hinting: every root score is promised to be exact
    rng.seed(0xB0A7C1EDULL);
    deadline = Clock::now() +
               std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeLimit));

    std::vector<Move> moves = root.legalMoves();
    if (moves.empty()) return out;

    std::vector<int> scores(moves.size(), -INF);
    bool haveScores = false;
    int doneDepth = 0;

    for (int d = 1; d <= maxDepth; ++d) {
        std::vector<int> pass(moves.size(), -INF);
        bool complete = true;
        for (size_t i = 0; i < moves.size(); ++i) {
            UndoInfo u;
            root.doMove(moves[i], u);
            int s = -negamax(root, d - 1, -INF, INF, 1);
            root.undoMove(moves[i], u);
            if (stopped) {
                complete = false;
                break;
            }
            pass[i] = s;
        }
        if (!complete) break;  // keep the deepest pass that finished
        scores.swap(pass);
        haveScores = true;
        doneDepth = d;
        int best = -INF;
        for (int s : scores)
            if (s > best) best = s;
        if (std::abs(best) >= MATE_BOUND) break;  // nothing deeper can change the order
    }
    if (!haveScores) return out;  // stopped before even the shallowest pass finished

    out.reserve(moves.size());
    for (size_t i = 0; i < moves.size(); ++i) out.push_back({moves[i], scores[i], doneDepth});
    std::sort(out.begin(), out.end(),
              [](const ScoredMove& a, const ScoredMove& b) { return a.score > b.score; });
    return out;
}

const char* tagName(MoveTag tag) {
    switch (tag) {
        case MoveTag::BRILLIANT:  return "Brilliant";
        case MoveTag::GREAT:      return "Great Move";
        case MoveTag::BEST:       return "Best Move";
        case MoveTag::EXCELLENT:  return "Excellent";
        case MoveTag::GOOD:       return "Good Move";
        case MoveTag::INACCURACY: return "Inaccuracy";
        case MoveTag::MISTAKE:    return "Mistake";
        case MoveTag::BLUNDER:    return "Blunder";
        case MoveTag::MISSED_WIN: return "Missed Win";
        default:                  return "";
    }
}

std::string tagSymbol(MoveTag tag) {
    switch (tag) {
        case MoveTag::BRILLIANT:  return "!!";
        case MoveTag::GREAT:      return "!";
        case MoveTag::BEST:       return "\u2605";  // ★
        case MoveTag::EXCELLENT:  return "\u2713";  // ✓
        case MoveTag::GOOD:       return "\u2022";  // • (never '+': that reads as a check)
        case MoveTag::INACCURACY: return "?!";
        case MoveTag::MISTAKE:    return "?";
        case MoveTag::BLUNDER:    return "??";
        case MoveTag::MISSED_WIN: return "\u2717";  // ✗
        default:                  return "";
    }
}

// Loss in centipawns relative to the strongest move decides the colour of the
// badge; sacrifice and margin checks promote an ordinary best move.
MoveTag classifyMove(const Assessment& a) {
    if (!a.ok || a.legalCount <= 1) return MoveTag::NONE;  // forced: nothing to praise or blame

    const int best = a.best;
    const int played = a.played;
    const int loss = best - played;
    const bool clearWinner = (best - a.secondUpper) >= 150 && best >= -250 && best <= 300;

    if (played >= MATE_BOUND) {  // mates are never a mistake, whatever the pace
        if (a.sacNet >= SAC_MIN) return MoveTag::BRILLIANT;
        return clearWinner ? MoveTag::GREAT : MoveTag::BEST;
    }
    if (loss <= 5) {
        if (a.sacNet >= SAC_MIN && played > -150) return MoveTag::BRILLIANT;
        return clearWinner ? MoveTag::GREAT : MoveTag::BEST;
    }
    if (loss <= 25) return MoveTag::EXCELLENT;
    if (loss <= 60) return MoveTag::GOOD;
    if (loss <= 120) return MoveTag::INACCURACY;
    if (best >= 300 && played >= 0) return MoveTag::MISSED_WIN;
    if (loss <= 250) return MoveTag::MISTAKE;
    return MoveTag::BLUNDER;
}
