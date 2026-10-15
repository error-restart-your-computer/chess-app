#pragma once
// levels.h -- the three difficulty levels the app plays at.
//
// Shared by the GUI (main.cpp) and the headless Elo harness (tests/elo_test.cpp)
// so that the strength measurements are made with exactly the settings a real
// game uses. Each level configures both engines: the built-in search runs to
// `depth` within `time` seconds with `noise` centipawns of randomness on the
// scores (0 = fully deterministic), while Stockfish gets a depth/movetime
// budget and a number of threads.

struct LevelCfg {
    const char* name;
    // built-in engine
    int depth;
    double time;
    int noise;
    // Stockfish: depth/movetime whichever runs out first, and how many of the
    // machine's cores to hand it.
    int sfDepth;
    int sfTimeMs;
    int sfThreads;
};

static const LevelCfg LEVELS[3] = {
    // `depth` is a ceiling, not a target: Easy is a fixed shallow ply, while
    // Medium and Hard let the iterative deepening climb until the clock runs
    // out (null-move pruning, reductions and the transposition table now buy
    // several plies inside the same budgets) and keep the deepest pass that
    // completed.
    {"Easy", 1, 0.50, 60, 6, 400, 1},
    {"Medium", 8, 1.20, 15, 14, 1000, 2},
    {"Hard", 14, 3.00, 0, 22, 2800, 4},
};
