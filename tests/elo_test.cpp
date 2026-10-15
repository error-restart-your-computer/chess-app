// tests/elo_test.cpp -- rate the built-in bot against Stockfish's UCI_Elo ladder.
//
// What is measured
//   The subject is the app's own search, `Engine` (src/engine.cpp), driven at
//   exactly the settings the GUI plays with -- the shared LEVELS table from
//   src/levels.h (Easy / Medium / Hard). No Stockfish anywhere on the bot's
//   side of the board.
//
// What it is measured against
//   Stockfish 19 (in-tree) understands UCI_LimitStrength + UCI_Elo: asking for
//   a rating in 1320..3190 switches on its Skill machinery, which replaces the
//   best root move with a deliberately weaker one picked by a formula fitted
//   to games between Stockfish and the Stash engine on the CCRL-blitz scale.
// That is a ladder of fixed, externally calibrated anchor strengths: each
// anchor gets a fixed batch of games with colours alternating, and one rating
// per level is fitted across all of them.
//
// Method
//   * Every (level, anchor) pair plays --games games from a fixed opening book
//     (eight main lines, five plies), colours alternating inside the pair.
//   * Stockfish searches to a depth that always passes its anchor's skill pick
//     (the pick happens at depth 1 + int(level); we ask for two plies beyond),
//     so a weak anchor really plays the weakness it claims instead of running
//     out of search first. A movetime guard is the backstop.
//   * One rating per level fits all of that level's game points by maximum
//     likelihood under the standard logistic curve
//         P(bot scores a point vs anchor A) = 1 / (1 + 10^((A - E) / 400))
//     with draws worth 1/2. The log-likelihood is concave in E, so bisecting
//     its derivative finds the unique optimum, and the Fisher information at
//     the fit gives the standard error printed next to it.
//
// Run it from the project root (once `make stockfish` has produced the engine):
//     build/elo_test                                    # full run
//     build/elo_test --games 4 --anchors 1600,1900      # quick slice
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/engine.h"
#include "../src/levels.h"
#include "../src/position.h"
#include "../src/uci.h"

