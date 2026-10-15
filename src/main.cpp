// main.cpp -- SDL2 chess application: human vs AI or two players on one board.
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "aiwatch.h"
#include "art.h"
#include "engine.h"
#include "levels.h"
#include "pieces.h"
#include "position.h"
#include "screen.h"
#include "text.h"
#include "uci.h"
#include "xoverlay.h"

// ---------------------------------------------------------------- layout ----
static const int SQ = 88;
static const int BOARD_X = 16;
static const int BOARD_Y = 16;
static const int BOARD_PX = SQ * 8;
static const int PANEL_X = BOARD_X + BOARD_PX + 16;
static const int PANEL_W = 300;
static const int WIN_W = PANEL_X + PANEL_W + 16;
static const int WIN_H = BOARD_Y + BOARD_PX + 16;

static const SDL_Color CLR_BG = {24, 25, 28, 255};
static const SDL_Color CLR_LIGHT = {238, 230, 206, 255};
static const SDL_Color CLR_DARK = {118, 150, 98, 255};
static const SDL_Color CLR_PANEL = {32, 34, 38, 255};
static const SDL_Color CLR_TEXT = {224, 224, 224, 255};
static const SDL_Color CLR_DIM = {146, 148, 154, 255};
static const SDL_Color CLR_HL_LAST = {186, 202, 68, 190};
static const SDL_Color CLR_HL_SEL = {246, 246, 105, 210};
static const SDL_Color CLR_HL_HOVER = {255, 255, 255, 40};
static const SDL_Color CLR_CHECK = {231, 76, 60, 175};
static const SDL_Color CLR_ACCENT = {126, 200, 120, 255};
static const SDL_Color CLR_WARN = {235, 120, 90, 255};
static const SDL_Color CLR_BTN = {255, 255, 255, 20};
static const SDL_Color CLR_BTN_H = {255, 255, 255, 46};
static const SDL_Color CLR_BORDER = {255, 255, 255, 40};

// Badge colours, in the spirit of chess.com's move classification.
static SDL_Color tagColor(MoveTag t) {
    switch (t) {
        case MoveTag::BRILLIANT:  return SDL_Color{38, 194, 163, 255};   // teal
        case MoveTag::GREAT:      return SDL_Color{74, 184, 96, 255};    // green
        case MoveTag::BEST:       return SDL_Color{129, 182, 76, 255};   // leaf green
        case MoveTag::EXCELLENT:  return SDL_Color{154, 192, 120, 255};  // light green
        case MoveTag::GOOD:       return SDL_Color{170, 186, 150, 255};  // sage
        case MoveTag::INACCURACY: return SDL_Color{255, 198, 88, 255};   // yellow
        case MoveTag::MISTAKE:    return SDL_Color{250, 138, 52, 255};   // orange
        case MoveTag::BLUNDER:    return SDL_Color{236, 78, 78, 255};    // red
        case MoveTag::MISSED_WIN: return SDL_Color{156, 164, 194, 255};  // steel
        default:                  return CLR_DIM;
    }
}

// How deeply moves are judged (fixed, independent of the playing level).
static const int ANALYZE_DEPTH = 5;   // built-in engine
static const double ANALYZE_TIME = 2.5;
static const int JUDGE_DEPTH = 16;    // Stockfish
static const int JUDGE_TIME_MS = 1000;
static const int JUDGE_THREADS = 2;
static const int JUDGE_HASH_MB = 64;

// Best-move hints: how many of the strongest moves to show, and how hard a
// third engine instance looks for them (built-in engine when there is none).
static const int HINT_MAX = 5;
static const int HINT_DEPTH = 16;
static const int HINT_TIME_MS = 700;
static const int HINT_THREADS = 1;
static const int HINT_HASH_MB = 32;

// Watching a board somewhere else on the screen: how often the screen is read
// and how hard the advice is searched for (a fourth engine instance).
static const int OV_DEPTH = 16;
static const int OV_TIME_MS = 600;
static const int OV_THREADS = 1;
static const int OV_HASH_MB = 32;
static const int OV_TAKE_MS = 450;        // between frames, when the root works
static const int OV_TAKE_TOOL_MS = 250;   // ... and when a screenshot tool is needed: a
                                          // locked board costs only the capture to look
                                          // at, so this plus the tool's own time puts a
                                          // look about every second, which is what is
                                          // asked for
// Four colour blocks drawn in our own window's bottom margin while the overlay
// runs: window positions are guesses under Wayland, this stripe is not.
static const SDL_Rect OV_MARKER = {WIN_W - 40, WIN_H - 14, 24, 6};

// Rank colours for the hints: the strongest move burns brightest, the fifth is
// the faintest, so the ranked list and its arrow are told apart at a glance.
static SDL_Color hintColor(int rank) {
    static const SDL_Color cols[HINT_MAX] = {
        {126, 200, 120, 235},  // accent green
        {96, 184, 214, 205},   // sky
        {240, 198, 92, 185},   // gold
        {176, 158, 224, 165},  // violet
        {232, 132, 112, 145},  // clay
    };
    return cols[std::min(std::max(rank, 0), HINT_MAX - 1)];
}

enum BtnId {
    BTN_NEW,
    BTN_UNDO,
    BTN_AI,
    BTN_LEVEL,
    BTN_FLIP,
    BTN_FEN,
    BTN_PAUSE,
    BTN_SPEED,
    BTN_HINTS,
    BTN_TOPN,
    BTN_OVERLAY,
    BTN_SETUP,
    BTN_NONE
};

// Watch mode: breath between engine moves, and the piece slide duration.
static const int PACES[3] = {1100, 650, 320};
static const char* PACE_NAMES[3] = {"Slow", "Normal", "Fast"};
static const Uint32 ANIM_MS = 190;
// Judgements are queued, but only this many at once: if the player races ahead
// of the analyst the oldest unfinished ones are dropped rather than piled up.
static const size_t AN_QUEUE_MAX = 8;

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    TextRenderer text;

    Position pos;
    std::vector<std::pair<Move, UndoInfo>> stack;
    std::vector<std::string> sans;
    std::vector<Move> legal;
    GameStatus status = GameStatus::ONGOING;
    int lastFrom = -1, lastTo = -1;

    // interaction
    int sel = -1;
    std::vector<Move> selMoves;
    bool dragging = false;
    int dragFrom = -1;
    SDL_Point mouse = {0, 0};
    bool promoOpen = false;
    int promoFrom = -1, promoTo = -1;
    bool flipped = false;
    int scroll = 0;

    // engine
    bool aiEnabled = true;
    Color aiColor = BLACK;
    int level = 1;
    bool aiBoth = false;      // AI vs AI (watch mode)
    bool paused = false;      // engine held back
    int pace = 1;             // index into PACES: delay between engine moves
    Uint32 aiHoldUntil = 0;   // engine waits for this tick before moving
    Uint32 gameOverAt = 0;    // when the current game ended (0 while ongoing)

    // move animation
    bool animate = true;
    bool snapshot = false;   // single-frame CLI snapshots: no easing, no banner
    Uint32 animAt = 0;
    int animFrom = -1, animTo = -1;
    Piece animPiece = 0;
    int animRookFrom = -1, animRookTo = -1;
    Piece animRookPiece = 0;

    // evaluation bar (White's point of view, centipawns; mate scores kept as-is)
    int evalCp = 0;
    double dispEval = 0.0;

    // move classification (chess.com-style badges next to the SAN)
    std::vector<MoveTag> tags;   // parallel to sans, NONE until judged
    bool analyze = true;         // judge moves as they are played
    bool analyzeSync = false;    // judge inline (used by --snap --analyze)
    bool anBusy = false;         // a judgement is still in flight
    int anGen = 0;               // results from before an undo/new game are dropped
    MoveTag toastTag = MoveTag::NONE;  // badge currently floating over the board
    std::string toastSan;
    Uint32 toastAt = 0;

    std::atomic<bool> anQuit{false};
    std::thread anThread;
    std::mutex anMutex;
    std::condition_variable anCv;
    struct AnJob {
        Position before;
        Move move;
        std::string san;
        int ply = -1;
        int gen = 0;
    };
    struct AnRes {
        int gen = -1;
        int ply = -1;
        MoveTag tag = MoveTag::NONE;
        std::string san;
    };
    std::vector<AnJob> anQueue;  // waiting for the worker
    bool anActive = false;       // worker is judging right now
    std::vector<AnRes> anDone;   // judged, waiting for the main thread

    std::atomic<bool> aiCancel{false};
    std::atomic<int> aiGen{0};
    std::thread aiThread;
    bool aiThinking = false;
    std::mutex aiMutex;
    struct AiOut {
        bool ready = false;
        int gen = -1;
        Move move;
        std::string info;
        int score = 0;
        bool mate = false;
    } aiOut;
    std::string engineInfo;
    std::string engineName;   // panel subtitle; empty while the built-in plays

    // External engine (Stockfish) for playing and for judging. UCI is a
    // strictly sequential protocol and the two never idle the same way, so
    // they get one process each rather than sharing one.
    Stockfish sfPlay;
    Stockfish sfJudge;

    // Best-move hints: a third instance answers "what are the strong moves in
    // this position?" with a MultiPV search, on a thread of its own so the
    // badges never queue up behind it. Started the first time hints are shown.
    Stockfish sfHint;
    std::string sfPath;   // where that engine was found ("" when there is none)

    bool hintsOn = false;
    int hintTop = 3;      // how many of the strongest moves to show (1..HINT_MAX)
    struct Hint {
        Move move;
        std::string san;
        int score = 0;    // White's point of view, like the eval bar
        int depth = 0;    // depth the score came from
    };
    std::vector<Hint> hintList;  // ready to draw, for the current position
    bool hintBusy = false;       // a search for the current position is running

    std::atomic<bool> hintQuit{false};
    std::atomic<bool> hintCancel{false};
    std::thread hintThread;
    std::mutex hintMutex;
    std::condition_variable hintCv;
    int hintGen = 0;             // bumped per request; older results are dropped
    bool hintPending = false;    // a request is waiting for the worker
    Position hintWant;
    int hintWantCount = 0;
    std::vector<Hint> hintOut;   // worker's answer, waiting to be picked up
    int hintOutGen = -1;
    bool hintReady = false;

    // ---------------------------------------------------- screen overlay ----
    // Watches a board somewhere else on the screen (another window, another
    // program, a browser tab) and draws the engine's advice over it through a
    // shaped X11 window of our own.
    bool ovOn = false;        // the button says On: the worker is looking
    bool ovSetup = false;     // its setup card is open over the board
    Color ovColor = WHITE;    // my colour on the board being watched
    bool ovFlip = false;      // that board is shown the other way round
    bool ovMirror = true;     // copy the watched board into our own window
    std::string mirrorFen;    // what the window copied last ("" while nothing)

    struct OvMove {
        int from = -1, to = -1;
        std::string san;
        int score = 0;   // White's point of view, like the eval bar
    };
    struct OvOut {
        int gen = 0;
        bool found = false;
        int covered = 0;      // squares hidden behind our own window
        SDL_Rect board{0, 0, 0, 0};
        std::string fen;
        std::string note;     // one line for the setup card / panel
        float conf = 0;
        bool hasMove = false;
        bool stockfish = false;
        std::vector<OvMove> moves;  // strongest first, up to HINT_MAX
    };

    XOverlay ovWin;                 // the shaped window the advice is drawn on
    SDL_Surface* ovFrame = nullptr;  // ARGB frame backing it
    SDL_Renderer* ovFrameR = nullptr;
    TextRenderer ovText;            // a second one, for that window
    bool ovTextOk = false;
    ArrowGeom ovArrow;              // what we last drew there (for the reader)
    int ovFrameGen = -1;            // result the frame was painted from
    bool ovFrameShown = false;      // and whether the window shows it

    std::atomic<bool> ovQuit{false};
    std::atomic<bool> ovCancel{false};  // cut a running search / capture short
    std::thread ovThread;
    std::mutex ovMutex;
    std::condition_variable ovCv;
    bool ovWake = false;    // the worker has something to do now
    bool ovEnabled = false; // and this is what it should be doing
    bool ovRescan = false;  // "look again" was pressed
    int ovGen = 0;          // bumped with every published result

    OvOut ovRes;    // the worker's latest result (under ovMutex)
    OvOut ovDraw;   // the one on screen (main thread only)
    int ovSeen = 0; // the generation the frame already shows

    // ---------------------------------------------------------- helpers ----
    Color humanSide() const { return aiEnabled ? ~aiColor : pos.side; }
    bool canHumanMove() const {
        return status == GameStatus::ONGOING && !aiThinking && !aiBoth &&
               (!aiEnabled || pos.side != aiColor);
    }
    bool engineTurn() const {
        return status == GameStatus::ONGOING && aiEnabled && !aiBoth &&
               (pos.side == aiColor);
    }
    // While the window mirrors somebody else's game the engines stay out of
    // it: the real moves are made in the other program, and a reply played
    // here would only leave the two boards disagreeing.
    bool ovMirroring() const { return ovOn && ovMirror; }
    bool animActive() const {
        return animate && animAt != 0 && SDL_GetTicks() - animAt < ANIM_MS;
    }
    std::string evalText() const;

    SDL_Rect squareRect(int sq) const {
        int f = fileOf(sq), r = rankOf(sq);
        int col = flipped ? 7 - f : f;
        int row = flipped ? r : 7 - r;
        return SDL_Rect{BOARD_X + col * SQ, BOARD_Y + row * SQ, SQ, SQ};
    }

    int squareAt(int x, int y) const {
        int col = (x - BOARD_X) / SQ;
        int row = (y - BOARD_Y) / SQ;
        if (x < BOARD_X || y < BOARD_Y || col < 0 || col > 7 || row < 0 || row > 7) return -1;
        int f = flipped ? 7 - col : col;
        int r = flipped ? row : 7 - row;
        return makeSquare(f, r);
    }

    void refresh();
    void applyMove(const Move& m);
    void newGame();
    void undo();
    bool tryMove(int from, int to);
    void selectSquare(int sq);
    void startAi();
    void stopAi();
    void pollAi();
    void tick();          // poll engine + pacing + auto rematch
    void cycleAi();
    void cycleLevel();
    void togglePause();
    void cyclePace();

    // best-move hints (arrows + ranked list over the board)
    void startHints();
    void stopHints();
    void requestHints();
    void hintLoop();
    void pollHints();
    void hintsSync();   // one inline search, for snapshots
    void toggleHints();
    void cycleHintCount();
    std::vector<Hint> computeHints(const Position& p, int count, std::atomic<bool>* cancel);

    // board on screen (see the overlay block above)
    void startOverlay();
    void stopOverlay();
    void toggleOverlay();
    void setOverlayColor(Color c);
    void overlayLoop();
    void pollOverlay();
    void drawOverlayMarker();
    void drawOverlayWindow();
    void drawOverlayMenu();
    SDL_Rect ovCardRect() const;
    bool ovCardHit(int x, int y, int& item) const;

    // move classification
    void startAnalysis();
    void stopAnalysis();
    void analysisLoop();
    void queueAnalysis(const Position& before, const Move& m, int ply, const std::string& san);
    void classifyNow(const Position& before, const Move& m, int ply, const std::string& san);
    void pollAnalysis();
    void dropAnalysis();
    void drainAnalysis(int timeoutMs);

    void handleEvent(const SDL_Event& e);
    void handleClick(int x, int y, bool left);
    void draw(bool present);

    // drawing
    void drawBoard();
    void drawPanel();
    void drawEvalBar();
    void drawBanner();
    void drawToast();
    void drawHints();
    void drawPromo();
    void drawButton(const SDL_Rect& r, BtnId id, const std::string& label, bool emphasized = false,
                    bool dim = false);

    bool buttonHit(int x, int y, SDL_Rect& out, BtnId& id) const;
    void copyFen();
};

// ------------------------------------------------------------- game flow ----

void App::refresh() {
    legal = pos.legalMoves();
    status = pos.status(legal);

    if (sel >= 0) {
        Piece p = pos.at(sel);
        if (isEmpty(p) || colorOf(p) != pos.side) {
            sel = -1;
            selMoves.clear();
        }
    }

    if (status == GameStatus::ONGOING) {
        gameOverAt = 0;
        if ((aiBoth || engineTurn()) && !paused && !snapshot && !ovMirroring()) startAi();
        else stopAi();
    } else {
        if (gameOverAt == 0) gameOverAt = SDL_GetTicks();
        stopAi();
    }

    // The position changed (or was confirmed unchanged): the hints on screen
    // belong to the old one, so ask for a fresh set.
    requestHints();
}

