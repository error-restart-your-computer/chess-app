#pragma once
// engine.h -- alpha-beta search with quiescence and piece-square evaluation
#include "position.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

struct SearchResult {
    Move move;
    int score = 0;
    int depth = 0;
    long long nodes = 0;
    bool mate = false;
    bool found = false;   // false when the side to move has no legal move
    bool cancelled = false;
};

// |score| at or above this means a forced mate (see MATE in engine.cpp).
constexpr int MATE_BOUND = 99000;
// Score for "mate in one ply"; an outside engine's "mate in m moves" maps onto
// this scale as MATE_SCORE - (2m - 1), exactly like the built-in engine counts.
constexpr int MATE_SCORE = 100000;

// chess.com-style judgement of a single move.
enum class MoveTag {
    NONE = 0,
    BRILLIANT,   // the best move and a real sacrifice
    GREAT,       // the only move that keeps the position together
    BEST,        // strongest move
    EXCELLENT,   // within ~0.25 pawn of the strongest
    GOOD,        // within ~0.6 pawn
    INACCURACY,  // gives away up to ~1.2 pawns
    MISTAKE,     // gives away up to ~2.5 pawns
    BLUNDER,     // gives away more
    MISSED_WIN   // a win was on the board and it is gone
};

// Everything needed to judge the move that was played in `before`.
struct Assessment {
    bool ok = false;
    int best = 0;         // strongest move's score (side to move's point of view)
    int played = 0;       // score of the move that was actually played
    int secondUpper = 0;  // upper bound on the strongest alternative to `played`
    int sacNet = 0;       // material the mover drops if the target square is taken
    int legalCount = 0;
    Move bestMove;
};

// Material the mover has to leave hanging before a best move is called a
// sacrifice: a bishop given for a pawn (230) qualifies, an even trade (10)
// never does.
constexpr int SAC_MIN = 150;

const char* tagName(MoveTag tag);
std::string tagSymbol(MoveTag tag);  // icon drawn after the SAN (UTF-8)
MoveTag classifyMove(const Assessment& a);

// Net material the mover offers up on the destination square: shared by the
// built-in assessor and the Stockfish one.
int sacrificeNet(const Position& before, const Move& played);

// One legal root move together with the score it was given (side to move's
// point of view, our centipawn/mate scale). Produced strongest first, both by
// Engine::rankMoves and by Stockfish's MultiPV search.
struct ScoredMove {
    Move move;
    int score = 0;
    int depth = 0;  // search depth the score came from (0 when unknown)
};

class Engine {
public:
    // maxDepth: hard ply limit; timeLimit: seconds; noise: eval randomness (0 = deterministic)
    SearchResult search(Position root, int maxDepth, double timeLimit, int noise, uint64_t seed = 0);

    // Score `played` against every alternative in `before` so the move can be judged.
    Assessment assess(const Position& before, const Move& played, int maxDepth, double timeLimit);

    // Every legal move of `root` with its score, strongest first -- the
    // built-in stand-in for asking an engine for its top N candidate moves.
    std::vector<ScoredMove> rankMoves(Position root, int maxDepth, double timeLimit);

    void setCancelFlag(std::atomic<bool>* flag) { cancelFlag = flag; }

    static int evaluate(const Position& pos);

    // Ply ceiling: check extensions can push a line deep, and this is where
    // they stop mattering (the draw guards and the qsearch cap already stop
    // it much earlier in practice).
    static constexpr int MAX_PLY = 128;

private:
    int negamax(Position& pos, int depth, int alpha, int beta, int ply);
    int quiesce(Position& pos, int alpha, int beta, int ply, int qply);
    bool timeUp();
    void orderMoves(const Position& pos, std::vector<Move>& moves, int ply);

    using Clock = std::chrono::steady_clock;
    Clock::time_point deadline{};
    long long nodes = 0;
    int noise = 0;
    bool stopped = false;
    std::atomic<bool>* cancelFlag = nullptr;
    std::mt19937 rng;

    // Judge/hint searches (assess, rankMoves) promise exact root scores, so
    // they turn off every heuristic that can bend a score -- null-move
    // pruning, late move reductions, transposition bounds. The playing search
    // keeps them all.
    bool exactMode = false;

    // Ordering aids: the transposition table's move for this position, two
    // killer slots per ply, and a from-to success count for quiet moves.
    Move ttMove;
    Move killers[2][MAX_PLY];
    int historyHeuristic[64][64] = {};
};