namespace {

// ---------------------------------------------------------------- settings --

struct Cfg {
    int games = 16;       // per (level, anchor) pair; even = colour-balanced
    int workers = 0;      // 0 = auto: one worker per two cores (bot + Stockfish)
    int movetime = 3000;  // Stockfish per-move guard in ms (depth cap binds first)
    int maxPlies = 240;   // adjudicate as a draw past this many half-moves
    std::vector<int> anchors = {1320, 1600, 1900, 2200, 2500};
    std::string sfPath;
};

// Fixed opening book: eight main lines of five plies. The line follows the
// game index, so every cell of the result table sees the same openings in the
// same order and stays directly comparable.
constexpr int BOOK_LINES = 8;
constexpr int BOOK_PLIES = 5;
const char* const BOOK[BOOK_LINES][BOOK_PLIES] = {
    {"e2e4", "e7e5", "g1f3", "b8c6", "f1b5"},  // Ruy Lopez
    {"e2e4", "c7c5", "g1f3", "d7d6", "d2d4"},  // Sicilian, Najdorf move order
    {"d2d4", "g8f6", "c2c4", "e7e6", "b1c3"},  // Queen's Indian / Nimzo order
    {"d2d4", "d7d5", "c2c4", "e7e6", "b1c3"},  // Queen's Gambit Declined
    {"c2c4", "e7e5", "g1f3", "b8c6", "b1c3"},  // English, Four Knights
    {"g1f3", "d7d5", "d2d4", "g8f6", "c2c4"},  // Catalan move order
    {"e2e4", "e7e6", "d2d4", "d7d5", "b1c3"},  // French, classical
    {"e2e4", "c7c6", "d2d4", "d7d5", "b1c3"},  // Caro-Kann, classical
};

// Stockfish's own conversion of a requested Elo into a skill level, verbatim
// from stockfish/src/search.h: the 1320..3190 range is a cubic fit of games
// between Stockfish at various skill levels and the Stash engine.
double sfSkillLevel(int elo) {
    const double e = double(elo - 1320) / (3190 - 1320);
    const double lvl = ((37.2473 * e - 40.8525) * e + 22.2943) * e - 0.311438;
    return std::min(19.0, std::max(0.0, lvl));
}

// The skill pick happens exactly at search depth 1 + int(level). Asking for
// two plies past it means every anchor reaches its calibrated pick under our
// budget: weak anchors stop almost immediately (and cheaply), strong ones get
// the deeper root scores the label promises. The movetime guard only ever
// fires on pathological positions.
int sfDepthFor(int elo) {
    return std::min(25, (int)std::floor(sfSkillLevel(elo)) + 3);
}

// ------------------------------------------------------------------- games --

struct Task {
    int anchor = 0;
    int level = 0;
    int game = 0;
};

struct GameResult {
    char res = 'D';       // the bot's score: 'W', 'D' or 'L'
    bool botWhite = true;
    bool voidGame = false;  // an engine failed; not counted in the statistics
    int plies = 0;
    double seconds = 0.0;
};

bool containsMove(const std::vector<Move>& legal, const Move& m) {
    return std::find(legal.begin(), legal.end(), m) != legal.end();
}

// Colours alternate from game to game, and a second pass through the book
// (game >= BOOK_LINES) flips them, so the same opening is seen from both
// sides of the pairing instead of replaying an identical game.
bool botIsWhite(int game) {
    return ((game / BOOK_LINES) + game) % 2 == 0;
}

// Play one game: the built-in Engine on the bot's side (at LEVELS[level], the
// app's real settings) and Stockfish pinned to `anchor` on the other.
GameResult playGame(const Task& t, const Cfg& cfg, Engine& bot, Stockfish& sf) {
    GameResult gr;
    gr.botWhite = botIsWhite(t.game);
    const Color botColor = gr.botWhite ? WHITE : BLACK;
    const LevelCfg& lv = LEVELS[t.level];
    const auto t0 = std::chrono::steady_clock::now();

    Position pos;
    pos.setStartPosition();
    for (int i = 0; i < BOOK_PLIES; ++i) {
        Move m;
        if (!Stockfish::parseUci(pos, BOOK[t.game % BOOK_LINES][i], m)) break;
        UndoInfo u;
        pos.doMove(m, u);
    }
    gr.plies = 0;

    const uint64_t seed =
        0xE10ULL * (uint64_t)(t.game + 1) + (uint64_t)t.level * 7 + 1;

    for (;;) {
        const std::vector<Move> legal = pos.legalMoves();
        const GameStatus st = pos.status(legal);
        if (st != GameStatus::ONGOING) {
            if (st == GameStatus::CHECKMATE)
                gr.res = (pos.side == botColor) ? 'L' : 'W';  // side to move is mated
            else
                gr.res = 'D';
            break;
        }
        if (gr.plies >= cfg.maxPlies) {  // endless shuffle: call it a draw
            gr.res = 'D';
            break;
        }

        SearchResult r;
        if (pos.side == botColor) {
            // The app's own bot: same depth/time/noise, a seed that differs
            // per game so the noise is reproducible run to run.
            r = bot.search(pos, lv.depth, lv.time, lv.noise, seed);
        } else {
            if (!sf.search(pos, sfDepthFor(t.anchor), cfg.movetime, nullptr, r) || !r.found) {
                gr.voidGame = true;  // the engine died or never answered
                break;
            }
        }

        Move chosen;
        if (r.found && containsMove(legal, r.move)) {
            chosen = r.move;
        } else {  // the built-in side produced nothing usable: any legal move
            chosen = legal.front();
        }
        UndoInfo u;
        pos.doMove(chosen, u);
        ++gr.plies;
    }

    gr.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return gr;
}

// Pin one Stockfish instance to an anchor, restarting it if it has gone away.
// Returns false when the engine cannot be brought up at all.
bool ensureAnchor(Stockfish& sf, const Cfg& cfg, int anchor, int& setAnchor) {
    if (setAnchor == anchor && sf.alive()) return true;
    if (!sf.alive()) {
        sf.quit();  // drop whatever half state is left
        if (!sf.start(cfg.sfPath, 1, 32)) return false;
    }
    if (!sf.setOption("UCI_LimitStrength", "true")) return false;
    if (!sf.setOption("UCI_Elo", std::to_string(anchor))) return false;
    setAnchor = anchor;
    return true;
}

// ------------------------------------------------------------- statistics ----

// One game point: the anchor it was played at and what the bot scored.
struct Point {
    int anchor;
    double y;
};

double winProb(double elo, int anchor) {
    return 1.0 / (1.0 + std::pow(10.0, (anchor - elo) / 400.0));
}

// d/dE of the log-likelihood, up to the positive factor ln(10)/400: draws
// count as half a point, exactly as in a match score.
double scoreSlope(double elo, const std::vector<Point>& pts) {
    double g = 0.0;
    for (const Point& pt : pts) {
        const double p = winProb(elo, pt.anchor);
        g += (pt.y - p) * p * (1.0 - p);
    }
    return g;
}

struct Fit {
    double elo = 0.0;
    double se = -1.0;  // <0: not computable
    bool belowLadder = false;
    bool aboveLadder = false;
    int n = 0;
};

// Maximum-likelihood rating over every game point of one level. The
// likelihood is concave in E, so bisection on its derivative converges to the
// unique optimum. `se` comes from the Fisher information at that point.
Fit fitElo(const std::vector<Point>& pts) {
    Fit f;
    f.n = (int)pts.size();
    if (pts.empty()) return f;

    const double lo = -1000.0, hi = 5000.0;
    const double sLo = scoreSlope(lo, pts), sHi = scoreSlope(hi, pts);
    if (sLo <= 0.0) {
        f.elo = lo;  // scored less than the whole ladder can explain
        f.belowLadder = true;
    } else if (sHi >= 0.0) {
        f.elo = hi;
        f.aboveLadder = true;
    } else {
        double a = lo, b = hi;
        for (int it = 0; it < 80; ++it) {
            const double mid = 0.5 * (a + b);
            if (scoreSlope(mid, pts) > 0.0)
                a = mid;
            else
                b = mid;
        }
        f.elo = 0.5 * (a + b);
    }

    double info = 0.0;
    for (const Point& pt : pts) {
        const double p = winProb(f.elo, pt.anchor);
        info += p * (1.0 - p);
    }
    if (info > 0.0)
        f.se = (400.0 / std::log(10.0)) / std::sqrt(info);
    return f;
}

struct Cell {
    int w = 0, d = 0, l = 0, n = 0;  // n = counted games (w + d + l)
};

// ------------------------------------------------------------------ driver --

struct Options {
    bool help = false;
    Cfg cfg;
};

bool parseInts(const std::string& csv, std::vector<int>& out) {
    out.clear();
    size_t i = 0;
    while (i <= csv.size()) {
        size_t j = csv.find(',', i);
        if (j == std::string::npos) j = csv.size();
        const std::string tok = csv.substr(i, j - i);
        if (tok.empty()) return false;
        out.push_back(std::atoi(tok.c_str()));
        i = j + 1;
    }
    return !out.empty();
}

Options parseArgs(int argc, char** argv) {
    Options o;
    const char* envSf = std::getenv("STOCKFISH");
    o.cfg.sfPath = envSf && *envSf ? envSf : "bin/stockfish";

    auto next = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "elo_test: %s needs a value\n", argv[i]);
            std::exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") o.help = true;
        else if (a == "--games") o.cfg.games = std::atoi(next(i));
        else if (a == "--workers") o.cfg.workers = std::atoi(next(i));
        else if (a == "--movetime") o.cfg.movetime = std::atoi(next(i));
        else if (a == "--max-plies") o.cfg.maxPlies = std::atoi(next(i));
        else if (a == "--sf") o.cfg.sfPath = next(i);
        else if (a == "--anchors") {
            if (!parseInts(next(i), o.cfg.anchors)) {
                std::fprintf(stderr, "elo_test: bad --anchors list\n");
                std::exit(2);
            }
        } else {
            std::fprintf(stderr, "elo_test: unknown option '%s' (try --help)\n", a.c_str());
            std::exit(2);
        }
    }
    return o;
}

