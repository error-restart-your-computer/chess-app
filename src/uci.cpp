#include "uci.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iterator>
#include <map>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        size_t start = i;
        while (i < s.size() && s[i] != ' ') ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

int elapsedMs(Clock::time_point t0) {
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// Stockfish reports scores from the side to move's point of view, in
// centipawns or in *moves* to mate. Everything the classifier works with is in
// centipawns on our own scale, where a mate in one ply is MATE_SCORE, so
// "mate in m moves" becomes "mate in 2m-1 plies" -- the same convention the
// built-in engine uses.
int mapScore(const std::string& kind, long long v) {
    if (kind == "cp") return (int)v;
    if (kind != "mate") return 0;
    if (v == 0) return 0;  // no forced mate
    long long m = std::min(std::llabs(v), 500LL);
    const int plies = (int)(2 * m - 1);
    const int mag = MATE_SCORE - plies;
    return v > 0 ? mag : -mag;
}

}  // namespace

// --------------------------------------------------------------------------

std::string Stockfish::uciMove(const Move& m) {
    std::string s = squareName(m.from);
    s += squareName(m.to);
    if (m.promotion != NO_PIECE) {
        char c = pieceLetter(m.promotion);
        s += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return s;
}

bool Stockfish::parseUci(const Position& pos, const std::string& text, Move& out) {
    if (text.size() < 4) return false;
    if (text[0] < 'a' || text[0] > 'h' || text[1] < '1' || text[1] > '8') return false;
    if (text[2] < 'a' || text[2] > 'h' || text[3] < '1' || text[3] > '8') return false;
    const int from = makeSquare(text[0] - 'a', text[1] - '1');
    const int to = makeSquare(text[2] - 'a', text[3] - '1');
    PieceType promo = NO_PIECE;
    if (text.size() >= 5) {
        switch (text[4] | 32) {
            case 'q': promo = QUEEN; break;
            case 'r': promo = ROOK; break;
            case 'b': promo = BISHOP; break;
            case 'n': promo = KNIGHT; break;
            default: return false;
        }
    }
    Position copy = pos;  // legalMoves() is not const
    for (const Move& m : copy.legalMoves())
        if (m.from == from && m.to == to && m.promotion == promo) {
            out = m;
            return true;
        }
    return false;
}

// Pulls depth/nodes/score (and the first move of the PV) out of an "info"
// line. Returns true only when the line carried a usable score plus a PV --
// "currmove" lines and bound-only lines are not worth judging from.
bool Stockfish::parseInfo(const std::string& line, Info& info, std::string* pvFirst, int* multipv) {
    const std::vector<std::string> t = split(line);
    bool ok = false;
    info = Info();
    for (size_t i = 1; i < t.size(); ++i) {
        const std::string& k = t[i];
        if (k == "depth" && i + 1 < t.size()) {
            info.depth = std::atoi(t[++i].c_str());
        } else if (k == "nodes" && i + 1 < t.size()) {
            info.nodes = std::atoll(t[++i].c_str());
        } else if (k == "multipv" && i + 1 < t.size()) {
            if (multipv) *multipv = std::atoi(t[++i].c_str());
            else ++i;
        } else if (k == "score" && i + 2 < t.size()) {
            const std::string& kind = t[i + 1];
            if (kind == "cp" || kind == "mate") {
                info.score = mapScore(kind, std::atoll(t[i + 2].c_str()));
                info.hasScore = true;
                ok = true;
            }
            i += 2;
        } else if (k == "pv" && pvFirst && i + 1 < t.size()) {
            *pvFirst = t[i + 1];
            break;  // everything after this is moves of the PV
        }
    }
    return ok;
}

// --------------------------------------------------------------------------

bool Stockfish::send(const std::string& line) {
    if (inFd < 0 || gone) return false;
    const std::string out = line + "\n";
    size_t off = 0;
    while (off < out.size()) {
        ssize_t n = ::write(inFd, out.data() + off, out.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            gone = true;  // EPIPE: the engine is gone
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

int Stockfish::readLine(std::string& out, int timeoutMs) {
    if (outFd < 0) return -1;
    for (;;) {
        size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            out = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return 1;
        }
        struct pollfd p;
        p.fd = outFd;
        p.events = POLLIN;
        p.revents = 0;
        int r = ::poll(&p, 1, timeoutMs);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            gone = true;
            return -1;
        }
        char tmp[8192];
        ssize_t n = ::read(outFd, tmp, sizeof(tmp));
        if (n <= 0) {
            gone = true;  // EOF or error: the engine stopped talking
            return -1;
        }
        buf.append(tmp, (size_t)n);
    }
}

bool Stockfish::readUntil(const std::string& token, int timeoutMs) {
    const Clock::time_point t0 = Clock::now();
    std::string line;
    while (elapsedMs(t0) < timeoutMs) {
        int r = readLine(line, 250);
        if (r < 0) return false;
        if (r == 0) continue;
        if (line.rfind("id name ", 0) == 0) name = line.substr(8);
        if (line.rfind(token, 0) == 0) return true;
    }
    return false;
}

// --------------------------------------------------------------------------

bool Stockfish::start(const std::string& path, int threads, int hashMb) {
    if (pid > 0) return true;
    // A dead engine must not take the app down with it (EPIPE -> SIGPIPE).
    std::signal(SIGPIPE, SIG_IGN);

    int toChild[2] = {-1, -1};
    int fromChild[2] = {-1, -1};
    if (::pipe(toChild) != 0 || ::pipe(fromChild) != 0) {
        for (int fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]})
            if (fd >= 0) ::close(fd);
        return false;
    }

    const long p = (long)::fork();
    if (p < 0) {
        for (int fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]}) ::close(fd);
        return false;
    }
    if (p == 0) {
        ::dup2(toChild[0], STDIN_FILENO);
        ::dup2(fromChild[1], STDOUT_FILENO);
        ::close(toChild[0]);
        ::close(toChild[1]);
        ::close(fromChild[0]);
        ::close(fromChild[1]);
        ::execl(path.c_str(), path.c_str(), (char*)nullptr);
        _exit(127);
    }

    ::close(toChild[0]);
    ::close(fromChild[1]);
    inFd = toChild[1];
    outFd = fromChild[0];
    pid = p;

    if (!send("uci") || !readUntil("uciok", 8000)) {
        quit();
        return false;
    }
    send("setoption name Hash value " + std::to_string(std::max(1, hashMb)));
    setThreads(threads);
    if (!send("isready") || !readUntil("readyok", 8000)) {
        quit();
        return false;
    }
    return true;
}

