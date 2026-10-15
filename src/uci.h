#pragma once
// uci.h -- UCI client for an external engine (Stockfish) run in a child process
#include "engine.h"
#include "position.h"

#include <atomic>
#include <string>
#include <vector>

// Spawns a UCI engine and talks to it over pipes. UCI is a strictly sequential
// protocol, so an instance is driven by exactly one thread: the app keeps one
// for the moves it plays and a second one for the moves it judges.
class Stockfish {
public:
    Stockfish() = default;
    ~Stockfish() { quit(); }
    Stockfish(const Stockfish&) = delete;
    Stockfish& operator=(const Stockfish&) = delete;

    // Spawn <path> and complete the UCI handshake; false if it did not answer.
    bool start(const std::string& path, int threads = 1, int hashMb = 64);
    bool alive() const { return pid > 0 && !gone; }
    const std::string& engineName() const { return name; }
    void setThreads(int threads);
    // Send any UCI option (e.g. "UCI_LimitStrength", "UCI_Elo") and wait for
    // the engine to confirm it is ready for the next search. `value` may be
    // empty for options that take no value.
    bool setOption(const std::string& name, const std::string& value);
    void quit();

    // One move: `depth` and `movetimeMs`, whichever runs out first.
    bool search(const Position& pos, int depth, int movetimeMs, std::atomic<bool>* cancel,
                SearchResult& out);

    // Score every legal move of `before` in a single MultiPV search so that
    // `played` can be judged against the strongest alternative.
    bool assess(const Position& before, const Move& played, int depth, int movetimeMs,
                std::atomic<bool>* cancel, Assessment& out);

    // The `count` strongest moves of `pos` from one MultiPV search, strongest
    // first, each with the score it was given. Stops early (with whatever it
    // has found so far) when `cancel` is set.
    bool bestMoves(const Position& pos, int depth, int movetimeMs, int count,
                   std::atomic<bool>* cancel, std::vector<ScoredMove>& out);

    static std::string uciMove(const Move& m);
    static bool parseUci(const Position& pos, const std::string& text, Move& out);

private:
    struct Info {
        int depth = 0;
        long long nodes = 0;
        int score = 0;
        bool hasScore = false;
    };

    static bool parseInfo(const std::string& line, Info& info, std::string* pvFirst, int* multipv);

    // One "root move" line out of a MultiPV search: the first move of its PV,
    // the score, which PV slot it came from and the depth that reported it.
    struct RootScore {
        std::string pv0;
        int score = 0;
        int multipv = 1;
        int depth = 0;
    };

    // Reads output until "bestmove". Returns false if the engine died or the
    // budget ran out. Optional sinks collect the last exact info line, every
    // root-move line seen, and the bestmove itself.
    bool waitForBestmove(int budgetMs, std::atomic<bool>* cancel, Info* lastInfo,
                         std::vector<RootScore>* rootScores, std::string* bestmove);

    bool send(const std::string& line);
    int readLine(std::string& out, int timeoutMs);  // 1 line, 0 timeout, -1 gone
    bool readUntil(const std::string& token, int timeoutMs);

    int inFd = -1;    // to the engine's stdin
    int outFd = -1;   // from the engine's stdout
    long pid = -1;
    bool gone = false;
    std::string buf;  // partial line waiting to be handed out
    std::string name = "engine";
};