void App::newGame() {
    stopAi();
    dropAnalysis();
    pos.setStartPosition();
    stack.clear();
    sans.clear();
    tags.clear();
    toastTag = MoveTag::NONE;
    sel = -1;
    selMoves.clear();
    dragging = false;
    promoOpen = false;
    scroll = 0;
    lastFrom = lastTo = -1;
    engineInfo.clear();
    evalCp = 0;
    dispEval = 0.0;
    gameOverAt = 0;
    paused = false;
    aiHoldUntil = 0;
    animAt = 0;
    refresh();
}

void App::applyMove(const Move& m) {
    bool found = false;
    for (const Move& l : legal)
        if (l == m) found = true;
    if (!found) return;

    std::string san = pos.toSan(m);
    UndoInfo u;
    const Position before = pos;  // the position the move has to be judged against

    // Slide animation for the moving piece (and the rook when castling).
    if (animate) {
        animAt = SDL_GetTicks();
        animFrom = m.from;
        animTo = m.to;
        animPiece = pos.at(m.from);
        animRookFrom = animRookTo = -1;
        animRookPiece = 0;
        if (m.castle) {
            const int r = rankOf(m.from);
            if (fileOf(m.to) == 6) {  // king side
                animRookFrom = makeSquare(7, r);
                animRookTo = makeSquare(5, r);
            } else {                  // queen side
                animRookFrom = makeSquare(0, r);
                animRookTo = makeSquare(3, r);
            }
            animRookPiece = pos.at(animRookFrom);
        }
    }

    pos.doMove(m, u);
    stack.emplace_back(m, u);
    sans.push_back(san);
    tags.push_back(MoveTag::NONE);
    if (analyze) {
        const int ply = (int)tags.size() - 1;
        if (analyzeSync) classifyNow(before, m, ply, san);
        else queueAnalysis(before, m, ply, san);
    }
    lastFrom = m.from;
    lastTo = m.to;
    sel = -1;
    selMoves.clear();
    dragging = false;
    promoOpen = false;

    // Static evaluation from White's point of view (search scores override this later).
    const int e = Engine::evaluate(pos);
    evalCp = pos.side == WHITE ? e : -e;

    // Watch mode: leave the engines a moment to admire their own move.
    aiHoldUntil = SDL_GetTicks() + (aiBoth ? (Uint32)PACES[pace] : 0);

    refresh();
}

void App::undo() {
    stopAi();
    dropAnalysis();
    toastTag = MoveTag::NONE;
    if (stack.empty()) return;
    auto popOne = [&]() {
        const auto& e = stack.back();
        pos.undoMove(e.first, e.second);
        stack.pop_back();
        if (!sans.empty()) sans.pop_back();
        if (!tags.empty()) tags.pop_back();
    };
    popOne();
    if (aiEnabled && !aiBoth) {
        while (!stack.empty() && pos.side != ~aiColor) popOne();
    }
    lastFrom = stack.empty() ? -1 : stack.back().first.from;
    lastTo = stack.empty() ? -1 : stack.back().first.to;
    sel = -1;
    selMoves.clear();
    promoOpen = false;
    engineInfo.clear();
    gameOverAt = 0;
    aiHoldUntil = 0;
    animAt = 0;
    const int e = Engine::evaluate(pos);
    evalCp = pos.side == WHITE ? e : -e;
    if (stack.empty()) scroll = 0;
    if (aiBoth) paused = true;  // stepping back in watch mode holds the engines
    refresh();
}

void App::selectSquare(int sq) {
    Piece p = pos.at(sq);
    if (isEmpty(p) || colorOf(p) != pos.side) {
        sel = -1;
        selMoves.clear();
        return;
    }
    sel = sq;
    selMoves.clear();
    for (const Move& m : legal)
        if (m.from == sq) selMoves.push_back(m);
    if (selMoves.empty()) sel = -1;
}

bool App::tryMove(int from, int to) {
    Move candidates[4];
    int n = 0;
    for (const Move& m : legal)
        if (m.from == from && m.to == to && n < 4) candidates[n++] = m;
    if (n == 0) return false;
    if (n > 1 || candidates[0].promotion != NO_PIECE) {
        promoOpen = true;
        promoFrom = from;
        promoTo = to;
        return true;
    }
    applyMove(candidates[0]);
    return true;
}

// ------------------------------------------------------------------- AI ----

void App::stopAi() {
    if (aiThread.joinable()) {
        aiCancel = true;
        aiThread.join();
    }
    aiCancel = false;
    aiThinking = false;
    aiGen++;
    std::lock_guard<std::mutex> g(aiMutex);
    aiOut.ready = false;
}

void App::startAi() {
    if (status != GameStatus::ONGOING || paused) return;
    if (!(aiBoth || engineTurn())) return;
    if (SDL_GetTicks() < aiHoldUntil) return;  // paced: tick() retries later
    if (aiThread.joinable()) {
        aiCancel = true;
        aiThread.join();
        aiCancel = false;
    }
    aiThinking = true;
    const int gen = ++aiGen;
    Position snapshot = pos;
    const Color mover = pos.side;
    const int lvl = level;
    const uint64_t seed = LEVELS[lvl].noise ? (uint64_t)std::random_device{}() : 0ULL;
    std::atomic<bool>* flag = &aiCancel;

    aiThread = std::thread([this, snapshot, gen, lvl, seed, flag, mover]() {
        // Stockfish when it is running, the built-in search otherwise.
        const bool useSf = sfPlay.alive();
        SearchResult res;
        if (useSf) {
            sfPlay.setThreads(LEVELS[lvl].sfThreads);
            sfPlay.search(snapshot, LEVELS[lvl].sfDepth, LEVELS[lvl].sfTimeMs, flag, res);
        } else {
            Engine engine;
            engine.setCancelFlag(flag);
            res = engine.search(snapshot, LEVELS[lvl].depth, LEVELS[lvl].time, LEVELS[lvl].noise, seed);
        }
        if (flag->load()) return;
        if (!res.found) {  // engine produced no move: take any legal one
            Position fallback = snapshot;
            std::vector<Move> lm = fallback.legalMoves();
            if (lm.empty()) return;
            res.move = lm.front();
            res.found = true;
        }
        char buf[64];
        double kn = res.nodes / 1000.0;
        if (res.mate) {
            int plies = MATE_SCORE - std::abs(res.score);
            int moves = (plies + 1) / 2;
            int signedMoves = res.score > 0 ? moves : -moves;
            std::snprintf(buf, sizeof(buf), "d%d mate in %d", res.depth, signedMoves);
        } else {
            std::snprintf(buf, sizeof(buf), "d%d %.0fk %+.2f", res.depth, kn, res.score / 100.0);
        }
        std::lock_guard<std::mutex> g(aiMutex);
        if (aiGen.load() != gen) return;
        aiOut.ready = res.found;
        aiOut.gen = gen;
        aiOut.move = res.move;
        aiOut.info = buf;
        aiOut.score = mover == WHITE ? res.score : -res.score;  // White's point of view
        aiOut.mate = res.mate;
    });
}

void App::pollAi() {
    if (!aiThinking) return;
    Move mv;
    bool got = false;
    int score = 0;
    {
        std::lock_guard<std::mutex> g(aiMutex);
        if (aiOut.ready && aiOut.gen == aiGen.load()) {
            mv = aiOut.move;
            score = aiOut.score;
            aiOut.ready = false;
            got = true;
        }
    }
    if (!got) return;
    aiThinking = false;
    engineInfo = aiOut.info;
    aiOut.info.clear();
    if (status != GameStatus::ONGOING) return;
    if (!(aiBoth || engineTurn())) return;
    for (const Move& m : legal) {
        if (m == mv) {
            applyMove(m);
            evalCp = score;  // search verdict (White's view) beats the static evaluation
            return;
        }
    }
}

void App::tick() {
    pollAnalysis();
    pollAi();
    pollHints();
    // The watched board, copied into our own window: same position, our own
    // rendering, our own pieces. Only a position the overlay vouched for gets
    // here -- the one with the advice attached -- and only when it differs
    // from what the window already shows.
    if (ovOn && ovMirror && ovDraw.hasMove && ovDraw.fen != mirrorFen) {
        mirrorFen = ovDraw.fen;
        stopAi();
        dropAnalysis();
        pos.setFen(ovDraw.fen);
        stack.clear();
        sans.clear();
        tags.clear();
        toastTag = MoveTag::NONE;
        sel = -1;
        selMoves.clear();
        dragging = false;
        promoOpen = false;
        scroll = 0;
        lastFrom = lastTo = -1;
        engineInfo.clear();
        evalCp = 0;
        dispEval = 0.0;
        gameOverAt = 0;
        paused = false;
        aiHoldUntil = 0;
        animAt = 0;
        flipped = (ovColor == BLACK) != ovFlip;  // the watched board's own view
        refresh();
    }
    // Start the engine when its turn arrives (or when a paced hold expires).
    if (!aiThinking && !paused && !ovMirroring() && status == GameStatus::ONGOING &&
        (aiBoth || engineTurn()) && SDL_GetTicks() >= aiHoldUntil) {
        startAi();
    }
    // Watch mode: auto-rematch a few seconds after the game ends.
    if (aiBoth && status != GameStatus::ONGOING && gameOverAt != 0 && !paused &&
        SDL_GetTicks() - gameOverAt > 4000) {
        newGame();
    }
}

// ------------------------------------------------------- move judgement ----

void App::startAnalysis() {
    if (anThread.joinable()) return;
    anQuit = false;
    anThread = std::thread(&App::analysisLoop, this);
}

void App::stopAnalysis() {
    if (!anThread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(anMutex);
        anQuit = true;
        anQueue.clear();
    }
    anCv.notify_all();
    anThread.join();
}

void App::analysisLoop() {
    Engine engine;
    engine.setCancelFlag(&anQuit);  // abandon the search promptly when quitting
    for (;;) {
        AnJob job;
        {
            std::unique_lock<std::mutex> lk(anMutex);
            anCv.wait(lk, [this] { return anQuit.load() || !anQueue.empty(); });
            if (anQueue.empty()) break;  // shutting down with nothing left to do
            job = anQueue.front();
            anQueue.erase(anQueue.begin());
            anActive = true;
        }

        AnRes res;
        res.gen = job.gen;
        res.ply = job.ply;
        res.san = job.san;
        res.tag = MoveTag::NONE;
        if (!anQuit.load()) {
            Assessment a;
            if (sfJudge.alive())
                sfJudge.assess(job.before, job.move, JUDGE_DEPTH, JUDGE_TIME_MS, &anQuit, a);
            else
                a = engine.assess(job.before, job.move, ANALYZE_DEPTH, ANALYZE_TIME);
            res.tag = classifyMove(a);
        }

        std::lock_guard<std::mutex> lk(anMutex);
        anActive = false;
        anDone.push_back(res);
    }
}

void App::queueAnalysis(const Position& before, const Move& m, int ply, const std::string& san) {
    if (!anThread.joinable()) return;
    std::lock_guard<std::mutex> lk(anMutex);
    if (anQueue.size() >= AN_QUEUE_MAX) anQueue.erase(anQueue.begin());
    AnJob job;
    job.before = before;
    job.move = m;
    job.san = san;
    job.ply = ply;
    job.gen = anGen;
    anQueue.push_back(job);
    anCv.notify_one();
}

void App::classifyNow(const Position& before, const Move& m, int ply, const std::string& san) {
    if (ply < 0 || ply >= (int)tags.size()) return;
    Assessment a;
    if (sfJudge.alive())  // sync judging never overlaps the worker thread
        sfJudge.assess(before, m, JUDGE_DEPTH, JUDGE_TIME_MS, nullptr, a);
    else {
        Engine engine;
        a = engine.assess(before, m, ANALYZE_DEPTH, ANALYZE_TIME);
    }
    const MoveTag t = classifyMove(a);
    tags[ply] = t;
    if (t != MoveTag::NONE) {
        toastTag = t;
        toastSan = san;
        toastAt = SDL_GetTicks();
    }
}

void App::pollAnalysis() {
    std::vector<AnRes> done;
    {
        std::lock_guard<std::mutex> lk(anMutex);
        done.swap(anDone);
        anBusy = anActive || !anQueue.empty();
    }
    for (const AnRes& r : done) {
        if (r.gen != anGen) continue;              // undone, or a new game started
        if (r.ply < 0 || r.ply >= (int)tags.size()) continue;
        if (tags[r.ply] != MoveTag::NONE) continue;
        tags[r.ply] = r.tag;
        if (r.tag != MoveTag::NONE) {
            toastTag = r.tag;
            toastSan = r.san;
            toastAt = SDL_GetTicks();
        }
    }
}

// Throw away judgements of moves that no longer exist (undo, new game).
void App::dropAnalysis() {
    ++anGen;
    std::lock_guard<std::mutex> lk(anMutex);
    anQueue.clear();
    anDone.clear();
}

// Wait for the analyst to catch up (used by the headless --plies report).
void App::drainAnalysis(int timeoutMs) {
    const Uint32 start = SDL_GetTicks();
    for (;;) {
        pollAnalysis();
        bool busy;
        {
            std::lock_guard<std::mutex> lk(anMutex);
            busy = anActive || !anQueue.empty();
        }
        if (!busy || SDL_GetTicks() - start > (Uint32)timeoutMs) return;
        SDL_Delay(15);
    }
}

// ---------------------------------------------------------- best-move hints ---

// Spawn the hint worker -- and its own engine process -- the first time hints
// are shown, so nothing is paid while they stay switched off.
void App::startHints() {
    if (hintThread.joinable()) return;
    if (!sfHint.alive() && !sfPath.empty()) sfHint.start(sfPath, HINT_THREADS, HINT_HASH_MB);
    hintQuit = false;
    hintThread = std::thread(&App::hintLoop, this);
}

void App::stopHints() {
    if (hintThread.joinable()) {
        hintQuit = true;
        hintCancel = true;  // cut a running search short
        hintCv.notify_all();
        hintThread.join();
    }
    hintQuit = false;
    hintCancel = false;
    hintList.clear();
    hintBusy = false;
    sfHint.quit();
}

// One ranked list for one position: Stockfish MultiPV when there is one, the
// built-in engine's own rooted search when there is not.
std::vector<App::Hint> App::computeHints(const Position& p, int count,
                                          std::atomic<bool>* cancel) {
    std::vector<Hint> list;
    if (count <= 0) return list;

    std::vector<ScoredMove> top;
    if (sfHint.alive()) {
        if (!sfHint.bestMoves(p, HINT_DEPTH, HINT_TIME_MS, count, cancel, top)) return list;
    } else {
        Engine engine;
        engine.setCancelFlag(cancel);
        top = engine.rankMoves(p, ANALYZE_DEPTH, ANALYZE_TIME);
        if ((int)top.size() > count) top.resize(count);
    }

    Position copy = p;  // toSan() is not const
    for (const ScoredMove& s : top) {
        Hint h;
        h.move = s.move;
        h.san = copy.toSan(s.move);
        h.score = p.side == WHITE ? s.score : -s.score;  // shown White's way, like the bar
        h.depth = s.depth;
        list.push_back(h);
    }
    return list;
}

// Called whenever the position is settled: the arrows on screen describe the
// position that just changed, so they go and a fresh search is queued.
void App::requestHints() {
    if (snapshot) return;  // a snapshot asks for its hints inline instead
    if (!hintsOn || status != GameStatus::ONGOING) {
        {
            std::lock_guard<std::mutex> lk(hintMutex);
            hintPending = false;
            hintCancel = true;  // a search running now would be for nothing
        }
        hintList.clear();
        hintBusy = false;
        return;
    }
    startHints();
    {
        std::lock_guard<std::mutex> lk(hintMutex);
        hintWant = pos;
        hintWantCount = hintTop;
        hintGen++;
        hintPending = true;
        hintCancel = true;  // the request already running is stale from here on
    }
    hintCv.notify_one();
    hintList.clear();
    hintBusy = true;
}

void App::hintLoop() {
    for (;;) {
        Position want;
        int count = 0, gen = 0;
        {
            std::unique_lock<std::mutex> lk(hintMutex);
            hintCv.wait(lk, [this] { return hintQuit.load() || hintPending; });
            if (hintQuit.load()) break;  // the queued request is abandoned too
            want = hintWant;
            count = hintWantCount;
            gen = hintGen;
            hintPending = false;
            hintCancel = false;  // this is the newest request, so let it run
        }

        std::vector<Hint> list = computeHints(want, count, &hintCancel);

        std::lock_guard<std::mutex> lk(hintMutex);
        hintOut = std::move(list);
        hintOutGen = gen;
        hintReady = true;
    }
}