void Stockfish::setThreads(int threads) {
    if (!alive()) return;
    send("setoption name Threads value " + std::to_string(std::max(1, threads)));
}

bool Stockfish::setOption(const std::string& optName, const std::string& value) {
    if (!alive()) return false;
    const std::string line =
        "setoption name " + optName + (value.empty() ? "" : " value " + value);
    if (!send(line)) return false;
    // Options take effect before the next search; isready pins that down so a
    // caller can switch settings between games without racing the engine.
    return send("isready") && readUntil("readyok", 4000);
}

void Stockfish::quit() {
    if (inFd >= 0) {
        ssize_t ignored = ::write(inFd, "quit\n", 5);
        (void)ignored;
    }
    if (pid > 0) {
        int status = 0;
        for (int i = 0; i < 100; ++i) {  // give it a second to leave politely
            pid_t r = ::waitpid((pid_t)pid, &status, WNOHANG);
            if (r == (pid_t)pid || r < 0) {
                pid = -1;
                break;
            }
            ::usleep(10000);
        }
        if (pid > 0) {
            ::kill((pid_t)pid, SIGKILL);
            ::waitpid((pid_t)pid, &status, 0);
            pid = -1;
        }
    }
    if (inFd >= 0) {
        ::close(inFd);
        inFd = -1;
    }
    if (outFd >= 0) {
        ::close(outFd);
        outFd = -1;
    }
    buf.clear();
    gone = false;
}

