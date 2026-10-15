// aiwatch.cpp -- one frame at a time across a pipe to tools/piece_ai.py
//
// Protocol (see the script for the other end):
//   -> "FRAME <w> <h> <blackAtBottom> <nskip> [<x y w h> ...]\n" then w*h*3
//      bytes of packed RGB24
//   <- "READY\n" once after the model loads, then per frame either
//      "B <x> <y> <w> <h> <conf> <fen-placement>\n" or "N <why>\n"
//   -> "QUIT\n" at exit.
//
// Deadlines everywhere: the worker thread must not be pinned by a slow
// interpreter, and a child that stops answering is killed and -- twice at
// most -- replaced, after which the template reader answers alone.
#include "aiwatch.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

namespace {

// The first answer pays for imports and the model load; later ones are just a
// transfer and an inference.
constexpr int READY_MS = 20000;
constexpr int FRAME_MS = 6000;
constexpr int MAX_FAILS = 2;  // launches that die before answering, then give up

struct Side {
    pid_t pid = -1;
    int to = -1;    // frames go out here
    int from = -1;  // answers come back here
    int fails = 0;
    bool off = false;
    std::string offWhy;  // said once by the caller, which deduplicates
};

Side& side() {
    static Side s;
    return s;
}

// poll() for one event on one fd, EINTR-safe. false = deadline or hangup.
bool ioWait(int fd, short ev, int timeoutMs) {
    pollfd p{fd, ev, 0};
    int r;
    do {
        r = ::poll(&p, 1, timeoutMs);
    } while (r < 0 && errno == EINTR);
    return r > 0 && (p.revents & ev) != 0;
}

// Exactly n bytes in, or the deadline; the fd is non-blocking, so a child that
// stops draining the pipe shows up here rather than in a hung write().
bool writeAll(int fd, const void* data, size_t n, int timeoutMs) {
    const char* p = static_cast<const char*>(data);
    size_t put = 0;
    while (put < n) {
        ssize_t r = ::write(fd, p + put, n - put);
        if (r > 0) {
            put += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR) && ioWait(fd, POLLOUT, timeoutMs))
            continue;
        return false;  // EPIPE, EBADF, ...
    }
    return true;
}