void App::pollHints() {
    std::vector<Hint> got;
    int gen = -1;
    {
        std::lock_guard<std::mutex> lk(hintMutex);
        if (!hintReady) return;
        got.swap(hintOut);
        gen = hintOutGen;
        hintReady = false;
    }
    if (gen != hintGen) return;  // the position moved on while this was searching
    hintBusy = false;
    if (hintsOn && status == GameStatus::ONGOING) hintList = std::move(got);
}

// Snapshots have no worker to ask, so they search right here, once.
void App::hintsSync() {
    hintList.clear();
    hintBusy = false;
    if (!hintsOn || status != GameStatus::ONGOING) return;
    if (!sfHint.alive() && !sfPath.empty()) sfHint.start(sfPath, HINT_THREADS, HINT_HASH_MB);
    hintList = computeHints(pos, hintTop, nullptr);
}

void App::toggleHints() {
    hintsOn = !hintsOn;
    requestHints();
}

void App::cycleHintCount() {
    hintTop = hintTop % HINT_MAX + 1;
    if (hintsOn) requestHints();
}

void App::cycleAi() {
    stopAi();
    if (aiBoth) {
        aiBoth = false;
        aiEnabled = false;
    } else if (aiEnabled && aiColor == BLACK) {
        aiColor = WHITE;
    } else if (aiEnabled && aiColor == WHITE) {
        aiBoth = true;
        aiEnabled = true;
    } else {
        aiEnabled = true;
        aiColor = BLACK;
    }
    paused = false;
    sel = -1;
    selMoves.clear();
    refresh();
}

void App::cycleLevel() {
    level = (level + 1) % 3;
    if (aiThinking) {
        stopAi();
        refresh();
    }
}

void App::togglePause() {
    paused = !paused;
    if (paused) stopAi();
    aiHoldUntil = 0;
    if (!paused) refresh();
}

void App::cyclePace() {
    pace = (pace + 1) % 3;
}

std::string App::evalText() const {
    if (std::abs(evalCp) >= 99000) {
        int plies = 100000 - std::abs(evalCp);
        int moves = (plies + 1) / 2;
        return std::string(evalCp > 0 ? "+" : "-") + "#" + std::to_string(moves);
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%+.2f", evalCp / 100.0);
    return buf;
}

void App::copyFen() {
    std::string fen = pos.toFen();
    SDL_SetClipboardText(fen.c_str());
}

// ---------------------------------------------------------------- input ----

bool App::buttonHit(int x, int y, SDL_Rect& out, BtnId& id) const {
    const int pad = 8;
    const int innerX = PANEL_X + pad;
    const int innerW = PANEL_W - 2 * pad;
    const int gap = 12;
    const int colW = (innerW - gap) / 2;
    const int btnH = 28;
    const int rowGap = 5;
    const int y0 = 112;

    static const BtnId ids[6][2] = {{BTN_NEW, BTN_UNDO},
                                    {BTN_AI, BTN_LEVEL},
                                    {BTN_FLIP, BTN_FEN},
                                    {BTN_PAUSE, BTN_SPEED},
                                    {BTN_HINTS, BTN_TOPN},
                                    {BTN_OVERLAY, BTN_SETUP}};
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 2; ++col) {
            SDL_Rect r{innerX + col * (colW + gap), y0 + row * (btnH + rowGap), colW, btnH};
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                out = r;
                id = ids[row][col];
                return true;
            }
        }
    }
    id = BTN_NONE;
    return false;
}

void App::handleClick(int x, int y, bool left) {
    // Promotion picker takes priority.
    if (promoOpen) {
        static const PieceType kinds[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
        const int cellW = 64, cellH = 52;
        SDL_Rect d = squareRect(promoTo);
        int sx = d.x + (SQ - cellW) / 2;
        // Rank 7 = White promotes (strip hangs below), rank 0 = Black (strip hangs above).
        int sy = (rankOf(promoTo) == 7) ? d.y : d.y + SQ - 4 * cellH;
        for (int i = 0; i < 4; ++i) {
            SDL_Rect c{sx, sy + i * cellH, cellW, cellH};
            if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) {
                for (const Move& m : legal) {
                    if (m.from == promoFrom && m.to == promoTo && m.promotion == kinds[i]) {
                        applyMove(m);
                        return;
                    }
                }
            }
        }
        promoOpen = false;
        return;
    }

    // The overlay's setup card: it takes the click, and a click outside it
    // simply closes it.
    if (ovSetup) {
        const SDL_Rect c = ovCardRect();
        const bool inside = x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h;
        int item = -1;
        if (inside && ovCardHit(x, y, item)) {
            switch (item) {
                case 0: toggleOverlay(); break;
                case 1: setOverlayColor(WHITE); break;
                case 2: setOverlayColor(BLACK); break;
                case 3:
                    {
                        std::lock_guard<std::mutex> lk(ovMutex);
                        ovFlip = !ovFlip;
                        ovRescan = true;
                        ovWake = true;
                    }
                    ovCv.notify_all();
                    break;
                case 4:
                    {
                        std::lock_guard<std::mutex> lk(ovMutex);
                        ovRescan = true;
                        ovWake = true;
                    }
                    ovCv.notify_all();
                    break;
                case 5:
                    ovMirror = !ovMirror;
                    mirrorFen.clear();  // copying starts over when turned back on
                    break;
                case 6: ovSetup = false; break;
                default: break;
            }
            return;
        }
        if (!inside) ovSetup = false;
        return;
    }

    SDL_Rect br;
    BtnId id;
    if (buttonHit(x, y, br, id)) {
        switch (id) {
            case BTN_NEW: newGame(); break;
            case BTN_UNDO: undo(); break;
            case BTN_AI: cycleAi(); break;
            case BTN_LEVEL: cycleLevel(); break;
            case BTN_FLIP: flipped = !flipped; break;
            case BTN_FEN: copyFen(); break;
            case BTN_PAUSE: togglePause(); break;
            case BTN_SPEED: cyclePace(); break;
            case BTN_HINTS: toggleHints(); break;
            case BTN_TOPN: cycleHintCount(); break;
            case BTN_OVERLAY: toggleOverlay(); break;
            case BTN_SETUP: ovSetup = !ovSetup; break;
            default: break;
        }
        return;
    }

    int sq = squareAt(x, y);
    if (!left) {
        sel = -1;
        selMoves.clear();
        dragging = false;
        return;
    }
    if (sq < 0) {
        sel = -1;
        selMoves.clear();
        return;
    }
    if (!canHumanMove()) return;

    if (sel >= 0 && sq != sel) {
        if (tryMove(sel, sq)) return;
    }

    Piece p = pos.at(sq);
    if (!isEmpty(p) && colorOf(p) == pos.side) {
        selectSquare(sq);
        dragging = true;
        dragFrom = sq;
    } else {
        sel = -1;
        selMoves.clear();
    }
}

void App::handleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_MOUSEMOTION:
            mouse.x = e.button.x;
            mouse.y = e.button.y;
            break;

        case SDL_MOUSEBUTTONDOWN:
            mouse.x = e.button.x;
            mouse.y = e.button.y;
            handleClick(e.button.x, e.button.y, e.button.button == SDL_BUTTON_LEFT);
            break;

        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT && dragging) {
                dragging = false;
                int sq = squareAt(e.button.x, e.button.y);
                if (sq >= 0 && sq != dragFrom) {
                    if (tryMove(dragFrom, sq)) break;
                    Piece p = pos.at(sq);
                    if (!isEmpty(p) && colorOf(p) == pos.side) selectSquare(sq);
                }
            }
            break;

        case SDL_MOUSEWHEEL:
            scroll -= e.wheel.y * 3;
            if (scroll < 0) scroll = 0;
            break;

        case SDL_KEYDOWN: {
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_ESCAPE || k == SDLK_q) {
                SDL_Event quitEv;
                SDL_zero(quitEv);
                quitEv.type = SDL_QUIT;
                SDL_PushEvent(&quitEv);
            } else if (k == SDLK_n) {
                newGame();
            } else if (k == SDLK_u || (k == SDLK_z && (e.key.keysym.mod & KMOD_CTRL))) {
                undo();
            } else if (k == SDLK_f) {
                flipped = !flipped;
            } else if (k == SDLK_a) {
                cycleAi();
            } else if (k == SDLK_1) {
                level = 0;
            } else if (k == SDLK_2) {
                level = 1;
            } else if (k == SDLK_3) {
                level = 2;
            } else if (k == SDLK_SPACE) {
                togglePause();
            } else if (k == SDLK_v) {
                // jump straight into watch mode (cycle at most once around)
                for (int i = 0; i < 3 && !aiBoth; ++i) cycleAi();
            } else if (k == SDLK_p) {
                cyclePace();
            } else if (k == SDLK_h) {
                // H toggles the best-move hints, Shift+H picks how many show
                if (e.key.keysym.mod & KMOD_SHIFT) cycleHintCount();
                else toggleHints();
            } else if (k == SDLK_o) {
                // O toggles the overlay on the screen, Shift+O opens its menu
                if (e.key.keysym.mod & KMOD_SHIFT) ovSetup = !ovSetup;
                else toggleOverlay();
            }
            break;
        }
        default:
            break;
    }
}

// -------------------------------------------------------------- drawing ----

void App::drawButton(const SDL_Rect& r, BtnId id, const std::string& label, bool emphasized,
                     bool dim) {
    (void)id;
    bool hover = mouse.x >= r.x && mouse.x < r.x + r.w && mouse.y >= r.y && mouse.y < r.y + r.h;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, CLR_BG.r, CLR_BG.g, CLR_BG.b, 255);
    SDL_RenderFillRect(renderer, &r);

    SDL_Color bg = hover && !dim ? CLR_BTN_H : (emphasized ? SDL_Color{126, 200, 120, 60} : CLR_BTN);
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
    SDL_RenderFillRect(renderer, &r);

    SDL_SetRenderDrawColor(renderer, CLR_BORDER.r, CLR_BORDER.g, CLR_BORDER.b,
                           dim ? 30 : (hover ? 120 : 55));
    SDL_RenderDrawRect(renderer, &r);

    SDL_Color tc = emphasized ? CLR_ACCENT : (dim ? SDL_Color{CLR_DIM.r, CLR_DIM.g, CLR_DIM.b, 140}
                                                  : CLR_TEXT);
    text.draw(label, 15, tc, r.x + r.w / 2, r.y + (r.h - text.lineHeight(15)) / 2 - 1, 1,
              FontStyle::REGULAR);
}

// Filled convex polygon, expanded into an explicit triangle fan: SDL renders
// vertices as a triangle list, so a 4-point shaft has to arrive as two
// triangles rather than one rounded-off quad.
static void fillPoly(SDL_Renderer* r, const SDL_Point* pts, int n, SDL_Color c) {
    if (n < 3) return;
    std::vector<SDL_Vertex> v;
    v.reserve((size_t)(n - 2) * 3);
    for (int i = 1; i + 1 < n; ++i) {
        const SDL_Point* tri[3] = {&pts[0], &pts[i], &pts[i + 1]};
        for (int k = 0; k < 3; ++k) {
            SDL_Vertex vert{};
            vert.position.x = (float)tri[k]->x;
            vert.position.y = (float)tri[k]->y;
            vert.color = c;
            v.push_back(vert);
        }
    }
    SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), nullptr, 0);
}

// A thick shaft from (x1,y1) to (x2,y2) with a head at the far end.
static void drawArrowShape(SDL_Renderer* r, float x1, float y1, float x2, float y2, int thick,
                           SDL_Color c) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 4.f) return;
    const float ux = dx / len, uy = dy / len;
    const float headLen = std::min(len * 0.5f, thick * 2.6f);
    const float headW = thick * 2.0f;
    const float bx = x2 - ux * headLen, by = y2 - uy * headLen;  // base of the head
    const float px = -uy * thick * 0.5f, py = ux * thick * 0.5f;  // half-thickness offset

    const SDL_Point shaft[4] = {
        {(int)(x1 + px), (int)(y1 + py)},
        {(int)(bx + px), (int)(by + py)},
        {(int)(bx - px), (int)(by - py)},
        {(int)(x1 - px), (int)(y1 - py)},
    };
    fillPoly(r, shaft, 4, c);

    const SDL_Point head[3] = {
        {(int)(x2 - ux * thick * 0.3f), (int)(y2 - uy * thick * 0.3f)},
        {(int)(bx - uy * headW * 0.5f), (int)(by + ux * headW * 0.5f)},
        {(int)(bx + uy * headW * 0.5f), (int)(by - ux * headW * 0.5f)},
    };
    fillPoly(r, head, 3, c);
}

// Our own window draws the loaded piece images when there are any (art.cpp);
// the vector pieces are the fallback, and stay what the screen reader builds
// its templates from -- matching a screen must never depend on which image
// files happen to be lying around.
static void guiPiece(SDL_Renderer* r, Piece p, const SDL_Rect& box) {
    if (artReady()) artDraw(r, p, box);
    else drawPiece(r, p, box);
}

void App::drawBoard() {
    const int hoverSq = squareAt(mouse.x, mouse.y);

    // squares
    for (int sq = 0; sq < 64; ++sq) {
        SDL_Rect r = squareRect(sq);
        bool light = ((fileOf(sq) + rankOf(sq)) & 1) == 0;
        SDL_Color c = light ? CLR_LIGHT : CLR_DARK;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
        SDL_RenderFillRect(renderer, &r);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // last move
    if (lastFrom >= 0) {
        SDL_SetRenderDrawColor(renderer, CLR_HL_LAST.r, CLR_HL_LAST.g, CLR_HL_LAST.b, CLR_HL_LAST.a);
        SDL_Rect a = squareRect(lastFrom), b = squareRect(lastTo);
        SDL_RenderFillRect(renderer, &a);
        SDL_RenderFillRect(renderer, &b);
    }

    // check
    if (pos.inCheck(pos.side)) {
        SDL_Rect k = squareRect(pos.kingSq[pos.side]);
        SDL_SetRenderDrawColor(renderer, CLR_CHECK.r, CLR_CHECK.g, CLR_CHECK.b, CLR_CHECK.a);
        SDL_RenderFillRect(renderer, &k);
    }

    // hover
    if (hoverSq >= 0 && canHumanMove()) {
        SDL_Rect h = squareRect(hoverSq);
        SDL_SetRenderDrawColor(renderer, CLR_HL_HOVER.r, CLR_HL_HOVER.g, CLR_HL_HOVER.b, CLR_HL_HOVER.a);
        SDL_RenderFillRect(renderer, &h);
    }

    // selection + targets
    if (sel >= 0) {
        SDL_Rect s = squareRect(sel);
        SDL_SetRenderDrawColor(renderer, CLR_HL_SEL.r, CLR_HL_SEL.g, CLR_HL_SEL.b, CLR_HL_SEL.a);
        SDL_RenderFillRect(renderer, &s);

        for (const Move& m : selMoves) {
            SDL_Rect t = squareRect(m.to);
            bool capture = !isEmpty(pos.at(m.to)) || m.enPassant;
            if (capture) {
                int cx = t.x + t.w / 2, cy = t.y + t.h / 2;
                drawStrokeCircle(renderer, cx, cy, SQ / 2 - 5, SDL_Color{20, 20, 20, 90}, 6);
            } else {
                int cx = t.x + t.w / 2, cy = t.y + t.h / 2;
                drawFillCircle(renderer, cx, cy, SQ / 6, SDL_Color{20, 20, 20, 70});
            }
        }
    }

    // coordinates
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 8; ++i) {
        // i counts rows/columns from the top-left of the screen.
        const int f = flipped ? 7 - i : i;
        const int r = flipped ? i : 7 - i;
        const int bottomRank = flipped ? 7 : 0;   // rank shown along the bottom edge
        const int leftFile = flipped ? 7 : 0;     // file shown along the left edge

        const int fileSqIdx = makeSquare(f, bottomRank);
        const int rankSqIdx = makeSquare(leftFile, r);
        SDL_Rect fileSq = squareRect(fileSqIdx);
        SDL_Rect rankSq = squareRect(rankSqIdx);

        const bool fileLight = ((fileOf(fileSqIdx) + rankOf(fileSqIdx)) & 1) == 0;
        const bool rankLight = ((fileOf(rankSqIdx) + rankOf(rankSqIdx)) & 1) == 0;
        SDL_Color fc = fileLight ? SDL_Color{CLR_DARK.r, CLR_DARK.g, CLR_DARK.b, 255}
                                 : SDL_Color{CLR_LIGHT.r, CLR_LIGHT.g, CLR_LIGHT.b, 255};
        SDL_Color rc = rankLight ? SDL_Color{CLR_DARK.r, CLR_DARK.g, CLR_DARK.b, 255}
                                 : SDL_Color{CLR_LIGHT.r, CLR_LIGHT.g, CLR_LIGHT.b, 255};

        text.draw(std::string(1, char('a' + f)), 13, fc, fileSq.x + fileSq.w - 6, fileSq.y + fileSq.h - 18, 2,
                  FontStyle::BOLD);
        text.draw(std::string(1, char('1' + r)), 13, rc, rankSq.x + 5, rankSq.y + 3, 0, FontStyle::BOLD);
    }

    // pieces (the mover is drawn separately, sliding toward its target)
    const bool animating = animActive();
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = pos.at(sq);
        if (isEmpty(p)) continue;
        if (dragging && sq == dragFrom) continue;
        if (animating && sq == animTo) continue;
        if (animating && sq == animRookTo) continue;
        guiPiece(renderer, p, squareRect(sq));
    }

    // sliding move animation with a simple ease-out
    if (animating) {
        const Uint32 now = SDL_GetTicks();
        float t = float(now - animAt) / float(ANIM_MS);
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        const float e = 1.f - (1.f - t) * (1.f - t);  // ease-out quad

        auto slide = [&](Piece pc, int from, int to) {
            if (isEmpty(pc) || from < 0 || to < 0) return;
            SDL_Rect a = squareRect(from), b = squareRect(to);
            SDL_Rect r{int(a.x + (b.x - a.x) * e), int(a.y + (b.y - a.y) * e), SQ, SQ};
            guiPiece(renderer, pc, r);
        };
        slide(animPiece, animFrom, animTo);
        slide(animRookPiece, animRookFrom, animRookTo);
    }

    // dragged piece follows the cursor
    if (dragging && dragFrom >= 0) {
        Piece p = pos.at(dragFrom);
        if (!isEmpty(p)) {
            SDL_Rect r{mouse.x - SQ / 2, mouse.y - SQ / 2, SQ, SQ};
            guiPiece(renderer, p, r);
        }
    }

    // best-move arrows and the ranked list they belong to
    drawHints();

    // frame
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect frame{BOARD_X - 1, BOARD_Y - 1, BOARD_PX + 2, BOARD_PX + 2};
    SDL_SetRenderDrawColor(renderer, 12, 12, 14, 255);
    SDL_RenderDrawRect(renderer, &frame);
}

