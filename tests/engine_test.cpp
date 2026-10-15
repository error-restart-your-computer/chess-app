// tests/engine_test.cpp -- headless checks for move handling, SAN, status and the search
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <atomic>

#include "../src/engine.h"
#include "../src/levels.h"
#include "../src/position.h"

static char promoChar(PieceType t) {
    switch (t) {
        case QUEEN: return 'q';
        case ROOK: return 'r';
        case BISHOP: return 'b';
        case KNIGHT: return 'n';
        default: return ' ';
    }
}

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                       \
    do {                                                       \
        ++checks;                                              \
        if (!(cond)) {                                         \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                        \
        }                                                      \
    } while (0)

static bool play(Position& p, const std::string& coord) {
    if (coord.size() < 4) return false;
    int from = makeSquare(coord[0] - 'a', coord[1] - '1');
    int to = makeSquare(coord[2] - 'a', coord[3] - '1');
    PieceType promo = NO_PIECE;
    if (coord.size() >= 5) {
        switch (coord[4]) {
            case 'q': case 'Q': promo = QUEEN; break;
            case 'r': case 'R': promo = ROOK; break;
            case 'b': case 'B': promo = BISHOP; break;
            case 'n': case 'N': promo = KNIGHT; break;
            default: break;
        }
    }
    for (const Move& m : p.legalMoves()) {
        if (m.from == from && m.to == to && m.promotion == promo) {
            UndoInfo u;
            p.doMove(m, u);
            return true;
        }
    }
    return false;
}

static std::string san(Position& p, const std::string& coord) {
    int from = makeSquare(coord[0] - 'a', coord[1] - '1');
    int to = makeSquare(coord[2] - 'a', coord[3] - '1');
    for (const Move& m : p.legalMoves())
        if (m.from == from && m.to == to) return p.toSan(m);
    return "<illegal>";
}

static void testSan() {
    Position p;
    p.setStartPosition();
    CHECK(san(p, "e2e4") == "e4", "pawn move SAN");
    play(p, "e2e4");
    play(p, "e7e5");
    CHECK(san(p, "g1f3") == "Nf3", "knight SAN");
    play(p, "g1f3");
    play(p, "b8c6");
    CHECK(san(p, "f1b5") == "Bb5", "bishop SAN");
    play(p, "f1b5");
    play(p, "a7a6");
    CHECK(san(p, "b5a4") == "Ba4", "bishop retreat SAN");
    play(p, "b5a4");
    play(p, "g8f6");
    CHECK(san(p, "e1g1") == "O-O", "castling SAN");
    play(p, "e1g1");
    play(p, "f8c5");
    play(p, "c2c3");
    play(p, "d7d6");
    play(p, "d2d4");
    CHECK(san(p, "e5d4") == "exd4", "pawn capture SAN");
    play(p, "e5d4");
    CHECK(san(p, "c3d4") == "cxd4", "recapture SAN");

    // Disambiguation: two knights can both reach c3.
    Position d;
    d.setFen("4k3/8/8/8/8/8/N3N3/4K3 w - - 0 1");
    CHECK(san(d, "a2c3") == "Nac3", "file disambiguation");
    CHECK(san(d, "e2c3") == "Nec3", "file disambiguation (second)");

    // Disambiguation by rank: two rooks on the same file can both reach a3.
    Position r;
    r.setFen("4k3/8/8/8/R7/8/R7/3K4 w - - 0 1");
    CHECK(san(r, "a2a3") == "R2a3", "rank disambiguation");
    CHECK(san(r, "a4a3") == "R4a3", "rank disambiguation (second)");

    // Check and mate suffixes.
    Position c;
    c.setStartPosition();
    play(c, "f2f3");
    play(c, "e7e5");
    play(c, "g2g4");
    CHECK(san(c, "d8h4") == "Qh4#", "fool's mate suffix");
    play(c, "d8h4");
    CHECK(c.status() == GameStatus::CHECKMATE, "fool's mate detected");
}

