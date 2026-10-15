// tests/debug.cpp -- prints a position's legal moves with SAN and a search score for each
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/engine.h"
#include "../src/position.h"

int main(int argc, char** argv) {
    std::string fen = argc > 1 ? argv[1] : START_FEN;
    int depth = argc > 2 ? std::atoi(argv[2]) : 3;

    Position p;
    p.setFen(fen);
    std::vector<Move> moves = p.legalMoves();
    std::printf("fen:   %s\n", p.toFen().c_str());
    std::printf("side:  %s, %zu legal moves\n\n", p.side == WHITE ? "white" : "black", moves.size());

    for (const Move& m : moves) {
        std::string s = p.toSan(m);
        Position child = p;
        UndoInfo u;
        child.doMove(m, u);
        Engine e;
        SearchResult r = e.search(child, depth, 2.0, 0, 1);
        std::printf("  %s%s  san=%-8s score=%+7d depth=%d\n", squareName(m.from).c_str(),
                    squareName(m.to).c_str(), s.c_str(), -r.score, r.depth);
    }
    return 0;
}