// Arrows for the strongest moves, a numbered badge on every arrow, and the
// ranked list the numbers refer to, in a card over the board's lower corner.
void App::drawHints() {
    if (!hintsOn) return;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    if (!hintList.empty()) {
        // Weakest arrow first so the pick of the position lies on top of them.
        for (int i = (int)hintList.size() - 1; i >= 0; --i) {
            const Hint& h = hintList[i];
            const SDL_Color c = hintColor(i);
            const SDL_Rect a = squareRect(h.move.from), b = squareRect(h.move.to);
            const float x1 = a.x + a.w * 0.5f, y1 = a.y + a.h * 0.5f;
            const float x2 = b.x + b.w * 0.5f, y2 = b.y + b.h * 0.5f;

            // a move that takes something is ringed, like the capture marker
            if (!isEmpty(pos.at(h.move.to)) || h.move.enPassant)
                drawStrokeCircle(renderer, (int)x2, (int)y2, SQ / 2 - 5,
                                 SDL_Color{c.r, c.g, c.b, 150}, 5);
            drawArrowShape(renderer, x1, y1, x2, y2, 10, c);
        }

        // Numbered badges sit on the shafts, halfway along; drawn the same way
        // round so rank 1 ends up on top when two moves start on one square.
        for (int i = (int)hintList.size() - 1; i >= 0; --i) {
            const Hint& h = hintList[i];
            const SDL_Rect a = squareRect(h.move.from), b = squareRect(h.move.to);
            const int cx = (a.x + b.x + SQ) / 2;
            const int cy = (a.y + b.y + SQ) / 2;
            const SDL_Color c = hintColor(i);
            drawFillCircle(renderer, cx, cy, 12, SDL_Color{c.r, c.g, c.b, 255});
            text.draw(std::to_string(i + 1), 14, SDL_Color{18, 20, 18, 255}, cx, cy - 8, 1,
                      FontStyle::BOLD);
        }
    }

    if (hintList.empty() && !hintBusy) return;  // nothing to report

    const int cardW = 186, padX = 10, padY = 10, headH = 22, rowH = 20;
    const int rows = (int)hintList.size();
    const int cardH = headH + (rows > 0 ? rows * rowH : rowH) + 6;
    const int x = BOARD_X + padX;
    const int y = BOARD_Y + BOARD_PX - padY - cardH;

    const SDL_Rect card{x, y, cardW, cardH};
    SDL_SetRenderDrawColor(renderer, 14, 15, 17, 215);
    SDL_RenderFillRect(renderer, &card);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 34);
    SDL_RenderDrawRect(renderer, &card);

    text.draw("BEST MOVES", 12, CLR_DIM, x + 10, y + 5, 0, FontStyle::BOLD);
    if (rows > 0) {
        int depth = 0;
        for (const Hint& h : hintList) depth = std::max(depth, h.depth);
        text.draw("d" + std::to_string(depth), 12, CLR_DIM, x + cardW - 10, y + 5, 2,
                  FontStyle::MONO);
    }

    if (rows == 0) {
        const std::string note =
            hintBusy ? "searching" + std::string(1 + int(SDL_GetTicks() / 360) % 3, '.') : "no moves";
        text.draw(note, 14, CLR_DIM, x + 10, y + headH + 1, 0, FontStyle::REGULAR);
        return;
    }

    for (int i = 0; i < rows; ++i) {
        const Hint& h = hintList[i];
        const int ry = y + headH + i * rowH;
        const SDL_Color c = hintColor(i);
        drawFillCircle(renderer, x + 18, ry + rowH / 2, 9, SDL_Color{c.r, c.g, c.b, 255});
        text.draw(std::to_string(i + 1), 11, SDL_Color{18, 20, 18, 255}, x + 18,
                  ry + rowH / 2 - 7, 1, FontStyle::BOLD);
        text.draw(h.san, 15, CLR_TEXT, x + 32, ry + (rowH - text.lineHeight(15)) / 2, 0,
                  FontStyle::MONO);

        // Evaluation the same way round as the bar, so the two never disagree.
        std::string ev;
        if (std::abs(h.score) >= MATE_BOUND) {
            const int moves = (MATE_SCORE - std::abs(h.score) + 1) / 2;
            ev = std::string(h.score > 0 ? "+" : "-") + "#" + std::to_string(moves);
        } else {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.2f", h.score / 100.0);
            ev = buf;
        }
        const SDL_Color ec = h.score > 0   ? SDL_Color{235, 238, 240, 255}
                             : h.score < 0 ? SDL_Color{CLR_ACCENT.r, CLR_ACCENT.g, CLR_ACCENT.b, 255}
                                           : SDL_Color{CLR_DIM.r, CLR_DIM.g, CLR_DIM.b, 255};
        text.draw(ev, 14, ec, x + cardW - 10, ry + (rowH - text.lineHeight(14)) / 2, 2,
                  FontStyle::MONO);
    }
}

// ------------------------------------------- advice over a board on screen ----
//
// The overlay watches the desktop for a chess board, reads the position off it
// and paints the engine's answer over it through a shaped, click-through X11
// window. Capture, search and painting happen on different threads, so
// everything they share travels through ovMutex.

static bool sameBoard(const Piece* a, const Piece* b) {
    return std::memcmp(a, b, 64 * sizeof(Piece)) == 0;
}

// How far apart two boards are: the count of squares that hold a different
// piece. One move touches at most four (king and rook on a castle, from and
// to for each), so anything further apart than that has not been reached by
// a single move -- see the side-to-move guard in the watcher.
static int changedSquares(const Piece* a, const Piece* b) {
    int n = 0;
    for (int i = 0; i < 64; ++i) n += a[i] != b[i];
    return n;
}

// "+0.42" / "-#3": the eval bar's way of saying it, so the two never disagree.
static std::string scoreText(int score) {
    char buf[24];
    if (std::abs(score) >= MATE_BOUND) {
        const int moves = (MATE_SCORE - std::abs(score) + 1) / 2;
        std::snprintf(buf, sizeof(buf), "%s#%d", score > 0 ? "+" : "-", moves);
    } else {
        std::snprintf(buf, sizeof(buf), "%+.2f", score / 100.0);
    }
    return buf;
}

// Spawn the watcher -- and its own engine process -- the first time it is asked
// for, so nothing is paid while it stays switched off.
void App::startOverlay() {
    {
        std::lock_guard<std::mutex> lk(ovMutex);
        ovEnabled = true;
        ovRescan = true;
        ovWake = true;
        ovCancel = false;
    }
    ovOn = true;
    if (!ovThread.joinable()) {
        ovQuit = false;
        ovThread = std::thread(&App::overlayLoop, this);
    }
    ovCv.notify_all();
}

void App::toggleOverlay() {
    if (ovOn) {
        {
            std::lock_guard<std::mutex> lk(ovMutex);
            ovEnabled = false;
            ovWake = true;
        }
        ovCancel = true;  // cut a running search short
        ovOn = false;
        ovCv.notify_all();
    } else {
        startOverlay();
    }
}

void App::setOverlayColor(Color c) {
    {
        std::lock_guard<std::mutex> lk(ovMutex);
        ovColor = c;
        ovRescan = true;  // the board is ours to read again from scratch
        ovWake = true;
    }
    ovCv.notify_all();
}

// Everything stops: the worker, its engine, the shaped window and the frame
// behind it. Called on the way out, so the threads cannot outlive SDL.
void App::stopOverlay() {
    {
        std::lock_guard<std::mutex> lk(ovMutex);
        ovEnabled = false;
        ovRescan = false;
        ovWake = true;
    }
    ovCancel = true;
    ovOn = false;
    ovQuit = true;
    ovCv.notify_all();
    if (ovThread.joinable()) ovThread.join();
    if (ovTextOk) {
        ovText.shutdown();
        ovTextOk = false;
    }
    if (ovFrameR) {
        SDL_DestroyRenderer(ovFrameR);
        ovFrameR = nullptr;
    }
    if (ovFrame) {
        SDL_FreeSurface(ovFrame);
        ovFrame = nullptr;
    }
    ovWin.close();
}

// A rectangle of a frame, clamped to it: OV_DUMP asks for the board and
// nothing else once there is one, because an argument about what a piece
// looks like is settled at the size the piece is actually drawn.
static ScreenImage cropFrame(const ScreenImage& im, int x, int y, int w, int h) {
    ScreenImage out;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > im.w) w = im.w - x;
    if (y + h > im.h) h = im.h - y;
    if (w <= 0 || h <= 0) return out;
    out.reset(w, h);
    for (int row = 0; row < h; ++row)
        std::memcpy(&out.rgb[(size_t)row * w * 3],
                    &im.rgb[(size_t)((y + row) * im.w + x) * 3], (size_t)w * 3);
    return out;
}

// The FEN with FEN field 1 -- whose move it is -- replaced. The turn the
// reader worked out stays in the reader (it is what infers an en passant
// square); what goes out as the advice and shows in the window is always
// the player's own side.
static std::string forSide(const std::string& fen, Color c) {
    const size_t sp1 = fen.find(' ');
    if (sp1 == std::string::npos || sp1 + 1 >= fen.size()) return fen;
    const size_t sp2 = fen.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return fen;
    std::string out = fen;
    out[sp1 + 1] = c == WHITE ? 'w' : 'b';
    return out;
}