// --------------------------------------------------------------------------

bool Stockfish::waitForBestmove(int budgetMs, std::atomic<bool>* cancel, Info* lastInfo,
                                std::vector<RootScore>* rootScores, std::string* bestmove) {
    const Clock::time_point t0 = Clock::now();
    const int hardBudget = budgetMs + 6000;  // how long to wait for a hung engine
    bool stopSent = false;
    std::string line;

    for (;;) {
        int r = readLine(line, 200);
        if (r < 0) return false;
        if (r == 1) {
            if (line.rfind("bestmove", 0) == 0) {
                if (bestmove) {
                    std::vector<std::string> t = split(line);
                    *bestmove = t.size() >= 2 ? t[1] : std::string();
                }
                return true;
            }
            if (line.rfind("info ", 0) == 0 && line.find("upperbound") == std::string::npos &&
                line.find("lowerbound") == std::string::npos) {
                Info info;
                std::string pv0;
                int mpv = 1;  // absent when the engine runs a single PV
                if (parseInfo(line, info, &pv0, &mpv) && info.hasScore && !pv0.empty()) {
                    if (lastInfo) *lastInfo = info;
                    if (rootScores) rootScores->push_back({pv0, info.score, mpv, info.depth});
                }
            }
        }
        if (cancel && !stopSent && cancel->load()) {
            send("stop");
            stopSent = true;
        }
        if (r == 0) {
            const int el = elapsedMs(t0);
            if (!stopSent && el > budgetMs + 1500) {  // overran its own movetime
                send("stop");
                stopSent = true;
            }
            if (el > hardBudget) return false;
        }
    }
}

bool Stockfish::search(const Position& pos, int depth, int movetimeMs, std::atomic<bool>* cancel,
                       SearchResult& out) {
    out = SearchResult();
    if (!alive()) return false;
    if (!send("setoption name MultiPV value 1")) return false;
    if (!send("position fen " + pos.toFen())) return false;
    if (!send("go depth " + std::to_string(depth) + " movetime " + std::to_string(movetimeMs)))
        return false;

    Info last;
    std::string bm;
    if (!waitForBestmove(movetimeMs, cancel, &last, nullptr, &bm)) return false;

    Move m;
    out.found = parseUci(pos, bm, m);
    out.move = m;
    out.score = last.score;
    out.depth = last.depth;
    out.nodes = last.nodes;
    out.mate = std::abs(out.score) >= MATE_BOUND;
    return true;
}