static void testStatus() {
    Position p;
    p.setStartPosition();
    CHECK(p.status() == GameStatus::ONGOING, "start position is ongoing");

    Position stale;
    stale.setFen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    CHECK(stale.status() == GameStatus::STALEMATE, "stalemate detected");

    Position mat;
    mat.setFen("8/8/8/4k3/8/8/8/4K3 w - - 0 1");
    CHECK(mat.status() == GameStatus::INSUFFICIENT_MATERIAL, "K vs K is a draw");
    Position mat2;
    mat2.setFen("8/8/8/4k3/8/8/8/4KB2 w - - 0 1");
    CHECK(mat2.status() == GameStatus::INSUFFICIENT_MATERIAL, "K+B vs K is a draw");
    Position mat3;
    mat3.setFen("8/8/8/4k3/8/8/8/4KN2 w - - 0 1");
    CHECK(mat3.status() == GameStatus::INSUFFICIENT_MATERIAL, "K+N vs K is a draw");
    Position mat4;
    mat4.setFen("8/8/8/4k3/8/8/8/3QK3 w - - 0 1");
    CHECK(mat4.status() == GameStatus::ONGOING, "K+Q vs K keeps going");

    // En passant is generated and legal.
    Position ep;
    ep.setFen("4k3/8/8/8/3pP3/8/8/4K3 b - e3 0 1");
    bool epFound = false;
    for (const Move& m : ep.legalMoves())
        if (m.enPassant) epFound = true;
    CHECK(epFound, "en passant available");

    // Promotion moves: four choices, plus king moves.
    Position pr;
    pr.setFen("8/4P3/8/8/8/8/2k5/K7 w - - 0 1");
    int promos = 0;
    for (const Move& m : pr.legalMoves())
        if (m.promotion != NO_PIECE) ++promos;
    CHECK(promos == 4, "four promotion choices");

    // Threefold repetition.
    Position rep;
    rep.setStartPosition();
    for (int i = 0; i < 2; ++i) {
        play(rep, "g1f3");
        play(rep, "g8f6");
        play(rep, "f3g1");
        play(rep, "f6g8");
    }
    CHECK(rep.status() == GameStatus::THREEFOLD, "threefold repetition detected");

    // Undo restores the exact position.
    Position u;
    u.setStartPosition();
    std::string before = u.toFen();
    std::vector<Move> legal = u.legalMoves();
    UndoInfo ui;
    u.doMove(legal[0], ui);
    u.undoMove(legal[0], ui);
    CHECK(u.toFen() == before, "do/undo restores the position");
}