void App::overlayLoop() {
    Stockfish engine;
    bool engineTried = false;
    bool enabled = false;

    // Local to this thread: the position we are acting on, the one that has
    // been seen once and has to be seen again before it is believed, and whose
    // turn it is to move there.
    Piece lastBoard[64];
    bool haveLast = false;
    Piece cand[64];
    int candCount = 0;
    Color side = WHITE;
    Color seenColor = WHITE;
    bool seenFlip = false;
    bool first = true;
    std::string cachedFen;
    std::vector<OvMove> cachedMoves;
    bool cachedSf = false;
    bool cachedOk = false;  // the cache holds a real answer, even "no moves"
    Color cachedFor = WHITE;  // ...for this player's side (the advice is always theirs)

    // Where the board was found. With the rectangle in hand, every look goes
    // straight to that area instead of searching the whole screen again --
    // every scale, every phase, the model as well -- and that search is the
    // expensive half of a look, the half that kept a look slower than the
    // second it is asked to take. The search comes back three ways: after
    // three reads in a row that are not a chess position (the board has been
    // moved or covered), whenever the player asks for it, and once every
    // quarter minute regardless, so a board that drifted follows.
    BoardRect locked;
    int lockFails = 0;    // refused reads in a row at the lock
    int sinceFind = 0;    // looks since the last full search

    auto publish = [&](OvOut& r) {
        std::lock_guard<std::mutex> lk(ovMutex);
        r.gen = ++ovGen;
        ovRes = r;
    };

    while (true) {
        Color myColor = WHITE;
        bool myFlip = false;
        {
            std::unique_lock<std::mutex> lk(ovMutex);
            if (ovQuit) break;
            const bool fast = std::strcmp(captureBackend(), "x11") == 0;
            const int ms = fast ? OV_TAKE_MS : OV_TAKE_TOOL_MS;
            if (enabled)
                ovCv.wait_for(lk, std::chrono::milliseconds(ms),
                              [this] { return ovQuit || ovWake; });
            else
                ovCv.wait(lk, [this] { return ovQuit || ovWake; });
            if (ovQuit) break;
            ovWake = false;
            enabled = ovEnabled;
            myColor = ovColor;
            myFlip = ovFlip;
            if (ovRescan) {
                ovRescan = false;
                first = true;  // start reading this board over again
                locked = BoardRect();
                lockFails = 0;
                sinceFind = 0;
            }
        }
        if (!enabled) continue;

        if (first || myColor != seenColor || myFlip != seenFlip) {
            first = false;
            seenColor = myColor;
            seenFlip = myFlip;
            haveLast = false;
            candCount = 0;
            cachedFen.clear();
            cachedMoves.clear();
            cachedOk = false;
            side = myColor;
        }

        const bool blackAtBottom = (myColor == BLACK) != myFlip;
        OvOut res;
        ScreenImage img;
        std::string err;
        // The terminal hears what the overlay refuses to act on, once per
        // distinct complaint: the card says what went wrong, not where. A
        // bounded set of the recent ones rather than just the last line --
        // a diagnosis that arrives as eight lines must not have line one
        // printed again the moment line eight is out, and any line comes back
        // only once enough has been said for it to have gone stale.
        auto say = [](const std::string& m) {
            static std::vector<std::string> said;
            for (size_t i = 0; i < said.size(); ++i)
                if (said[i] == m) return;
            std::fprintf(stderr, "chess: overlay: %s\n", m.c_str());
            if (said.size() >= 64) said.erase(said.begin());
            said.push_back(m);
        };
        const auto t0 = std::chrono::steady_clock::now();
        if (!captureScreen(img, &err)) {
            res.note = err.empty() ? std::string("the screen cannot be captured") : err;
            publish(res);
            continue;
        }
        const auto tCap = std::chrono::steady_clock::now();
        // Our own window is the best-looking checkerboard on the screen, so it
        // carries a marker stripe and is skipped by name.
        std::vector<SDL_Rect> skip;
        SDL_Rect me{0, 0, 0, 0};
        if (findSelfWindow(img, OV_MARKER, WIN_W, WIN_H, me)) {
            skip.push_back(me);
            // Where the marker puts our window, said the moment it is found:
            // everything below -- what it covers, what it hides from the
            // search -- is only as good as this rectangle, so it is worth
            // seeing on its own with no board needed to earn the line.
            say("marker at " + std::to_string(me.x) + "," + std::to_string(me.y) + " " +
                std::to_string(me.w) + "x" + std::to_string(me.h));
        }

        // What we last painted over the board: those pixels are the arrow's,
        // not the board's, and the reader has to be told.
        ArrowGeom arrow;
        {
            std::lock_guard<std::mutex> lk(ovMutex);
            arrow = ovArrow;
        }

        // The board's own area, and only that area, between searches: three
        // reads in a row that are not a chess position mean the lock is
        // pointing at something else now (moved, covered, gone) and the
        // search gets the screen back.
        if (lockFails >= 3) {
            say("that is not the board any more - looking for it again");
            locked = BoardRect();
            lockFails = 0;
        }
        BoardRect br;
        bool foundT = false;
        // The AI piece finder gets the frame as well -- one model pass that
        // knows a chessboard when it sees one (tools/piece_ai.py). The grid
        // search's rectangle is snapped to the squares and is what the overlay
        // has always drawn over, so it stands when both readers mean the same
        // board; the model's box stands in when the search finds nothing, and
        // the model's reading answers for the pieces whenever its board is
        // this board.
        auto share = [](const BoardRect& a, const BoardRect& b) {
            const int ix = std::min(a.x + a.cell * 8, b.x + b.cell * 8) - std::max(a.x, b.x);
            const int iy = std::min(a.y + a.cell * 8, b.y + b.cell * 8) - std::max(a.y, b.y);
            if (ix <= 0 || iy <= 0) return false;
            const long inter = (long)ix * iy;
            return 2 * inter >=
                   std::min((long)a.cell * 8 * (a.cell * 8), (long)b.cell * 8 * (b.cell * 8));
        };
        BoardRect brAi;
        std::string field0;
        float aiConf = 0;
        std::string aiWhy;
        bool haveAi = false;
        bool aiHere = false;
        if (!locked.ok || sinceFind >= 15) {
            // No board locked down (first look, or the player asked), or the
            // quarter minute is up: the whole screen, both readers, and the
            // rectangle adopted for the looks in between.
            sinceFind = 0;
            foundT = findBoard(img, skip, br);
            haveAi = aiAskBoard(img, skip, blackAtBottom, brAi, field0, aiConf, &aiWhy);
            aiHere = haveAi && (!foundT || share(br, brAi));
            if (!foundT && haveAi) {
                br = brAi;
            } else if (aiHere) {
                // The grid's lattice is only pinned down modulo two squares: a
                // shift by one cell is still a perfect checkerboard, and then the
                // read (and the advice drawn over it) comes out a whole square
                // off. The model's box has no such ambiguity -- so slide the
                // grid's rectangle by whole cells until the two agree, keeping
                // the grid's square size, which is the snapped one.
                const int kx = (int)std::lround((double)(brAi.x - br.x) / br.cell);
                const int ky = (int)std::lround((double)(brAi.y - br.y) / br.cell);
                br.x += kx * br.cell;
                br.y += ky * br.cell;
                // Sliding the phase cannot fix a square size: the coarse search
                // only walks even widths, so a board an odd number of pixels wide
                // can leave the lattice a whole pixel small, and by the far file
                // every square samples a sliver of its neighbour -- pieces appear
                // on empty squares and no position adds up. The model measured the
                // board itself; score its rectangle with the finder's own ruler
                // and take it when it fits better than ours.
                if (brAi.x != br.x || brAi.y != br.y || brAi.cell != br.cell) {
                    const float sAi = scoreGridAt(img, brAi.x, brAi.y, brAi.cell, skip);
                    if (sAi > br.score + 0.03f) {
                        br.x = brAi.x;
                        br.y = brAi.y;
                        br.cell = brAi.cell;
                        br.score = sAi;
                    }
                }
            }
            // A rectangle the frame does not fully contain is half a board,
            // and half a board cannot be read: the search stays inside the
            // frame by construction, the model's box is drawn where the model
            // saw the checkerboard, and that can hang off the screen. Not
            // found is the honest answer -- the refusal loop would only come
            // to the same conclusion three reads later.
            const bool fits = br.cell > 0 && br.x >= 0 && br.y >= 0 &&
                              br.x + (long)br.cell * 8 <= img.w &&
                              br.y + (long)br.cell * 8 <= img.h;
            if (foundT || haveAi) {
                if (fits) {
                    locked = br;
                    lockFails = 0;
                } else {
                    foundT = false;
                    haveAi = false;
                    aiWhy = "box runs past the edge of the frame";
                }
            }
        } else {
            // What was asked for: the area the board was found in, checked
            // again for updates. Nothing else on the screen is looked at,
            // neither the search nor the model -- the terminal says so where
            // it would otherwise wonder why the model is quiet. One exception
            // is checked first: our own window has been dragged across that
            // area, and the pixels there are then the mirror, not the game.
            // The mirror often reads back as the very position the lock last
            // saw, so the three-refusals rule never fires while the advice
            // slowly rests on itself -- and when it does not read back, the
            // board under our window is simply gone. The marker knows where
            // we are; the search gets the screen back and reports the cover.
            const SDL_Rect board{locked.x, locked.y, locked.cell * 8, locked.cell * 8};
            if (me.w > 0 && SDL_HasIntersection(&me, &board)) {
                aiWhy = "our window is over the board - move it aside";
                say("our own window is on the board - looking for it elsewhere");
                locked = BoardRect();
                lockFails = 0;
                sinceFind = 0;
            }
            if (locked.ok) {
                ++sinceFind;
                br = locked;
                foundT = true;
                aiWhy = "not asked - watching where the board was found";
            }
        }
        const bool found = foundT || haveAi;
        // OV_DUMP names a file, and what goes into it depends on how the
        // search went. A board found means the board itself, cropped out at
        // full size -- what a piece looks like is settled at the size the
        // piece is actually drawn. No board means the whole frame, because
        // "nothing there" is a claim about the whole screen. Either way, once
        // a board frame has been written, frames without a board stop
        // replacing it: the last capture of a session is usually the one where
        // the board has already been moved away, and that is the one least
        // worth having. The terminal hears about it once if the file cannot be
        // written.
        {
            static const char* dump = std::getenv("OV_DUMP");
            static bool boardWritten = false;
            if (dump && *dump) {
                bool did = false, wrote = true;
                if (found) {
                    const int pad = br.cell / 2;
                    const int span = br.cell * 8 + 2 * pad;
                    did = true;
                    wrote = saveScreenImage(
                        cropFrame(img, br.x - pad, br.y - pad, span, span), dump);
                    boardWritten = wrote;
                } else if (!boardWritten) {
                    did = true;
                    wrote = saveScreenImage(img, dump);
                }
                if (did && !wrote) {
                    static bool said = false;
                    if (!said) {
                        said = true;
                        std::fprintf(stderr, "chess: cannot write OV_DUMP %s\n", dump);
                    }
                }
            }
        }
        if (!found) {
            res.covered = br.covered;
            res.note = br.covered > 0 ? "the board is behind this window - move it aside"
                                      : "no chess board found on screen";
            // The card says what went wrong in a few words; the terminal gets
            // the numbers behind it -- how well the best grid scored, how many
            // candidates there were, whether our own window ate them.
            const std::string why =
                br.why.empty() ? std::string("nothing was recorded") : br.why;
            const std::string win =
                skip.empty()
                    ? std::string("no marker seen")
                    : "our window " + std::to_string(me.x) + "," + std::to_string(me.y) + " " +
                          std::to_string(me.w) + "x" + std::to_string(me.h);
            const std::string ai = aiWhy.empty() ? std::string() : " | ai: " + aiWhy;
            if (br.covered > 0)
                say(win + " covers a board at " + std::to_string(br.x) + "," + std::to_string(br.y) +
                    " " + std::to_string(br.cell * 8) + "px wide | " + why + ai);
            else
                say("no board: " + why + " | " + win + ai);
            publish(res);
            continue;
        }

        // OV_DEBUG asks for the squares and the terms even when the read was
        // accepted: a refusal explains itself, a confident mistake does not,
        // and that is the case worth having numbers for.
        static const bool ovDiag = std::getenv("OV_DEBUG") != nullptr;
        // Which reader answers. The model's reading when its board is this
        // board; the grid reader otherwise; and the grid reader once more when
        // the model's placement is refused -- a model that miscounts kings and
        // a search that liked this checkerboard are two independent opinions,
        // and one of them may still be right.
        auto aiRead = [&](Color s) {
            ReadResult r;
            aiToReadResult(field0, aiConf, s, r);
            return r;
        };
        ReadResult rd;
        bool usedAi = false;
        // What the model was doing all this time -- silence is otherwise
        // indistinguishable from "the grid reader answered", and which of the
        // two is speaking decides which one to fix, so both the refusal and
        // the accepted read carry it.
        const std::string aiNote =
            !haveAi ? " | ai: " + (aiWhy.empty() ? std::string("no answer") : aiWhy)
            : !aiHere ? " | ai board at " + std::to_string(brAi.x) + "," +
                            std::to_string(brAi.y) + " cell " + std::to_string(brAi.cell) +
                            " - elsewhere"
                      : "";
        if (aiHere) {
            rd = aiRead(side);
            usedAi = rd.ok;
            if (!rd.ok && foundT) {
                // The model's answer is kept for the terminal: once the grid
                // reader answers instead, its FEN is all the log would show,
                // and what the model said is the half worth comparing.
                say("the ai read: " + rd.fen + " (" + std::to_string(rd.kings) +
                    " kings) - asking the grid reader too");
                rd = readBoard(img, br, blackAtBottom, side, arrow,
                               haveLast ? lastBoard : nullptr, ovDiag);
            }
        } else {
            rd = readBoard(img, br, blackAtBottom, side, arrow,
                           haveLast ? lastBoard : nullptr, ovDiag);
        }
        if (!rd.ok) {
            // A board was found and its squares were read; the answer just is
            // not a chess position. Everything it saw still goes out -- the
            // FEN it came to is the only clue to which square went wrong.
            res.found = true;
            res.board = SDL_Rect{br.x, br.y, br.cell * 8, br.cell * 8};
            res.fen = forSide(rd.fen, myColor);
            res.conf = rd.confidence;
            int pieces = 0;
            for (int i = 0; i < 64; ++i)
                if (rd.board[i] != 0) ++pieces;
            res.note = pieces == 0 ? std::string("no pieces read off that board")
                       : rd.kings == 1
                           ? std::string("one king read, not two - no position")
                           : rd.kings == 2
                               ? std::string("the placement did not add up - no position")
                               : std::to_string(rd.kings) +
                                     " kings read - not a chess position";
            // Everything the reader had to work from, said in one line: where
            // the grid sat, how sure it was, whether our own window was in the
            // way, and what it thought it saw. "Not a position" on its own
            // never says which square went wrong.
            const std::string win =
                skip.empty()
                    ? std::string(", no marker seen")
                    : ", our window " + std::to_string(me.x) + "," + std::to_string(me.y) + " " +
                          std::to_string(me.w) + "x" + std::to_string(me.h);
            char where[512];
            // The grid's own score goes with it: 0.9 means the detector was
            // sure this is a checkerboard and the reader is what failed, 0.63
            // means the grid itself was found barely standing.
            std::snprintf(where, sizeof where,
                          "not a position: %d kings, conf %.2f, board %d,%d cell %d grid %.2f%s%s: %s",
                          rd.kings, rd.confidence, br.x, br.y, br.cell, br.score, win.c_str(),
                          aiNote.c_str(), rd.fen.c_str());
            say(where);
            // Then the squares themselves: the FEN says what the board came
            // out as, these say how sure each square was, how every other
            // piece type scored and how much left with a smaller blob -- the
            // difference between a king that lost narrowly and a square
            // nobody could read at all. The squares decided by the narrowest
            // margins then come apart into the six measurements their scores
            // add up to: the total says who won, this says why.
            for (size_t i = 0; i < rd.detail.size(); ++i) say("squares " + rd.detail[i]);
            for (size_t i = 0; i < rd.terms.size(); ++i) say("terms " + rd.terms[i]);
            if (locked.ok) ++lockFails;  // three in a row and the lock lets go
            publish(res);
            continue;
        }
        // The good reads say where they sat as well: with a working example to
        // compare against, a grid at the wrong scale or offset shows itself.
        char got[384];
        std::snprintf(got, sizeof got, "read %d,%d cell %d grid %.2f conf %.2f [%s]: %s%s", br.x,
                      br.y, br.cell, br.score, rd.confidence, usedAi ? "ai" : "grid",
                      rd.fen.c_str(), aiNote.c_str());
        say(got);
        // ...and the squares and the terms behind it when asked. A refusal
        // has already said all this; the read that was accepted and is wrong
        // has said nothing at all, which is the case OV_DEBUG exists for.
        if (ovDiag) {
            for (size_t i = 0; i < rd.detail.size(); ++i) say("squares " + rd.detail[i]);
            for (size_t i = 0; i < rd.terms.size(); ++i) say("terms " + rd.terms[i]);
        }

        bool changed = false;
        if (haveLast && sameBoard(rd.board, lastBoard)) {
            // Nothing has moved -- and the candidate waiting in the wings has
            // had its chance: a board that showed up once and never came back
            // was a half-moved piece or a misread, and holding on to it would
            // pair it with any repeat much later into a "move".
            candCount = 0;
        } else if (!haveLast) {
            std::memcpy(lastBoard, rd.board, sizeof lastBoard);
            haveLast = true;
        } else {
            if (candCount > 0 && sameBoard(rd.board, cand)) {
                ++candCount;
            } else {
                std::memcpy(cand, rd.board, sizeof cand);
                candCount = 1;
            }
            if (candCount < 2) {
                // Seen once only: a piece may still be sliding into place, so
                // wait for the same board twice before acting on it.
                res.found = true;
                res.board = SDL_Rect{br.x, br.y, br.cell * 8, br.cell * 8};
                res.conf = rd.confidence;
                res.note = "reading the board...";
                publish(res);
                continue;
            }
            // A move touches at most four squares -- king and rook on a
            // castle, from and to for each -- so two boards further apart
            // than that have not been reached by one move: a misread that
            // holds for two frames, or the board starting over. Flipping
            // the side to move on one of those is what hands the overlay's
            // advice to the other player (hints for Black while playing
            // White), so the flip happens only on a real move; anything
            // further along just arrives, keeping the turn we had, and the
            // first real move after it puts the turn right again.
            const Color was = side;
            if (changedSquares(rd.board, lastBoard) <= 4)
                side = (side == WHITE) ? BLACK : WHITE;
            if (side != was) {
                // Someone has just moved. Whose turn it is changes with
                // them, and that decides whether a two-square pawn push is
                // offering a capture, so the position is read once more
                // with it right.
                const ReadResult again = usedAi ? aiRead(side)
                                                 : readBoard(img, br, blackAtBottom, side, arrow,
                                                             lastBoard);
                if (again.ok) rd = again;
            }
            std::memcpy(lastBoard, rd.board, sizeof lastBoard);
            candCount = 0;
            changed = true;
        }

        lockFails = 0;  // the lock is still pointed at a chess position
        res.found = true;
        res.board = SDL_Rect{br.x, br.y, br.cell * 8, br.cell * 8};
        res.fen = forSide(rd.fen, myColor);
        res.conf = rd.confidence;

        // A checkmate or an illegal read is an answer too: caching it stops the
        // worker from searching the same dead position round after round.
        const auto tAdv = std::chrono::steady_clock::now();
        const bool fresh = cachedOk && !changed && cachedFen == rd.fen && cachedFor == myColor;
        if (!fresh) {
            Position p;
            p.setFen(rd.fen);
            // The advice belongs to the player, not to whoever the read said
            // is waiting. The overlay can know the board says Black to move
            // and still answer with White's moves, because "what do I play"
            // is the question: the read's own turn stays as read inside the
            // reader (it is what infers an en passant square), and only the
            // search, the moves and the window showing them are the player's.
            p.side = myColor;
            std::vector<ScoredMove> top;
            bool usedSf = false;
            if (!engineTried && !sfPath.empty()) {
                engineTried = true;
                engine.start(sfPath, OV_THREADS, OV_HASH_MB);
            }
            if (!ovCancel && engine.alive())
                usedSf = engine.bestMoves(p, OV_DEPTH, OV_TIME_MS, HINT_MAX, &ovCancel, top);
            if (top.empty() && !ovCancel) {
                Engine builtin;
                builtin.setCancelFlag(&ovCancel);
                top = builtin.rankMoves(p, ANALYZE_DEPTH, ANALYZE_TIME);
                usedSf = false;
            }
            if (ovCancel) continue;  // switched off mid-search: nobody wants this
            Position copy = p;
            for (const ScoredMove& s : top) {
                if ((int)res.moves.size() >= HINT_MAX) break;
                OvMove m;
                m.from = s.move.from;
                m.to = s.move.to;
                m.san = copy.toSan(s.move);
                m.score = p.side == WHITE ? s.score : -s.score;
                res.moves.push_back(m);
            }
            res.stockfish = usedSf;
            cachedOk = true;
            cachedFen = rd.fen;
            cachedMoves = res.moves;
            cachedSf = usedSf;
            cachedFor = myColor;
        } else {
            res.moves = cachedMoves;
            res.stockfish = cachedSf;
        }

        res.hasMove = !res.moves.empty();
        res.note = res.hasMove ? "on screen: " + res.moves[0].san + " " + scoreText(res.moves[0].score)
                               : "no legal moves in that position";
        // Once per look, once the position has been read and answered: whose
        // advice went out against whose turn the board says, and -- with one
        // line per look -- the interval between them, which is the thing the
        // player notices when it slips.
        if (ovDiag) {
            static std::chrono::steady_clock::time_point lastLook =
                std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const double gap = std::chrono::duration<double>(now - lastLook).count();
            lastLook = now;
            const size_t sp = rd.fen.find(' ');
            const char turn =
                sp != std::string::npos && sp + 1 < rd.fen.size() ? rd.fen[sp + 1] : '?';
            std::fprintf(stderr,
                         "chess: overlay: look +%.2fs cap %.2fs adv %.2fs%s, "
                         "advice for %c, board says %c: %s\n",
                         gap, std::chrono::duration<double>(tCap - t0).count(),
                         std::chrono::duration<double>(now - tAdv).count(), fresh ? " fresh" : "",
                         myColor == WHITE ? 'w' : 'b', turn,
                         res.hasMove ? res.moves[0].san.c_str() : "none");
        }
        publish(res);
    }
    engine.quit();
    aiShutdown();
}