void usage() {
    std::printf(
        "usage: build/elo_test [options]\n"
        "  --games N        games per (level, anchor) pair [16]\n"
        "  --anchors a,b,c  Stockfish UCI_Elo anchors, 1320..3190 [1320,1600,1900,2200,2500]\n"
        "  --workers N      parallel workers; 0 = one per two cores [0]\n"
        "  --movetime MS    Stockfish per-move guard in ms [3000]\n"
        "  --max-plies N    adjudicate a draw after N half-moves [240]\n"
        "  --sf PATH        Stockfish binary [$STOCKFISH or bin/stockfish]\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options opt = parseArgs(argc, argv);
    if (opt.help) {
        usage();
        return 0;
    }
    Cfg& cfg = opt.cfg;
    if (cfg.games < 1 || cfg.movetime < 50 || cfg.maxPlies < 20) {
        std::fprintf(stderr, "elo_test: --games/--movetime/--max-plies out of range\n");
        return 2;
    }
    for (int a : cfg.anchors) {
        if (a < 1320 || a > 3190) {
            std::fprintf(stderr, "elo_test: anchor %d outside Stockfish's UCI_Elo range 1320..3190\n", a);
            return 2;
        }
    }

    // Preflight: the engine must come up and take a strength setting.
    {
        Stockfish probe;
        if (!probe.start(cfg.sfPath, 1, 32)) {
            std::fprintf(stderr,
                         "elo_test: cannot start Stockfish at '%s' -- run `make stockfish` first\n",
                         cfg.sfPath.c_str());
            return 2;
        }
        if (!probe.setOption("UCI_LimitStrength", "true") ||
            !probe.setOption("UCI_Elo", std::to_string(cfg.anchors.front()))) {
            std::fprintf(stderr, "elo_test: %s did not accept UCI_Elo\n", probe.engineName().c_str());
            return 2;
        }
        std::printf("stockfish: %s\n", probe.engineName().c_str());
    }

    const int levels = (int)(sizeof(LEVELS) / sizeof(LEVELS[0]));
    if (cfg.workers <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        cfg.workers = std::max(1, (int)(hw ? hw : 4) / 2);
    }
    cfg.workers = std::min(cfg.workers, 16);

    // One task per (anchor, level, game); anchor-major so a worker usually
    // stays parked on one strength setting and rarely has to re-pin it.
    std::vector<Task> tasks;
    for (int anchor : cfg.anchors)
        for (int lvl = 0; lvl < levels; ++lvl)
            for (int g = 0; g < cfg.games; ++g) tasks.push_back({anchor, lvl, g});

    std::printf("levels:  ");
    for (int lvl = 0; lvl < levels; ++lvl)
        std::printf("  %s (depth %d, %.2fs, noise %dp)", LEVELS[lvl].name, LEVELS[lvl].depth,
                    LEVELS[lvl].time, LEVELS[lvl].noise);
    std::printf("\nanchors:");
    for (int a : cfg.anchors) std::printf(" %d", a);
    std::printf("   book: 8 lines / 5 plies, colours alternating\n");
    std::printf("plan: %d games, %d workers, Stockfish guard %dms, draw cap %d plies\n\n",
                (int)tasks.size(), cfg.workers, cfg.movetime, cfg.maxPlies);
    std::fflush(stdout);

    std::vector<GameResult> out(tasks.size());
    std::atomic<int> next{0}, done{0};
    std::mutex printMu;
    const auto t0 = std::chrono::steady_clock::now();

    auto worker = [&]() {
        Engine bot;
        Stockfish sf;
        int setAnchor = -1;
        for (;;) {
            const int i = next.fetch_add(1);
            if (i >= (int)tasks.size()) break;
            const Task& t = tasks[i];

            GameResult gr;
            if (!ensureAnchor(sf, cfg, t.anchor, setAnchor)) {
                gr.voidGame = true;
                gr.botWhite = botIsWhite(t.game);
                if (!sf.alive()) setAnchor = -1;
            } else {
                gr = playGame(t, cfg, bot, sf);
            }
            out[i] = gr;

            const int finished = done.fetch_add(1) + 1;
            const char* verdict =
                gr.voidGame ? "void" : gr.res == 'W' ? "win " : gr.res == 'L' ? "loss" : "draw";
            std::lock_guard<std::mutex> g(printMu);
            std::printf("[%3d/%3d] anchor %-4d %-6s bot %-5s %s (%d plies, %2.0fs)\n", finished,
                        (int)tasks.size(), t.anchor, LEVELS[t.level].name,
                        gr.botWhite ? "White" : "Black", verdict, gr.plies, gr.seconds);
            std::fflush(stdout);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve((size_t)cfg.workers);
    for (int w = 0; w < cfg.workers; ++w) pool.emplace_back(worker);
    for (std::thread& th : pool) th.join();

    const double minutes =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 60.0;

    // ------------------------------------------------------------- report ----
    std::vector<std::vector<Cell>> cell((size_t)levels, std::vector<Cell>(cfg.anchors.size()));
    std::vector<std::vector<Point>> pts((size_t)levels);
    int voidGames = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const GameResult& gr = out[i];
        const Task& t = tasks[i];
        if (gr.voidGame) {
            ++voidGames;
            continue;
        }
        size_t col = 0;
        for (size_t c = 0; c < cfg.anchors.size(); ++c)
            if (cfg.anchors[c] == t.anchor) col = c;
        Cell& cl = cell[(size_t)t.level][col];
        if (gr.res == 'W') ++cl.w;
        else if (gr.res == 'L') ++cl.l;
        else ++cl.d;
        ++cl.n;
        pts[(size_t)t.level].push_back(
            {t.anchor, gr.res == 'W' ? 1.0 : gr.res == 'L' ? 0.0 : 0.5});
    }

    std::printf("\n==== built-in bot vs Stockfish UCI_Elo anchors ====\n");
    std::printf("cells: the bot's W-D-L, points%% (win 1, draw 1/2); %d games counted, %d void\n",
                (int)tasks.size() - voidGames, voidGames);
    std::printf("  %-8s", "level");
    for (int a : cfg.anchors) std::printf("  vs %-10d", a);
    std::printf("\n");
    for (int lvl = 0; lvl < levels; ++lvl) {
        std::printf("  %-8s", LEVELS[lvl].name);
        for (size_t c = 0; c < cfg.anchors.size(); ++c) {
            const Cell& cl = cell[(size_t)lvl][c];
            if (cl.n == 0)
                std::printf("  %-6s %5s", "--", "--");
            else {
                char wdl[16];
                std::snprintf(wdl, sizeof(wdl), "%d-%d-%d", cl.w, cl.d, cl.l);
                std::printf("  %-6s %5.1f%%", wdl, 100.0 * (cl.w + 0.5 * cl.d) / cl.n);
            }
        }
        std::printf("\n");
    }

    std::printf("\nfitted rating per level (all anchors pooled, draws = 1/2 point):\n");
    for (int lvl = 0; lvl < levels; ++lvl) {
        const Fit f = fitElo(pts[(size_t)lvl]);
        if (f.n == 0) {
            std::printf("  %-8s no games\n", LEVELS[lvl].name);
            continue;
        }
        if (f.belowLadder || f.aboveLadder) {
            // The scores are too one-sided for any point on the ladder; the
            // standard error would be meaningless, so print the bound instead.
            std::printf("  %-8s %s the %d..%d ladder (one-sided scores, n=%d)\n",
                        LEVELS[lvl].name, f.belowLadder ? "below" : "above", 1320, 3190, f.n);
            continue;
        }
        std::printf("  %-8s %4.0f +- %.0f Elo", LEVELS[lvl].name, f.elo, f.se);
        if (f.se > 0)
            std::printf("  (95%% CI %.0f..%.0f)", f.elo - 1.96 * f.se, f.elo + 1.96 * f.se);
        std::printf("  n=%d\n", f.n);
    }

    std::printf("\nnote: Stockfish's UCI_Elo is its own CCRL-blitz-style calibration\n"
                "      (fitted against the Stash engine), so treat it as a well-founded\n"
                "      reference, not a tournament-certified rating.\n");
    std::printf("run: %.1f minutes on %d workers\n", minutes, cfg.workers);
    return 0;
}