// One line without its newline. false = deadline, hangup, or EOF.
bool readLine(int fd, std::string& line, int timeoutMs) {
    line.clear();
    for (;;) {
        if (!ioWait(fd, POLLIN, timeoutMs)) return false;
        char c;
        ssize_t r = ::read(fd, &c, 1);
        if (r == 1) {
            if (c == '\n') return true;
            if (line.size() < 8192) line.push_back(c);
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        return false;
    }
}

// Which interpreters to try, and which script: both answer to the environment,
// and the defaults follow the project's habit of running from its own
// directory (bin/stockfish is found the same way).
bool resolve(std::vector<std::string>& pys, std::string& script, std::string& why) {
    if (const char* e = std::getenv("OV_AI_PY"); e && *e) {
        pys.assign(1, e);  // asked for by name: it alone is tried
    } else {
        const char* home = std::getenv("HOME");
        const std::string h = home && *home ? std::string(home) : std::string();
        pys = {"venv/bin/python", "aivenv/bin/python"};
        if (!h.empty()) {
            pys.push_back(h + "/aivenv/bin/python");
            pys.push_back(h + "/.venv/bin/python");
        }
        pys.push_back("python3");
    }
    if (const char* e = std::getenv("OV_AI"); e && *e) {
        script = e;
    } else {
        script = "tools/piece_ai.py";
    }
    if (::access(script.c_str(), R_OK) != 0) {
        why = "piece finder: no " + script + " (start the app from the project directory)";
        return false;
    }
    return true;
}

void reap(Side& s) {
    if (s.to >= 0) {
        ::close(s.to);
        s.to = -1;
    }
    if (s.from >= 0) {
        ::close(s.from);
        s.from = -1;
    }
    if (s.pid > 0) {
        ::kill(s.pid, SIGKILL);
        ::waitpid(s.pid, nullptr, 0);
        s.pid = -1;
    }
}

// One interpreter's attempt: fork, exec, wait for READY. false leaves no
// child behind; `why` says what went wrong.
bool tryStart(const std::string& py, const std::string& script, std::string& why) {
    int inP[2], outP[2];
    if (::pipe(inP) != 0) {
        why = "piece finder: pipe() failed";
        return false;
    }
    if (::pipe(outP) != 0) {
        ::close(inP[0]);
        ::close(inP[1]);
        why = "piece finder: pipe() failed";
        return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(inP[0]);
        ::close(inP[1]);
        ::close(outP[0]);
        ::close(outP[1]);
        why = "piece finder: fork() failed";
        return false;
    }
    if (pid == 0) {
        ::dup2(inP[0], 0);
        ::dup2(outP[1], 1);
        ::close(inP[0]);
        ::close(inP[1]);
        ::close(outP[0]);
        ::close(outP[1]);
        ::execlp(py.c_str(), py.c_str(), script.c_str(), static_cast<char*>(nullptr));
        std::fprintf(stderr, "chess: piece finder: cannot start %s\n", py.c_str());
        ::_exit(127);
    }
    ::close(inP[0]);
    ::close(outP[1]);
    Side& s = side();
    s.pid = pid;
    s.to = inP[1];
    s.from = outP[0];
    ::fcntl(s.to, F_SETFL, ::fcntl(s.to, F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(s.from, F_SETFL, ::fcntl(s.from, F_GETFL, 0) | O_NONBLOCK);
    std::string line;
    if (!readLine(s.from, line, READY_MS) || line != "READY") {
        // The child's own words went to the terminal's stderr; this is the
        // card's version: what a caller can still act on.
        why = line.empty() ? "piece finder: no READY from " + py + " (onnxruntime missing?)"
                           : "piece finder: " + line;
        reap(s);
        return false;
    }
    return true;
}

bool launch(std::string& why) {
    std::vector<std::string> pys;
    std::string script;
    if (!resolve(pys, script, why)) return false;
    // A reader on a dead child must fail, not take the app down with it --
    // the engine installs the same silence for its own pipe (uci.cpp).
    std::signal(SIGPIPE, SIG_IGN);
    bool tried = false;
    for (const std::string& py : pys) {
        if (::access(py.c_str(), X_OK) != 0) continue;  // a candidate that isn't there says nothing
        tried = true;
        if (tryStart(py, script, why)) return true;
    }
    if (!tried) why = "piece finder: no python interpreter (set OV_AI_PY)";
    return false;  // `why` holds the last attempt's complaint
}

// The child is gone or silent: close it down, count it, and decide whether
// this was the failure that turns the finder off for the session.
void failFrame(Side& s, const char* what, std::string* why) {
    reap(s);
    if (++s.fails >= MAX_FAILS) {
        s.off = true;
        s.offWhy = what;
    }
    if (why) *why = what;
}

}  // namespace

bool aiAskBoard(const ScreenImage& img, const std::vector<SDL_Rect>& skip, bool blackAtBottom,
                BoardRect& br, std::string& field0, float& conf, std::string* why) {
    if (why) why->clear();
    if (const char* e = std::getenv("OV_AI"); e && *e && std::strcmp(e, "0") == 0) return false;

    Side& s = side();
    if (s.off) {
        if (why) *why = s.offWhy;
        return false;
    }
    if (s.pid < 0) {
        std::string w;
        if (!launch(w)) {
            if (++s.fails >= MAX_FAILS) {
                s.off = true;
                s.offWhy = w;
            }
            if (why) *why = w;
            return false;
        }
    }

    std::string head = "FRAME " + std::to_string(img.w) + " " + std::to_string(img.h) + " " +
                       (blackAtBottom ? "1" : "0") + " " + std::to_string(skip.size());
    for (const SDL_Rect& r : skip)
        head += " " + std::to_string(r.x) + " " + std::to_string(r.y) + " " +
                std::to_string(r.w) + " " + std::to_string(r.h);
    head += '\n';
    if (!writeAll(s.to, head.data(), head.size(), FRAME_MS) ||
        !writeAll(s.to, img.rgb.data(), img.rgb.size(), FRAME_MS)) {
        failFrame(s, "piece finder: the frame did not go out", why);
        return false;
    }
    std::string line;
    if (!readLine(s.from, line, FRAME_MS)) {
        failFrame(s, "piece finder: no answer for the frame", why);
        return false;
    }
    s.fails = 0;  // it is answering; only silence counts against it
    if (line.size() > 2 && line[0] == 'N') {
        if (why) *why = line.substr(2);
        return false;
    }
    int x = 0, y = 0, w = 0, h = 0;
    char tag = 0;
    char field[1024] = {0};
    if (line[0] != 'B' ||
        std::sscanf(line.c_str(), " %c %d %d %d %d %f %1023[^\n]", &tag, &x, &y, &w, &h, &conf,
                    field) != 7 ||
        tag != 'B' || w <= 0 || h <= 0 || !field[0]) {
        if (why) *why = "piece finder: unreadable answer";
        return false;
    }
    br = BoardRect();
    br.ok = true;
    br.x = x;
    br.y = y;
    br.cell = (w + h) / 16;  // the box is never perfectly square: count both ways
    br.score = conf;
    br.why = "the model's chessboard box";
    field0 = field;
    return true;
}

void aiToReadResult(const std::string& field0, float conf, Color side, ReadResult& rd) {
    rd = ReadResult();
    rd.confidence = conf;
    bool parsed = true;
    int sq = 56;  // a FEN's first field is rank 8, from a8
    for (char c : field0) {
        if (c == '/') {
            // The rank just seen is over, and its count ran one file past
            // its last square (the empty-square digits count too), so the
            // next rank starts sixteen back -- h8 ends at 64, a7 is 48.
            // Landing exactly on a rank boundary is also the check that the
            // rank was eight files wide; anything else has shifted every
            // square after it and cannot be trusted.
            if (sq < 16 || sq > 64 || (sq % 8) != 0) {
                parsed = false;
                break;
            }
            sq -= 16;
            continue;
        }
        if (c >= '1' && c <= '8') {
            sq += c - '0';
            continue;
        }
        PieceType t = NO_PIECE;
        switch (c) {
            case 'p': t = PAWN; break;
            case 'n': t = KNIGHT; break;
            case 'b': t = BISHOP; break;
            case 'r': t = ROOK; break;
            case 'q': t = QUEEN; break;
            case 'k': t = KING; break;
            case 'P': t = PAWN; break;
            case 'N': t = KNIGHT; break;
            case 'B': t = BISHOP; break;
            case 'R': t = ROOK; break;
            case 'Q': t = QUEEN; break;
            case 'K': t = KING; break;
            default: break;
        }
        if (t == NO_PIECE || sq > 63 || sq < 0) {
            parsed = false;
            break;
        }
        const bool white = c >= 'A' && c <= 'Z';
        rd.board[sq] = makePiece(t, white ? WHITE : BLACK);
        if (t == KING) ++rd.kings;
        ++sq;
    }
    // Eight ranks of eight, sixteen apart on the way down: the count stops
    // one past h1. A placement that is short or long anywhere lands off that
    // mark even when it still happens to hold two kings.
    if (parsed && sq != 8) parsed = false;

    // Castling rights from where the back-rank pieces are: a screenshot cannot
    // tell "never moved" from "moved and came back" -- the same limit the
    // template reader works under (screen.cpp).
    std::string castle;
    if (rd.board[4] == makePiece(KING, WHITE)) {
        if (rd.board[7] == makePiece(ROOK, WHITE)) castle += 'K';
        if (rd.board[0] == makePiece(ROOK, WHITE)) castle += 'Q';
    }
    if (rd.board[60] == makePiece(KING, BLACK)) {
        if (rd.board[63] == makePiece(ROOK, BLACK)) castle += 'k';
        if (rd.board[56] == makePiece(ROOK, BLACK)) castle += 'q';
    }
    rd.fen = field0;
    rd.fen += side == WHITE ? " w " : " b ";
    rd.fen += castle.empty() ? "-" : castle;
    rd.fen += " - 0 1";
    rd.ok = parsed && rd.kings == 2;
}

void aiShutdown() {
    Side& s = side();
    if (s.pid < 0 && s.to < 0 && s.from < 0) return;
    if (s.to >= 0) {  // the polite exit first: the child drains stdin and stops
        writeAll(s.to, "QUIT\n", 5, 300);
        ::close(s.to);
        s.to = -1;
    }
    if (s.from >= 0) {
        ::close(s.from);
        s.from = -1;
    }
    if (s.pid > 0) {
        for (int i = 0; i < 50 && ::waitpid(s.pid, nullptr, WNOHANG) == 0; ++i) ::usleep(10000);
        if (::waitpid(s.pid, nullptr, WNOHANG) == 0) {
            ::kill(s.pid, SIGKILL);
            ::waitpid(s.pid, nullptr, 0);
        }
        s.pid = -1;
    }
}