bool Stockfish::assess(const Position& before, const Move& played, int depth, int movetimeMs,
                       std::atomic<bool>* cancel, Assessment& out) {
    out = Assessment();
    if (!alive()) return false;

    Position copy = before;
    std::vector<Move> legal = copy.legalMoves();
    if (legal.empty()) return false;
    out.legalCount = (int)legal.size();
    if (legal.size() == 1) return true;  // forced: nothing to judge (tag NONE)

    const int mpv = std::min<int>((int)legal.size(), 500);
    if (!send("setoption name MultiPV value " + std::to_string(mpv))) return false;
    if (!send("position fen " + before.toFen())) return false;
    if (!send("go depth " + std::to_string(depth) + " movetime " + std::to_string(movetimeMs)))
        return false;

    std::vector<RootScore> rootScores;
    const bool done = waitForBestmove(movetimeMs, cancel, nullptr, &rootScores, nullptr);
    send("setoption name MultiPV value 1");  // leave it ready for plain searches
    if (!done) return false;

    // The engine prints every root move at every depth, so the last score seen
    // for a move is the one from its deepest completed search.
    std::vector<std::pair<std::string, int>> scores;
    scores.reserve(rootScores.size());
    for (const RootScore& kv : rootScores) {
        bool seen = false;
        for (auto& s : scores)
            if (s.first == kv.pv0) {
                s.second = kv.score;
                seen = true;
                break;
            }
        if (!seen) scores.emplace_back(kv.pv0, kv.score);
    }
    if (scores.empty()) return false;

    const std::string want = uciMove(played);
    int playedScore = 0;
    bool havePlayed = false;
    for (const auto& s : scores)
        if (s.first == want) {
            playedScore = s.second;
            havePlayed = true;
            break;
        }
    if (!havePlayed) {
        // Rare (MultiPV did not cover it): judge the move on its own instead.
        Position after = before;
        UndoInfo u;
        after.doMove(played, u);
        SearchResult solo;
        if (!search(after, depth, movetimeMs, cancel, solo)) return false;
        playedScore = -solo.score;
    }

    const int LOW = -1000000;
    out.best = LOW;
    out.secondUpper = LOW;
    std::string bestUci;
    for (const auto& s : scores) {
        if (s.second > out.best) {
            out.best = s.second;
            bestUci = s.first;
        }
        if (s.first == want) continue;
        if (s.second > out.secondUpper) out.secondUpper = s.second;
    }
    if (out.secondUpper == LOW) out.secondUpper = out.best;  // nothing else to compare with
    if (bestUci.empty()) bestUci = want;
    for (const Move& m : legal)
        if (uciMove(m) == bestUci) {
            out.bestMove = m;
            break;
        }

    out.played = playedScore;
    out.sacNet = sacrificeNet(before, played);
    out.ok = true;
    return true;
}

bool Stockfish::bestMoves(const Position& pos, int depth, int movetimeMs, int count,
                          std::atomic<bool>* cancel, std::vector<ScoredMove>& out) {
    out.clear();
    if (!alive() || count <= 0) return false;
    Position copy = pos;
    const int legalCount = (int)copy.legalMoves().size();
    if (legalCount == 0) return false;
    const int mpv = std::min(std::min(count, legalCount), 500);

    if (!send("setoption name MultiPV value " + std::to_string(mpv))) return false;
    if (!send("position fen " + pos.toFen())) return false;
    if (!send("go depth " + std::to_string(depth) + " movetime " + std::to_string(movetimeMs)))
        return false;

    std::vector<RootScore> lines;
    const bool finished = waitForBestmove(movetimeMs, cancel, nullptr, &lines, nullptr);
    send("setoption name MultiPV value 1");  // leave it ready for plain searches
    if (!finished && lines.empty()) return false;

    // One line per PV per depth -- but a search stopped by movetime can leave
    // its last depth half printed, which would both shorten the list and mix
    // two depths' scores. Prefer the deepest depth that already has them all.
    std::map<int, std::vector<RootScore>> byDepth;
    for (const RootScore& l : lines) {
        if (l.multipv < 1 || l.multipv > mpv) continue;
        byDepth[l.depth].push_back(l);
    }

    std::vector<ScoredMove> ranked;
    for (auto it = byDepth.rbegin(); it != byDepth.rend(); ++it) {
        std::vector<ScoredMove> cand;
        for (const RootScore& l : it->second) {
            Move m;
            if (!parseUci(pos, l.pv0, m)) continue;
            bool dup = false;
            for (const ScoredMove& r : cand)
                if (r.move == m) dup = true;
            if (dup) continue;
            cand.push_back({m, l.score, it->first});
        }
        if ((int)cand.size() >= count || std::next(it) == byDepth.rend()) {
            std::sort(cand.begin(), cand.end(),
                      [](const ScoredMove& a, const ScoredMove& b) { return a.score > b.score; });
            ranked = std::move(cand);
            break;
        }
    }
    if ((int)ranked.size() > count) ranked.resize(count);

    out = std::move(ranked);
    return !out.empty();
}