// Take the worker's latest answer for the frame to show.
void App::pollOverlay() {
    std::lock_guard<std::mutex> lk(ovMutex);
    if (ovRes.gen == ovSeen) return;
    ovDraw = ovRes;
    ovSeen = ovRes.gen;
}

// Four colour blocks in our own window's bottom margin: window positions are
// guesses when SDL runs on Wayland, but a stripe in a screenshot is not, and it
// is what lets the watcher tell our board from the one being watched.
void App::drawOverlayMarker() {
    if (!ovOn || snapshot) return;
    static const SDL_Color cols[4] = {{255, 0, 128, 255}, {0, 255, 64, 255},
                                      {64, 128, 255, 255}, {255, 255, 0, 255}};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    for (int i = 0; i < 4; ++i) {
        SDL_Rect b{OV_MARKER.x + i * 6, OV_MARKER.y, 6, 6};
        SDL_SetRenderDrawColor(renderer, cols[i].r, cols[i].g, cols[i].b, 255);
        SDL_RenderFillRect(renderer, &b);
    }
}

// The advice itself: an arrow over the board and a chip beside it, painted into
// an ARGB frame whose alpha becomes the shape of the window holding it -- so
// nothing undrawn ever covers the board, and an empty input region keeps the
// player's clicks going underneath.
void App::drawOverlayWindow() {
    if (snapshot) return;
    if (!ovOn) {
        if (ovWin.isOpen()) ovWin.hide();
        ovFrameShown = false;
        return;
    }
    if (!ovDraw.found || !ovDraw.hasMove || ovDraw.board.w < 32 || ovDraw.moves.empty()) {
        if (ovWin.isOpen()) ovWin.hide();
        ovFrameShown = false;
        ovFrameGen = -1;
        ArrowGeom none;
        std::lock_guard<std::mutex> lk(ovMutex);
        ovArrow = none;
        return;
    }
    if (!ovWin.isOpen()) {
        std::string why;
        if (!ovWin.open(&why)) return;
    }

    const SDL_Rect b = ovDraw.board;
    const bool blackAtBottom = (ovColor == BLACK) != ovFlip;
    const int cell = b.w / 8;
    const int chip = std::max(30, cell * 40 / 100);

    BoardRect br;
    br.ok = true;
    br.x = b.x;
    br.y = b.y;
    br.cell = cell;
    SDL_Color ac = hintColor(0);
    ac.a = 255;  // solid: the window's alpha channel is the shape, not a fade
    const OvMove& best = ovDraw.moves[0];
    const ArrowGeom ag = makeArrow(br, blackAtBottom, best.from, best.to,
                                   std::max(8, cell * 26 / 100), ac);

    // The chip sits above the board when there is room for it there, below
    // otherwise: it must never land on the squares it is talking about.
    const bool chipTop = b.y >= chip;
    const SDL_Rect fr = chipTop ? SDL_Rect{b.x, b.y - chip, b.w, b.h + chip}
                                : SDL_Rect{b.x, b.y, b.w, b.h + chip};

    if (ovFrameGen == ovDraw.gen && ovFrame && ovFrame->w == fr.w && ovFrame->h == fr.h &&
        ovFrameShown)
        return;

    if (!ovFrame || ovFrame->w != fr.w || ovFrame->h != fr.h) {
        if (ovTextOk) {
            ovText.shutdown();
            ovTextOk = false;
        }
        if (ovFrameR) {
            SDL_DestroyRenderer(ovFrameR);
            ovFrameR = nullptr;
        }
        if (ovFrame) {
            SDL_FreeSurface(ovFrame);
            ovFrame = nullptr;
        }
        ovFrame = SDL_CreateRGBSurfaceWithFormat(0, fr.w, fr.h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!ovFrame) return;
        ovFrameR = SDL_CreateSoftwareRenderer(ovFrame);
        if (!ovFrameR) return;
        ovTextOk = ovText.init(ovFrameR);
    }
    if (!ovFrameR) return;

    // Everything undrawn stays transparent (and therefore out of the shape), so
    // start from zero rather than trusting the renderer's clear.
    if (SDL_MUSTLOCK(ovFrame)) SDL_LockSurface(ovFrame);
    std::memset(ovFrame->pixels, 0, (size_t)ovFrame->pitch * ovFrame->h);
    if (SDL_MUSTLOCK(ovFrame)) SDL_UnlockSurface(ovFrame);

    SDL_SetRenderDrawBlendMode(ovFrameR, SDL_BLENDMODE_NONE);
    SDL_Point shaft[4], head[3];
    for (int i = 0; i < 4; ++i) shaft[i] = SDL_Point{ag.shaft[i].x - fr.x, ag.shaft[i].y - fr.y};
    for (int i = 0; i < 3; ++i) head[i] = SDL_Point{ag.head[i].x - fr.x, ag.head[i].y - fr.y};
    fillPoly(ovFrameR, shaft, 4, ag.color);
    fillPoly(ovFrameR, head, 3, ag.color);

    // The chip: opaque, and always outside the board.
    SDL_Rect chipR = chipTop ? SDL_Rect{0, 0, b.w, chip} : SDL_Rect{0, b.h, b.w, chip};
    SDL_SetRenderDrawColor(ovFrameR, 18, 20, 24, 255);
    SDL_RenderFillRect(ovFrameR, &chipR);
    SDL_SetRenderDrawColor(ovFrameR, ac.r, ac.g, ac.b, 255);
    SDL_RenderDrawRect(ovFrameR, &chipR);
    if (ovTextOk) {
        const int pad = 12;
        const int px = 17;
        const int ty = chipR.y + (chip - ovText.lineHeight(px, FontStyle::BOLD)) / 2;
        ovText.draw(best.san, px, SDL_Color{240, 242, 244, 255}, chipR.x + pad, ty, 0,
                    FontStyle::MONO);
        const std::string sc = scoreText(best.score);
        ovText.draw(sc, 15, ac, chipR.x + chipR.w - pad, ty + 1, 2, FontStyle::MONO);
        // the moves behind the first one, when they fit
        std::string alt;
        for (size_t i = 1; i < ovDraw.moves.size() && i < 4; ++i) {
            if (!alt.empty()) alt += "  ";
            alt += ovDraw.moves[i].san;
        }
        if (!alt.empty()) {
            const int ax = chipR.x + pad + ovText.width(best.san, px, FontStyle::MONO) + 16;
            const int aw = ovText.width(alt, 13, FontStyle::REGULAR);
            const int sw = ovText.width(sc, 15, FontStyle::MONO) + 16;
            if (ax + aw + sw <= chipR.x + chipR.w - pad)
                ovText.draw(alt, 13, SDL_Color{150, 152, 158, 255}, ax, ty + 3, 0,
                            FontStyle::REGULAR);
        }
    }

    {
        std::lock_guard<std::mutex> lk(ovMutex);
        ovArrow = ag;  // the reader ignores these pixels on the next frame
    }
    ovWin.place(fr);
    ovWin.present(ovFrame);
    ovFrameGen = ovDraw.gen;
    ovFrameShown = true;
}

SDL_Rect App::ovCardRect() const {
    const int w = BOARD_PX - 100;
    const int h = 44 + 7 * 40 + 74;
    return SDL_Rect{BOARD_X + (BOARD_PX - w) / 2, BOARD_Y + (BOARD_PX - h) / 2, w, h};
}

// Rows of the setup card, in the order they are drawn.
bool App::ovCardHit(int x, int y, int& item) const {
    if (!ovSetup) return false;
    const SDL_Rect c = ovCardRect();
    for (int i = 0; i < 7; ++i) {
        const SDL_Rect r{c.x + 20, c.y + 40 + i * 40, c.w - 40, 34};
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
            item = i;
            return true;
        }
    }
    return false;
}

void App::drawOverlayMenu() {
    const SDL_Rect c = ovCardRect();
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect scrim{BOARD_X, BOARD_Y, BOARD_PX, BOARD_PX};
    SDL_SetRenderDrawColor(renderer, 8, 9, 11, 165);
    SDL_RenderFillRect(renderer, &scrim);
    SDL_SetRenderDrawColor(renderer, 44, 47, 52, 255);
    SDL_RenderFillRect(renderer, &c);
    SDL_SetRenderDrawColor(renderer, 126, 200, 120, 255);
    SDL_RenderDrawRect(renderer, &c);

    text.draw("BOARD ON SCREEN", 14, CLR_DIM, c.x + 20, c.y + 15, 0, FontStyle::BOLD);

    const std::string viewTxt =
        std::string("Board view: ") + (ovFlip ? "the other way up" : "my colour at the bottom");
    const std::string mirrorTxt =
        std::string("Copy the board into this window: ") + (ovMirror ? "On" : "Off");
    const std::string labels[7] = {
        ovOn ? "Stop watching this screen" : "Watch this screen",
        "I play White",
        "I play Black",
        viewTxt,
        "Look again now",
        mirrorTxt,
        "Close",
    };
    for (int i = 0; i < 7; ++i) {
        const SDL_Rect r{c.x + 20, c.y + 40 + i * 40, c.w - 40, 34};
        const bool hover =
            mouse.x >= r.x && mouse.x < r.x + r.w && mouse.y >= r.y && mouse.y < r.y + r.h;
        const bool chosen = (i == 1 && ovColor == WHITE) || (i == 2 && ovColor == BLACK);
        SDL_SetRenderDrawColor(renderer, hover ? 52 : 38, hover ? 56 : 42, hover ? 62 : 47, 255);
        SDL_RenderFillRect(renderer, &r);
        SDL_SetRenderDrawColor(renderer, chosen ? 126 : 70, chosen ? 200 : 74,
                               chosen ? 120 : 80, 255);
        SDL_RenderDrawRect(renderer, &r);
        const SDL_Color tc = (i == 0 && ovOn) ? SDL_Color{126, 200, 120, 255} : CLR_TEXT;
        text.draw(labels[i], 15, tc, r.x + 14, r.y + (r.h - text.lineHeight(15)) / 2 - 1, 0,
                  FontStyle::REGULAR);
    }

    // What it has to say for itself: where it got to, what it read, how sure
    // it was -- squeezed onto the card rather than allowed to run off it.
    auto fit = [&](const std::string& s, int px, int avail, FontStyle style) {
        while (px > 11 && text.width(s, px, style) > avail) --px;
        if (text.width(s, px, style) <= avail) return s;
        std::string out = s;
        while (!out.empty() && text.width(out + "\xE2\x80\xA6", px, style) > avail) out.pop_back();
        return out.empty() ? std::string("\xE2\x80\xA6") : out + "\xE2\x80\xA6";
    };
    const int avail = c.w - 40;
    int y = c.y + 40 + 7 * 40 + 10;
    std::string status = ovDraw.note;
    if (status.empty()) status = ovOn ? "looking at the screen..." : "switched off";
    // The overlay window's own complaint -- it could not be created, or the
    // server refused a frame -- outranks the run of the mill status line.
    if (ovOn && !ovWin.why().empty()) status = ovWin.why();
    text.draw(fit(status, 14, avail, FontStyle::REGULAR), 14, CLR_TEXT, c.x + 20, y, 0,
              FontStyle::REGULAR);
    y += 20;
    if (!ovDraw.fen.empty()) {
        text.draw(fit(ovDraw.fen, 13, avail, FontStyle::MONO), 13, CLR_DIM, c.x + 20, y, 0,
                  FontStyle::MONO);
        y += 19;
    }
    if (ovDraw.found) {
        char buf[80];
        std::snprintf(buf, sizeof buf, "read %.2f  ·  %s  ·  %s", ovDraw.conf,
                      ovDraw.hasMove ? (ovDraw.stockfish ? "Stockfish" : "built-in engine")
                                     : "not analysed",
                      ovDraw.covered > 0 ? "some squares hidden" : "all 64 squares");
        text.draw(fit(buf, 13, avail, FontStyle::REGULAR), 13, CLR_DIM, c.x + 20, y, 0,
                  FontStyle::REGULAR);
    }
}

