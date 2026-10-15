// tests/rootscore.cpp -- prints the root score of every move at a fixed depth
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/engine.h"
#include "../src/position.h"

int main(int argc, char** argv) {
    std::string fen = argc > 1 ? argv[1] : START_FEN;
    int depth = argc > 2 ? std::atoi(argv[2]) : 3;

    Position root;
    root.setFen(fen);
    Engine e;
    // Reproduce the root loop of Engine::search with full-window scoring.
    std::vector<Move> moves = root.legalMoves();
    std::printf("fen: %s  depth=%d\n", root.toFen().c_str(), depth);
    for (const Move& m : moves) {
        UndoInfo u;
        root.doMove(m, u);
        int s = -e.search(root, depth - 1, 5.0, 0, 1).score;
        root.undoMove(m, u);
        std::printf("  %s%s san=%-8s score=%+7d\n", squareName(m.from).c_str(),
                    squareName(m.to).c_str(), root.toSan(m).c_str(), s);
    }
    return 0;
}