static void testEngine() {
    // The same LEVELS table the GUI and the Elo harness use, so this test
    // exercises the settings a real game actually runs at.
    for (int lvl = 0; lvl < 3; ++lvl) {
        Position p;
        p.setStartPosition();
        Engine e;
        auto t0 = std::chrono::steady_clock::now();
        SearchResult res = e.search(p, LEVELS[lvl].depth, LEVELS[lvl].time, LEVELS[lvl].noise, 42);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        std::printf("  %-6s move=%s%c%s score=%+d depth=%d nodes=%lld time=%.2fs\n", LEVELS[lvl].name,
                    squareName(res.move.from).c_str(), promoChar(res.move.promotion),
                    squareName(res.move.to).c_str(), res.score, res.depth, res.nodes, secs);
        CHECK(res.found, "engine returns a move");
        CHECK(res.depth >= 1, "engine completed at least one depth");
        CHECK(secs < LEVELS[lvl].time + 1.0, "engine respects its time budget");
        bool legal = false;
        for (const Move& m : p.legalMoves())
            if (m == res.move) legal = true;
        CHECK(legal, "engine move is legal");
    }

    // Mate in one: Rh8#.
    Position mate;
    mate.setFen("k7/8/1K6/8/8/8/8/7R w - - 0 1");
    Engine e1;
    SearchResult mr = e1.search(mate, 3, 2.0, 0, 7);
    std::printf("  mate-in-1: %s%s score=%+d mate=%d\n", squareName(mr.move.from).c_str(),
                squareName(mr.move.to).c_str(), mr.score, (int)mr.mate);
    CHECK(mr.found, "mate search returns a move");
    CHECK(mr.move.from == 7 && mr.move.to == 63, "engine finds Rh8#");
    CHECK(mr.mate && mr.score > 99000, "mate score reported");

    // Grab the undefended queen on a8.
    Position win;
    win.setFen("q3k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    Engine e2;
    SearchResult wr = e2.search(win, 3, 2.0, 0, 11);
    std::printf("  capture test: %s%s score=%+d depth=%d\n", squareName(wr.move.from).c_str(),
                squareName(wr.move.to).c_str(), wr.score, wr.depth);
    CHECK(wr.found, "capture search returns a move");
    CHECK(wr.move.from == 0 && wr.move.to == 56, "engine finds Rxa8");
    CHECK(wr.score > 300, "engine sees it wins the queen");

    // Cancel flag stops the search quickly.
    Position slow;
    slow.setStartPosition();
    std::atomic<bool> flag{false};
    Engine e3;
    e3.setCancelFlag(&flag);
    std::thread th([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        flag = true;
    });
    auto t0 = std::chrono::steady_clock::now();
    e3.search(slow, 6, 10.0, 0, 1);
    double stoppedIn = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    th.join();
    std::printf("  cancel: stopped after %.2fs\n", stoppedIn);
    CHECK(stoppedIn < 2.0, "cancel flag aborts the search");
}

// The ranking behind the best-move hints: every legal move scored in one rooted
// search and handed back strongest first.
static void testRankMoves() {
    Position p;
    p.setStartPosition();
    Engine e;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<ScoredMove> top = e.rankMoves(p, 2, 5.0);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::vector<Move> legal = p.legalMoves();
    bool sorted = true, allLegal = true;
    for (size_t i = 0; i < top.size(); ++i) {
        if (i > 0 && top[i - 1].score < top[i].score) sorted = false;
        bool found = false;
        for (const Move& m : legal)
            if (m == top[i].move) found = true;
        if (!found) allLegal = false;
    }
    std::printf("  start position: %zu of %zu moves ranked, best=%s%s %+d (d%d, %.2fs)\n",
                top.size(), legal.size(), top.empty() ? "?" : squareName(top[0].move.from).c_str(),
                top.empty() ? "" : squareName(top[0].move.to).c_str(),
                top.empty() ? 0 : top[0].score, top.empty() ? 0 : top[0].depth, secs);
    CHECK(top.size() == legal.size(), "every legal move is ranked");
    CHECK(allLegal, "ranked moves are legal");
    CHECK(sorted, "moves come back strongest first");
    CHECK(secs < 7.0, "ranking respects its time budget");

    // Mate in one must lead the list, carrying a mate score.
    Position mate;
    mate.setFen("k7/8/1K6/8/8/8/8/7R w - - 0 1");
    Engine e2;
    std::vector<ScoredMove> ranked = e2.rankMoves(mate, 3, 3.0);
    CHECK(!ranked.empty(), "mating position is ranked");
    if (!ranked.empty()) {
        std::printf("  mate ranking: first=%s%s %+d, %zu moves\n",
                    squareName(ranked[0].move.from).c_str(),
                    squareName(ranked[0].move.to).c_str(), ranked[0].score, ranked.size());
        CHECK(ranked[0].move.from == 7 && ranked[0].move.to == 63, "Rh8# ranks first");
        CHECK(ranked[0].score > MATE_BOUND, "the mating move carries a mate score");
    }
}

static void testClassification() {
    const int depth = 5;
    const double time = 3.0;
    Engine e;

    auto find = [](Position& p, const char* coord, Move& out) {
        int from = makeSquare(coord[0] - 'a', coord[1] - '1');
        int to = makeSquare(coord[2] - 'a', coord[3] - '1');
        PieceType promo = NO_PIECE;
        if (coord[4] == 'q') promo = QUEEN;
        else if (coord[4] == 'r') promo = ROOK;
        else if (coord[4] == 'b') promo = BISHOP;
        else if (coord[4] == 'n') promo = KNIGHT;
        for (const Move& m : p.legalMoves())
            if (m.from == from && m.to == to && m.promotion == promo) {
                out = m;
                return true;
            }
        return false;
    };

    // Smothered mate: Qxg8+!! Rxg8 Nf7#.  The queen goes for a rook and the
    // knight goes with it -- the archetypal Brilliant.
    Position sac;
    sac.setFen("5rrk/ppppp1pp/7N/3Q4/8/8/PPP2PPP/6K1 w - - 0 1");
    Move m;
    CHECK(find(sac, "d5g8", m), "Qxg8+ is legal");
    Assessment a = e.assess(sac, m, depth, time);
    MoveTag tag = classifyMove(a);
    std::printf("  Qxg8+!: best=%+d played=%+d sac=%+d -> %s\n", a.best, a.played, a.sacNet, tagName(tag));
    CHECK(a.ok, "assessment completes");
    CHECK(a.played >= MATE_BOUND, "queen sac is seen as a forced mate");
    CHECK(a.sacNet >= SAC_MIN, "the sac really does give material away");
    CHECK(tag == MoveTag::BRILLIANT, "forcing queen sac is Brilliant");

    // Walking away from the forced mate: the knight on h6 is loose, so this is
    // not merely slower, it throws the advantage away as well.
    CHECK(find(sac, "d5d1", m), "Qd1 is legal");
    a = e.assess(sac, m, depth, time);
    tag = classifyMove(a);
    std::printf("  Qd1:    best=%+d played=%+d -> %s\n", a.best, a.played, tagName(tag));
    CHECK(tag == MoveTag::BLUNDER, "giving up a forced mate and the knight is a Blunder");

    // A free rook sits on h6 behind the g5 pawn: taking it is clearly best, but
    // a quiet move still leaves White a queen for a rook up -- a win missed,
    // not a position ruined.
    Position missed;
    missed.setFen("6k1/pppppp1p/7r/6P1/8/8/PPPPPP1P/3Q2K1 w - - 0 1");
    CHECK(find(missed, "g5h6", m), "gxh6 is legal");
    a = e.assess(missed, m, depth, time);
    CHECK(a.best >= 300, "winning the rook is worth more than three pawns");
    CHECK(find(missed, "a2a3", m), "a3 is legal");
    a = e.assess(missed, m, depth, time);
    tag = classifyMove(a);
    std::printf("  a3:     best=%+d played=%+d -> %s\n", a.best, a.played, tagName(tag));
    CHECK(a.played >= 0, "quiet move keeps White better");
    CHECK(a.best - a.played > 120, "but it hands back a whole rook");
    CHECK(tag == MoveTag::MISSED_WIN, "leaving a free rook is Missed Win");

    // Throw away a whole queen in an otherwise ordinary position.
    Position loose;
    loose.setFen("3k4/8/8/3q4/8/2N5/8/K7 w - - 0 1");
    CHECK(find(loose, "c3d5", m), "Nxd5 is legal");
    a = e.assess(loose, m, depth, time);
    tag = classifyMove(a);
    std::printf("  Nxd5:   best=%+d played=%+d -> %s\n", a.best, a.played, tagName(tag));
    CHECK(a.played >= a.best - 5, "Nxd5 is the strongest move");
    CHECK(tag == MoveTag::BEST, "winning the queen is Best");
    CHECK(a.sacNet < SAC_MIN, "taking a loose queen outright is no sacrifice");

    // An even trade is not a sacrifice either: the knights swap and neither
    // side has given anything away.
    Position trade;
    trade.setFen("3k4/8/8/3n4/8/2N5/8/K7 w - - 0 1");
    CHECK(find(trade, "c3d5", m), "Nxd5 is legal in the trade position");
    a = e.assess(trade, m, depth, time);
    tag = classifyMove(a);
    std::printf("  Nxd5 trade: best=%+d played=%+d sac=%+d -> %s\n", a.best, a.played,
                a.sacNet, tagName(tag));
    CHECK(a.sacNet < SAC_MIN, "swapping knights gives nothing away");
    CHECK(tag != MoveTag::BRILLIANT, "an even trade is never Brilliant");

    // Same trade, but the side that recaptures *can* go on: the queen may take
    // back on d2, it just loses itself to the c1 bishop. Nobody has to play
    // that, so the exchange settles as an even one.
    Position poison;
    poison.setFen("3q4/8/8/8/1b6/5N2/3N4/6K1 b - - 0 1");
    CHECK(find(poison, "b4d2", m), "Bxd2 is legal in the poisoned position");
    a = e.assess(poison, m, depth, time);
    tag = classifyMove(a);
    std::printf("  Bxd2:   best=%+d played=%+d sac=%+d -> %s\n", a.best, a.played, a.sacNet,
                tagName(tag));
    CHECK(a.sacNet < SAC_MIN, "a recapture nobody is forced to make is no sacrifice");

    CHECK(find(loose, "a1b1", m), "Kb1 is legal");
    a = e.assess(loose, m, depth, time);
    tag = classifyMove(a);
    std::printf("  Kb1:    best=%+d played=%+d loss=%+d -> %s\n", a.best, a.played,
                a.best - a.played, tagName(tag));
    CHECK(a.best - a.played > 250, "walking away from the queen loses a lot");
    CHECK(tag == MoveTag::BLUNDER, "losing the win like that is a Blunder");

    // A move with no alternative is not judged at all.
    Position forced;
    forced.setFen("7k/8/8/8/8/8/6q1/7K w - - 0 1");
    CHECK(find(forced, "h1g2", m), "Kxg2 is the reply to the check");
    a = e.assess(forced, m, depth, time);
    std::printf("  forced: legal=%d -> %s\n", a.legalCount, tagName(classifyMove(a)));
    CHECK(a.legalCount == 1, "only one legal move in the forced position");
    CHECK(classifyMove(a) == MoveTag::NONE, "a forced move is not judged");
}

int main() {
    std::printf("engine_test\n");
    std::printf("- SAN and moves\n");
    testSan();
    std::printf("- status rules\n");
    testStatus();
    std::printf("- search\n");
    testEngine();
    std::printf("- best-move ranking\n");
    testRankMoves();
    std::printf("- move classification\n");
    testClassification();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