void App::drawPanel() {
    SDL_Rect panel{PANEL_X, BOARD_Y, PANEL_W, BOARD_PX};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, CLR_PANEL.r, CLR_PANEL.g, CLR_PANEL.b, 255);
    SDL_RenderFillRect(renderer, &panel);

    const int pad = 8;
    const int innerX = PANEL_X + pad;
    const int innerW = PANEL_W - 2 * pad;

    text.draw("CHESS", 30, CLR_TEXT, innerX, 16, 0, FontStyle::BOLD);
    text.draw(engineName.empty() ? "C++ / SDL2" : engineName.c_str(), 13, CLR_DIM,
              innerX + innerW, 26, 2, FontStyle::REGULAR);

    // status
    SDL_Color statusCol = CLR_TEXT;
    std::string line1, line2;
    switch (status) {
        case GameStatus::ONGOING: {
            line1 = std::string(pos.side == WHITE ? "White" : "Black") + " to move";
            if (pos.inCheck(pos.side)) {
                line1 += " - check!";
                statusCol = CLR_WARN;
            } else if (aiThinking) {
                // animated ellipsis so the engine feels alive
                const int dots = 1 + int(SDL_GetTicks() / 320) % 3;
                line1 = std::string(pos.side == WHITE ? "White" : "Black") + " thinking" +
                        std::string(dots, '.');
                statusCol = CLR_ACCENT;
            } else if (aiBoth) {
                statusCol = CLR_ACCENT;
            } else if (aiEnabled) {
                line2 = std::string("AI ") + (aiColor == WHITE ? "White" : "Black") + " - you " +
                        (aiColor == WHITE ? "Black" : "White");
            } else {
                line2 = "Two player mode";
            }
            // Level and pace live on their buttons, so the hint only has to
            // say what kind of game this is -- the search report goes on the
            // right of the same row and the two must not collide.
            if (aiBoth && line2.empty()) line2 = "AI vs AI watch mode";
            if (paused && line2.empty()) line2 = "engines on hold";
            break;
        }
        case GameStatus::CHECKMATE: {
            line1 = "Checkmate";
            line2 = std::string(pos.side == WHITE ? "Black" : "White") + " wins";
            statusCol = CLR_ACCENT;
            break;
        }
        case GameStatus::STALEMATE:
            line1 = "Stalemate";
            line2 = "Draw";
            statusCol = CLR_WARN;
            break;
        case GameStatus::FIFTY_MOVE:
            line1 = "Draw";
            line2 = "Fifty move rule";
            statusCol = CLR_WARN;
            break;
        case GameStatus::THREEFOLD:
            line1 = "Draw";
            line2 = "Threefold repetition";
            statusCol = CLR_WARN;
            break;
        case GameStatus::INSUFFICIENT_MATERIAL:
            line1 = "Draw";
            line2 = "Insufficient material";
            statusCol = CLR_WARN;
            break;
    }
    // Squeeze a status line into `avail` pixels so it can never run into the
    // readouts on the right of the panel: smaller type first, then an ellipsis.
    auto fitLine = [&](const std::string& s, int& px, int avail, FontStyle style) {
        while (px > 11 && text.width(s, px, style) > avail) --px;
        if (text.width(s, px, style) <= avail) return s;
        const std::string ell = "\xE2\x80\xA6";  // …
        std::string out = s;
        while (!out.empty() && text.width(out + ell, px, style) > avail) out.pop_back();
        return out.empty() ? ell : out + ell;
    };

    const std::string evalStr = evalText();
    const int evalW = text.width(evalStr, 19, FontStyle::BOLD);
    int line1Px = 21;
    const std::string line1Draw =
        fitLine(line1, line1Px, innerW - evalW - 12, FontStyle::BOLD);
    text.draw(line1Draw, line1Px, statusCol, innerX, 54, 0, FontStyle::BOLD);

    const int infoW =
        engineInfo.empty() ? 0 : text.width(engineInfo, 12, FontStyle::MONO) + 14;
    // While the overlay is running this line belongs to it: what is on the
    // screen matters more than the local game at that point.
    if (ovOn && status == GameStatus::ONGOING && !ovDraw.note.empty()) line2 = ovDraw.note;
    int line2Px = 15;
    const std::string line2Draw =
        fitLine(line2, line2Px, innerW - infoW, FontStyle::REGULAR);
    text.draw(line2Draw, line2Px, CLR_DIM, innerX, 84, 0, FontStyle::REGULAR);
    if (!engineInfo.empty()) text.draw(engineInfo, 12, CLR_DIM, innerX + innerW, 86, 2, FontStyle::MONO);

    // live evaluation readout (White's point of view)
    const bool evalSign = evalCp > 0;
    text.draw(evalStr, 19, evalCp == 0 ? SDL_Color{CLR_DIM.r, CLR_DIM.g, CLR_DIM.b, 255}
                                       : (evalSign ? SDL_Color{235, 238, 240, 255}
                                                   : SDL_Color{CLR_ACCENT.r, CLR_ACCENT.g, CLR_ACCENT.b, 255}),
              innerX + innerW, 50, 2, FontStyle::BOLD);

    // buttons (six rows: the last one holds the screen overlay)
    const int gap = 12;
    const int colW = (innerW - gap) / 2;
    const int btnH = 28, rowGap = 5, y0 = 112;
    static const BtnId ids[6][2] = {{BTN_NEW, BTN_UNDO},
                                    {BTN_AI, BTN_LEVEL},
                                    {BTN_FLIP, BTN_FEN},
                                    {BTN_PAUSE, BTN_SPEED},
                                    {BTN_HINTS, BTN_TOPN},
                                    {BTN_OVERLAY, BTN_SETUP}};
    std::string labels[6][2];
    labels[0][0] = "New game";
    labels[0][1] = "Undo";
    labels[1][0] = aiBoth ? "AI: Both" : (aiEnabled ? (aiColor == BLACK ? "AI: Black" : "AI: White") : "AI: Off");
    labels[1][1] = std::string("Level: ") + LEVELS[level].name;
    labels[2][0] = flipped ? "View: Black" : "View: White";
    labels[2][1] = "Copy FEN";
    labels[3][0] = paused ? "Resume" : "Pause";
    labels[3][1] = std::string("Pace: ") + PACE_NAMES[pace];
    labels[4][0] = hintsOn ? "Best moves: On" : "Best moves: Off";
    labels[4][1] = "Show: " + std::to_string(hintTop) + (hintTop == 1 ? " move" : " moves");
    labels[5][0] = ovOn ? "Overlay: On" : "Overlay: Off";
    labels[5][1] = ovSetup ? "Close overlay menu" : "Overlay setup";

    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 2; ++col) {
            SDL_Rect r{innerX + col * (colW + gap), y0 + row * (btnH + rowGap), colW, btnH};
            bool emph = (ids[row][col] == BTN_UNDO && !stack.empty()) ||
                        (ids[row][col] == BTN_PAUSE && paused) ||
                        (ids[row][col] == BTN_HINTS && hintsOn) ||
                        (ids[row][col] == BTN_OVERLAY && ovOn) ||
                        (ids[row][col] == BTN_SETUP && ovSetup);
            bool dim = (ids[row][col] == BTN_SPEED && !aiBoth) ||
                       (ids[row][col] == BTN_TOPN && !hintsOn);
            drawButton(r, ids[row][col], labels[row][col], emph, dim);
        }
    }

    // capture tray
    const int trayY = 311;
    text.draw("CAPTURED", 12, CLR_DIM, innerX, trayY, 0, FontStyle::BOLD);
    static const int initCount[7] = {0, 8, 2, 2, 2, 1, 0};
    static const PieceType trayOrder[5] = {PAWN, KNIGHT, BISHOP, ROOK, QUEEN};

    auto material = [&](Color c) {
        int total = 0;
        static const int val[7] = {0, 100, 320, 330, 500, 900, 0};
        for (int sq = 0; sq < 64; ++sq)
            if (!isEmpty(pos.at(sq)) && colorOf(pos.at(sq)) == c) total += val[typeOf(pos.at(sq))];
        return total;
    };
    int whiteMat = material(WHITE), blackMat = material(BLACK);

    auto trayRow = [&](int y, Color capturedColor, int advantage, SDL_Color advColor) {
        const int box = 26;
        const int availW = innerW - 44;  // keep the right edge free for the +N label
        int count = 0;
        for (PieceType t : trayOrder)
            count += std::max(0, initCount[t] - pos.countPieces(capturedColor, t));
        // Squeeze the icons when a whole army has been captured so the row always fits.
        const int step = count > 1 ? std::max(6, std::min(box - 6, (availW - box) / (count - 1)))
                                   : box - 6;
        const int bw = count > 1 ? std::min(box, availW - (count - 1) * step) : box;
        int x = innerX;
        for (PieceType t : trayOrder) {
            int missing = initCount[t] - pos.countPieces(capturedColor, t);
            for (int i = 0; i < missing; ++i) {
                SDL_Rect r{x, y, bw, bw};
                guiPiece(renderer, makePiece(t, capturedColor), r);
                x += step;
            }
        }
        if (advantage > 0) {
            text.draw("+" + std::to_string(advantage), 17, advColor, innerX + innerW, y + 2, 2, FontStyle::BOLD);
        }
    };
    trayRow(trayY + 20, BLACK, (whiteMat - blackMat) / 100, CLR_TEXT);
    trayRow(trayY + 54, WHITE, (blackMat - whiteMat) / 100, CLR_TEXT);

    // move list
    const int listHeadY = 404;
    const int statsY = 426;
    const int listY = 448;
    const int lineHeight = 24;
    const int visible = (BOARD_Y + BOARD_PX - listY) / lineHeight;
    const int totalLines = int(sans.size() + 1) / 2;
    int maxScroll = totalLines > visible ? totalLines - visible : 0;
    if (scroll > maxScroll) scroll = maxScroll;

    text.draw("MOVES", 12, CLR_DIM, innerX, listHeadY, 0, FontStyle::BOLD);
    text.draw(std::to_string(sans.size()), 12, CLR_DIM, innerX + innerW, listHeadY, 2, FontStyle::MONO);

    // verdict tally, then a note while the analyst is still working
    {
        const std::string busyText = anBusy ? ("judging" + std::string(1 + int(SDL_GetTicks() / 360) % 3, '.')) : "";
        const int busyW = busyText.empty() ? 0 : text.width(busyText, 12, FontStyle::REGULAR) + 14;
        int x = innerX;
        for (int t = 1; t <= 9 && x < innerX + innerW - busyW; ++t) {
            MoveTag tag = MoveTag(t);
            int n = 0;
            for (MoveTag g : tags)
                if (g == tag) ++n;
            if (!n) continue;
            const std::string sym = tagSymbol(tag);
            const std::string count = std::to_string(n);
            text.draw(sym, 14, tagColor(tag), x, statsY, 0, FontStyle::MONO);
            x += text.width(sym, 14, FontStyle::MONO) + 4;
            text.draw(count, 14, CLR_DIM, x, statsY, 0, FontStyle::MONO);
            x += text.width(count, 14, FontStyle::MONO) + 12;
        }
        if (!busyText.empty())
            text.draw(busyText, 12, CLR_DIM, innerX + innerW, statsY, 2, FontStyle::REGULAR);
    }

    if (sans.empty()) {
        text.draw("(no moves yet)", 14, CLR_DIM, innerX, listY, 0, FontStyle::MONO);
    } else {
        for (int line = scroll; line < totalLines && line - scroll < visible; ++line) {
            const int y = listY + (line - scroll) * lineHeight;
            const int idx = line * 2;
            text.draw(std::to_string(line + 1) + ".", 15, CLR_DIM, innerX, y, 0, FontStyle::MONO);

            // SAN followed by its classification badge, drawn in the badge's colour.
            auto drawPly = [&](int ply, int colX, int limit) {
                const std::string& s = sans[ply];
                const bool last = ply == (int)sans.size() - 1;
                const SDL_Color mc = last ? SDL_Color{CLR_ACCENT.r, CLR_ACCENT.g, CLR_ACCENT.b, 255} : CLR_TEXT;
                text.draw(s, 15, mc, colX, y, 0, FontStyle::MONO);
                if (ply >= (int)tags.size() || tags[ply] == MoveTag::NONE) return;
                const std::string sym = tagSymbol(tags[ply]);
                const int sw = text.width(sym, 15, FontStyle::MONO);
                const int ideal = colX + text.width(s, 15, FontStyle::MONO) + 6;
                text.draw(sym, 15, tagColor(tags[ply]), std::min(ideal, limit - sw), y, 0,
                          FontStyle::MONO);
            };

            drawPly(idx, innerX + 44, innerX + 140);
            if (idx + 1 < (int)sans.size()) drawPly(idx + 1, innerX + 144, innerX + innerW);
        }
        if (maxScroll > 0)
            text.draw("scroll", 11, CLR_DIM, innerX + innerW, listY + visible * lineHeight + 2, 2,
                      FontStyle::REGULAR);
    }
}

void App::drawPromo() {
    const int cellW = 64, cellH = 52;
    SDL_Rect d = squareRect(promoTo);
    int sx = d.x + (SQ - cellW) / 2;
    int sy = (rankOf(promoTo) == 7) ? d.y : d.y + SQ - 4 * cellH;

    static const PieceType kinds[4] = {QUEEN, ROOK, BISHOP, KNIGHT};

    SDL_Rect back{sx - 6, sy - 6, cellW + 12, 4 * cellH + 12};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 240);
    SDL_RenderFillRect(renderer, &back);
    SDL_SetRenderDrawColor(renderer, CLR_ACCENT.r, CLR_ACCENT.g, CLR_ACCENT.b, 200);
    SDL_RenderDrawRect(renderer, &back);

    for (int i = 0; i < 4; ++i) {
        SDL_Rect c{sx, sy + i * cellH, cellW, cellH};
        bool hover = mouse.x >= c.x && mouse.x < c.x + c.w && mouse.y >= c.y && mouse.y < c.y + c.h;
        if (hover) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60);
            SDL_RenderFillRect(renderer, &c);
        }
        SDL_Rect piece = {c.x + 4, c.y + 2, cellW - 8, cellH - 4};
        guiPiece(renderer, makePiece(kinds[i], pos.side), piece);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// Vertical evaluation bar in the 16px gutter between board and panel.
// White's share grows upward; the value eases toward the latest verdict.
void App::drawEvalBar() {
    const int barX = BOARD_X + BOARD_PX + 3;
    const int barW = 10;
    const int barY = BOARD_Y + 1;
    const int barH = BOARD_PX - 2;

    // Ease the displayed value toward the target so the bar glides, not jumps.
    if (snapshot) dispEval = double(evalCp);
    else dispEval += (double(evalCp) - dispEval) * 0.12;

    double cp = dispEval;
    const double mag = std::abs(cp);
    double share;
    if (mag >= 99000) {
        share = cp > 0 ? 1.0 : 0.0;  // mate: fill completely
    } else {
        share = 0.5 + 0.5 * std::tanh(cp / 450.0);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_Rect track{barX, barY, barW, barH};
    SDL_SetRenderDrawColor(renderer, 12, 12, 14, 255);
    SDL_RenderFillRect(renderer, &track);

    // White's share grows from White's own end of the board (bottom normally,
    // top when the view is flipped), so the bar always mirrors the pieces.
    const int split = int(share * barH);
    const int whiteY = flipped ? barY : barY + barH - split;
    if (split > 0) {
        SDL_Rect white{barX, whiteY, barW, split};
        SDL_SetRenderDrawColor(renderer, CLR_LIGHT.r, CLR_LIGHT.g, CLR_LIGHT.b, 255);
        SDL_RenderFillRect(renderer, &white);
    }
    if (split < barH) {
        SDL_Rect black{barX, flipped ? barY + split : barY, barW, barH - split};
        SDL_SetRenderDrawColor(renderer, 46, 46, 52, 255);
        SDL_RenderFillRect(renderer, &black);
    }

    // centre tick marks every rank, plus a stronger mid-line
    SDL_SetRenderDrawColor(renderer, 24, 25, 28, 255);
    for (int i = 1; i < 8; ++i) {
        int y = barY + i * barH / 8;
        SDL_RenderDrawLine(renderer, barX, y, barX + barW, y);
    }
    SDL_SetRenderDrawColor(renderer, 126, 200, 120, 200);
    SDL_RenderDrawLine(renderer, barX, barY + barH / 2, barX + barW, barY + barH / 2);

    SDL_SetRenderDrawColor(renderer, 12, 12, 14, 255);
    SDL_RenderDrawRect(renderer, &track);
}

// Full-board result banner shown once a game ends.
void App::drawBanner() {
    if (status == GameStatus::ONGOING) return;

    std::string title, sub;
    switch (status) {
        case GameStatus::CHECKMATE:
            title = (pos.side == WHITE) ? "BLACK WINS" : "WHITE WINS";
            sub = "by checkmate";
            break;
        case GameStatus::STALEMATE:
            title = "DRAW"; sub = "stalemate"; break;
        case GameStatus::FIFTY_MOVE:
            title = "DRAW"; sub = "fifty move rule"; break;
        case GameStatus::THREEFOLD:
            title = "DRAW"; sub = "threefold repetition"; break;
        default:
            title = "DRAW"; sub = "insufficient material"; break;
    }

    // Pulse so the banner feels alive.
    const float pulse = 0.5f + 0.5f * std::sin(float(SDL_GetTicks() - gameOverAt) / 300.f);
    const Uint8 a = Uint8(150 + 70 * pulse);

    SDL_Rect band{BOARD_X, BOARD_Y + BOARD_PX / 2 - 74, BOARD_PX, 148};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 12, 12, 14, a);
    SDL_RenderFillRect(renderer, &band);
    SDL_SetRenderDrawColor(renderer, CLR_ACCENT.r, CLR_ACCENT.g, CLR_ACCENT.b, a);
    SDL_RenderDrawRect(renderer, &band);

    SDL_Color tc{235, 238, 240, a};
    SDL_Color sc{CLR_ACCENT.r, CLR_ACCENT.g, CLR_ACCENT.b, a};
    text.draw(title, 44, status == GameStatus::CHECKMATE ? sc : tc,
              band.x + band.w / 2, band.y + 30, 1, FontStyle::BOLD);
    text.draw(sub, 17, SDL_Color{CLR_DIM.r, CLR_DIM.g, CLR_DIM.b, a},
              band.x + band.w / 2, band.y + 92, 1, FontStyle::REGULAR);

    if (aiBoth) {
        std::string msg;
        if (paused) {
            msg = "paused - press Resume";
        } else {
            const int left = 4 - int(SDL_GetTicks() - gameOverAt) / 1000;
            msg = left > 0 ? ("rematch in " + std::to_string(left) + "s") : "restarting...";
        }
        text.draw(msg, 14, SDL_Color{CLR_DIM.r, CLR_DIM.g, CLR_DIM.b, a},
                  band.x + band.w / 2, band.y + 118, 1, FontStyle::MONO);
    } else {
        text.draw("press N for a new game", 14,
                  SDL_Color{CLR_DIM.r, CLR_DIM.g, CLR_DIM.b, a},
                  band.x + band.w / 2, band.y + 118, 1, FontStyle::MONO);
    }
}

// chess.com-style verdict: a badge that floats over the board for a moment.
void App::drawToast() {
    if (toastTag == MoveTag::NONE) return;
    const Uint32 age = SDL_GetTicks() - toastAt;
    if (age > 2300) return;

    int alpha = 255;
    int dy = 0;
    if (!snapshot) {  // snapshots are a single frame: show it at full strength
        if (age < 200) {
            alpha = int(age) * 255 / 200;
            dy = -((200 - int(age)) * 14) / 200;
        } else if (age > 2000) {
            alpha = int(2300 - age) * 255 / 300;
        }
    }

    const std::string sym = tagSymbol(toastTag);
    const std::string name = tagName(toastTag);
    const SDL_Color c = tagColor(toastTag);

    const int w = 340, h = 78;
    const int x = BOARD_X + (BOARD_PX - w) / 2;
    const int y = BOARD_Y + 34 + dy;

    SDL_Rect box{x, y, w, h};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 15, 16, 19, (Uint8)(235 * alpha / 255));
    SDL_RenderFillRect(renderer, &box);
    for (int i = 0; i < 3; ++i) {  // thick coloured frame
        SDL_Rect r{x + i, y + i, w - 2 * i, h - 2 * i};
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, (Uint8)((i == 0 ? 255 : 120) * alpha / 255));
        SDL_RenderDrawRect(renderer, &r);
    }

    const SDL_Color symCol{c.r, c.g, c.b, (Uint8)alpha};
    const SDL_Color white{255, 255, 255, (Uint8)alpha};
    text.draw(sym, 44, symCol, x + 22, y + 13, 0, FontStyle::BOLD);
    const int tx = x + 22 + text.width(sym, 44, FontStyle::BOLD) + 20;
    text.draw(name, 23, white, tx, y + 13, 0, FontStyle::BOLD);
    text.draw(toastSan, 15, symCol, tx, y + 46, 0, FontStyle::MONO);
}

void App::draw(bool present) {
    pollOverlay();
    SDL_SetRenderDrawColor(renderer, CLR_BG.r, CLR_BG.g, CLR_BG.b, 255);
    SDL_RenderClear(renderer);
    drawBoard();
    drawEvalBar();
    drawPanel();
    drawOverlayMarker();
    if (promoOpen) drawPromo();
    if (status != GameStatus::ONGOING && !promoOpen) drawBanner();
    drawToast();
    if (ovSetup) drawOverlayMenu();
    if (present) SDL_RenderPresent(renderer);
    drawOverlayWindow();  // a window of its own: painted once this frame is up
}

// --------------------------------------------------------- perft / cli -----

static uint64_t perft(Position& pos, int depth) {
    if (depth <= 0) return 1;
    std::vector<Move> moves = pos.legalMoves();
    if (depth == 1) return moves.size();
    uint64_t nodes = 0;
    for (const Move& m : moves) {
        UndoInfo u;
        pos.doMove(m, u);
        nodes += perft(pos, depth - 1);
        pos.undoMove(m, u);
    }
    return nodes;
}

static bool moveFromCoord(Position& pos, const std::string& text, Move& out) {
    if (text.size() < 4) return false;
    int from = makeSquare(text[0] - 'a', text[1] - '1');
    int to = makeSquare(text[2] - 'a', text[3] - '1');
    PieceType promo = NO_PIECE;
    if (text.size() >= 5) {
        switch (text[4]) {
            case 'q': case 'Q': promo = QUEEN; break;
            case 'r': case 'R': promo = ROOK; break;
            case 'b': case 'B': promo = BISHOP; break;
            case 'n': case 'N': promo = KNIGHT; break;
            default: break;
        }
    }
    for (const Move& m : pos.legalMoves()) {
        if (m.from == from && m.to == to && m.promotion == promo) {
            out = m;
            return true;
        }
    }
    return false;
}

static bool isExecutable(const std::string& p) {
    return !p.empty() && ::access(p.c_str(), X_OK) == 0;
}

static std::string parentDir(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

static std::string findOnPath(const std::string& exe) {
    const char* env = std::getenv("PATH");
    if (!env) return "";
    std::string paths(env);
    size_t i = 0;
    for (;;) {
        size_t j = paths.find(':', i);
        if (j == std::string::npos) j = paths.size();
        std::string dir = paths.substr(i, j - i);
        if (dir.empty()) dir = ".";
        if (isExecutable(dir + "/" + exe)) return dir + "/" + exe;
        if (j == paths.size()) break;
        i = j + 1;
    }
    return "";
}

// --engine, then $STOCKFISH, then the copy built inside the project (relative
// to this executable, then to the working directory), then $PATH.
static std::string findStockfish(const std::string& flagPath, const char* argv0) {
    if (!flagPath.empty()) return isExecutable(flagPath) ? flagPath : std::string();
    const char* env = std::getenv("STOCKFISH");
    if (env && isExecutable(env)) return env;
    static const char* kRel[] = {"bin/stockfish", "stockfish/src/stockfish"};
    const std::string dir = argv0 ? parentDir(argv0) : std::string();
    if (!dir.empty())
        for (const char* r : kRel)
            if (isExecutable(dir + "/" + r)) return dir + "/" + r;
    for (const char* r : kRel)
        if (isExecutable(r)) return r;
    const std::string onPath = findOnPath("stockfish");
    if (!onPath.empty()) return onPath;
    return isExecutable("/usr/games/stockfish") ? std::string("/usr/games/stockfish") : std::string();
}

static void printUsage(const char* argv0) {
    std::printf("Usage: %s [options]\n", argv0);
    std::printf("  --fen <fen>       start from the given position\n");
    std::printf("  --moves <list>    apply coordinate moves, e.g. \"e2e4 e7e5 g1f3\"\n");
    std::printf("  --flip            show the board from Black's side\n");
    std::printf("  --watch           start in AI vs AI mode (both engines play)\n");
    std::printf("  --plies <n>       play n half-moves (with --watch), print the moves and exit;\n");
    std::printf("                    intended for headless testing, e.g.\n");
    std::printf("                    SDL_VIDEODRIVER=dummy ./chess --watch --plies 12\n");
    std::printf("  --level <0|1|2>   engine strength: 0 Easy, 1 Medium, 2 Hard\n");
    std::printf("  --engine <path>   UCI engine to play and judge with (default: a Stockfish\n");
    std::printf("                    build found via $STOCKFISH, ./bin/stockfish or $PATH)\n");
    std::printf("  --builtin         ignore Stockfish and use the engine compiled in\n");
    std::printf("  --hints           show the strongest moves: ranked arrows on the board\n");
    std::printf("                    plus the list they are numbered from (H while running)\n");
    std::printf("  --top <1-5>       how many moves the hints show (default 3)\n");
    std::printf("  --overlay         watch a chess board elsewhere on this screen and draw\n");
    std::printf("                    the best move over it (O toggles, Shift+O opens its menu)\n");
    std::printf("  --snap <file>     render one frame to <file>.bmp and exit\n");
    std::printf("  --analyze         judge the --moves given to a snapshot (adds badges and\n");
    std::printf("                    the verdict banner, but the snapshot takes a moment longer)\n");
    std::printf("  --perft <depth>   run a move generation benchmark and exit\n");
    std::printf("  --select <sq>     highlight a square's moves in a snapshot (e.g. e2)\n");
    std::printf("  --promo           open the promotion picker in a snapshot\n");
    std::printf("  --help            show this message\n");
}

int main(int argc, char** argv) {
    // Xlib gets capture and the shaped overlay window, both on threads of
    // their own, and it is only safe when this comes first.
    x11InitThreads();

    std::string fen = START_FEN;
    std::string movesArg;
    std::string snapPath;
    std::string selectArg;
    bool flip = false;
    bool watch = false;
    int plies = -1;
    int levelArg = -1;
    bool promoArg = false;
    bool analyzeArg = false;
    bool builtinArg = false;
    bool hintsArg = false;
    int topArg = 0;           // --top n; 0 keeps the default
    bool overlayArg = false;  // --overlay: watch a board elsewhere on screen
    std::string engineArg;
    int perftDepth = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--fen") fen = next();
        else if (a == "--moves") movesArg = next();
        else if (a == "--snap") snapPath = next();
        else if (a == "--flip") flip = true;
        else if (a == "--watch") watch = true;
        else if (a == "--plies") plies = std::atoi(next());
        else if (a == "--level") levelArg = std::atoi(next());
        else if (a == "--select") selectArg = next();
        else if (a == "--promo") promoArg = true;
        else if (a == "--analyze") analyzeArg = true;
        else if (a == "--builtin") builtinArg = true;
        else if (a == "--hints") hintsArg = true;
        else if (a == "--top") topArg = std::atoi(next());
        else if (a == "--overlay") overlayArg = true;
        else if (a == "--engine") engineArg = next();
        else if (a == "--perft") perftDepth = std::atoi(next());
        else if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "chess: unknown option '%s'\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    // ---- headless move generation test ----
    if (perftDepth >= 0) {
        Position pos;
        pos.setFen(fen);
        for (int d = 1; d <= perftDepth; ++d) {
            Position p = pos;
            uint64_t n = perft(p, d);
            std::printf("perft(%d) = %llu\n", d, (unsigned long long)n);
        }
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "chess: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const bool hidden = !snapPath.empty();
    SDL_Window* window = SDL_CreateWindow("Chess", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H,
                                          hidden ? SDL_WINDOW_HIDDEN : 0);
    if (!window) {
        std::fprintf(stderr, "chess: window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    App app;
    app.window = window;
    app.renderer = SDL_CreateRenderer(window, -1, hidden ? SDL_RENDERER_SOFTWARE : (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
    if (!app.renderer) app.renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!app.renderer) {
        std::fprintf(stderr, "chess: renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);

    // The standard piece images, when they are where they should be: our own
    // window then draws those instead of the vector pieces (art.cpp). With no
    // complete set on disk the drawn pieces stay, and nothing else changes.
    if (artLoad(app.renderer, nullptr))
        std::printf("chess: pieces: image set loaded (%s)\n",
                    std::getenv("PIECES") && *std::getenv("PIECES") ? std::getenv("PIECES")
                                                                    : "pieces");
    else
        std::printf("chess: pieces: no complete image set found - drawn pieces\n");

    if (!app.text.init(app.renderer)) {
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    app.flipped = flip;
    app.snapshot = hidden;
    app.animate = !hidden;   // CLI snapshots render the settled position
    // Interactive runs judge every move on a background thread; snapshots only
    // do it when asked, and then inline so the frame can show the verdict.
    app.analyze = !hidden || analyzeArg;
    app.analyzeSync = hidden && analyzeArg;
    // Hints are off by default so nothing searches until they are asked for.
    app.hintsOn = hintsArg;
    if (topArg > 0) app.hintTop = std::min(topArg, HINT_MAX);

    // Stockfish, when there is one: one process plays the moves, a second one
    // judges them, a third ranks the hinted moves. Without it the engine
    // compiled into the app does all three.
    const bool wantPlay = !hidden;
    const bool wantJudge = app.analyze;
    std::string sfPath;
    if (!builtinArg && (wantPlay || wantJudge || app.hintsOn || overlayArg))
        sfPath = findStockfish(engineArg, argv[0]);
    if (sfPath.empty()) {
        const bool looked = !builtinArg && (wantPlay || wantJudge);  // said so before, still does
        if (!engineArg.empty())
            std::fprintf(stderr, "chess: no executable engine at '%s'\n", engineArg.c_str());
        else if (looked)
            std::printf("chess: Stockfish not found\n");
        if (looked && wantPlay) std::fprintf(stderr, "chess: the built-in engine will play\n");
        if (looked && wantJudge)
            std::fprintf(stderr, "chess: the built-in analysis will judge the moves\n");
        if (app.hintsOn && !builtinArg)
            std::fprintf(stderr, "chess: the built-in engine will rank the hints\n");
    } else {
        app.sfPath = sfPath;  // the hint worker starts its own copy from here
        if (wantPlay) app.sfPlay.start(sfPath, 1, 128);
        if (wantJudge) app.sfJudge.start(sfPath, JUDGE_THREADS, JUDGE_HASH_MB);
        if (app.sfPlay.alive() && app.sfJudge.alive())
            std::printf("chess: %s plays and judges (%s)\n", app.sfPlay.engineName().c_str(),
                        sfPath.c_str());
        else if (app.sfPlay.alive())
            std::printf("chess: %s plays (%s)\n", app.sfPlay.engineName().c_str(), sfPath.c_str());
        else if (app.sfJudge.alive())
            std::printf("chess: %s judges (%s)\n", app.sfJudge.engineName().c_str(), sfPath.c_str());
        if (wantPlay && !app.sfPlay.alive())
            std::fprintf(stderr, "chess: the built-in engine will play\n");
        if (wantJudge && !app.sfJudge.alive())
            std::fprintf(stderr, "chess: the built-in analysis will judge the moves\n");
        // Read once, now, while no engine thread is running yet: the panel
        // shows this in its subtitle for the rest of the run.
        app.engineName = app.sfPlay.alive() ? app.sfPlay.engineName()
                         : (app.sfJudge.alive() ? app.sfJudge.engineName() : std::string());
    }

    if (app.analyze && !app.analyzeSync) app.startAnalysis();
    app.aiBoth = watch;
    if (watch) app.aiEnabled = true;
    if (levelArg >= 0) app.level = levelArg % 3;
    if (plies >= 0) app.pace = 2;  // don't slow the headless run down
    app.pos.setFen(fen);
    app.lastFrom = app.lastTo = -1;

    // Apply scripted moves (coordinate notation, space or comma separated).
    if (!movesArg.empty()) {
        app.refresh();
        std::string list;
        for (char c : movesArg) list += (c == ',') ? ' ' : c;
        size_t i = 0;
        while (i < list.size()) {
            while (i < list.size() && list[i] == ' ') ++i;
            size_t start = i;
            while (i < list.size() && list[i] != ' ') ++i;
            std::string tok = list.substr(start, i - start);
            if (tok.empty()) continue;
            Move m;
            if (!moveFromCoord(app.pos, tok, m)) {
                std::fprintf(stderr, "chess: illegal or malformed move '%s'\n", tok.c_str());
                break;
            }
            app.applyMove(m);  // keeps the move list, badges and animation in step
        }
    }
    app.refresh();
    app.stopAi();  // snapshot runs never search
    {
        const int e = Engine::evaluate(app.pos);
        app.evalCp = app.pos.side == WHITE ? e : -e;
    }

    // Debug aids for snapshots: pre-select a square / open the promotion picker.
    if (selectArg.size() >= 2 && selectArg[0] >= 'a' && selectArg[0] <= 'h' && selectArg[1] >= '1' &&
        selectArg[1] <= '8') {
        app.selectSquare(makeSquare(selectArg[0] - 'a', selectArg[1] - '1'));
    }
    if (promoArg) {
        for (const Move& m : app.legal) {
            if (m.promotion != NO_PIECE) {
                app.promoOpen = true;
                app.promoFrom = m.from;
                app.promoTo = m.to;
                break;
            }
        }
    }

    // Watching a board elsewhere on this screen: started only when asked for,
    // so nothing is ever captured from the desktop behind the app's back.
    if (overlayArg && !hidden) {
        const std::string problem = captureProblem();
        if (!problem.empty()) std::fprintf(stderr, "chess: overlay: %s\n", problem.c_str());
        app.startOverlay();
    }

    if (hidden) {
        if (app.hintsOn) app.hintsSync();  // no worker in a snapshot: search inline
        app.draw(false);
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, WIN_W, WIN_H, 32, SDL_PIXELFORMAT_ARGB8888);
        if (surf) {
            if (SDL_RenderReadPixels(app.renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch) == 0)
                SDL_SaveBMP(surf, snapPath.c_str());
            else
                std::fprintf(stderr, "chess: RenderReadPixels failed: %s\n", SDL_GetError());
            SDL_FreeSurface(surf);
        }
        app.stopOverlay();
        app.stopAnalysis();
        app.stopHints();
        app.sfPlay.quit();
        app.sfJudge.quit();
        app.text.shutdown();
        artShutdown();
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    bool running = true;
    int gameNo = 1;
    size_t finishedPlies = 0;   // plies of already-reported games
    bool reported = false;
    auto reportGame = [&](size_t pliesInGame) {
        app.drainAnalysis(8000);  // let the badges land before reporting
        auto badge = [&](size_t ply) -> std::string {
            return ply < app.tags.size() ? tagSymbol(app.tags[ply]) : std::string();
        };
        const char* result = "unfinished";
        if (app.status == GameStatus::CHECKMATE)
            result = (app.pos.side == WHITE) ? "0-1" : "1-0";
        else if (app.status != GameStatus::ONGOING)
            result = "1/2-1/2";
        std::printf("game %d [%s]:", gameNo++, result);
        for (size_t i = 0; i < app.sans.size(); i += 2) {
            std::printf(" %zu. %s%s", i / 2 + 1, app.sans[i].c_str(), badge(i).c_str());
            if (i + 1 < app.sans.size())
                std::printf(" %s%s", app.sans[i + 1].c_str(), badge(i + 1).c_str());
        }
        std::printf("  (%zu plies)\n", pliesInGame);
    };

    while (running) {
        app.tick();
        app.draw(true);

        if (plies >= 0) {
            if (app.status != GameStatus::ONGOING && !reported) {
                reportGame(app.sans.size());
                finishedPlies += app.sans.size();
                reported = true;
            }
            if (app.status == GameStatus::ONGOING) reported = false;
            const size_t played = finishedPlies + (reported ? 0 : app.sans.size());
            // No auto-rematch outside watch mode: stop once the game is over.
            if (played >= (size_t)plies) running = false;
            if (reported && !app.aiBoth) running = false;
        }

        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 16)) {
            do {
                if (ev.type == SDL_QUIT) running = false;
                app.handleEvent(ev);
            } while (SDL_PollEvent(&ev));
        }
    }

    if (plies >= 0) {
        if (!reported && !app.sans.empty()) reportGame(app.sans.size());
        std::printf("(total %zu plies)\n", finishedPlies + (reported ? 0 : app.sans.size()));
    }

    app.stopAi();
    app.stopOverlay();
    app.stopAnalysis();
    app.stopHints();
    app.sfPlay.quit();
    app.sfJudge.quit();
    app.text.shutdown();
    artShutdown();
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
