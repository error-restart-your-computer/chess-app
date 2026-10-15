#pragma once
// screen.h -- finding a chess board on the desktop and reading the pieces on it
//
// The overlay watches the screen: capture a frame, find the 8x8 checkerboard,
// read its 64 squares and hand the position to the engine. Everything here
// works on plain RGB frames so the tests can drive it with rendered boards.
#include "common.h"

#include <SDL.h>
#include <string>
#include <vector>

// One captured frame of the desktop, packed RGB24.
struct ScreenImage {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgb;

    void reset(int width, int height) {
        w = width;
        h = height;
        rgb.assign((size_t)w * h * 3, 0);
    }
    bool empty() const { return w <= 0 || h <= 0 || rgb.empty(); }
    const uint8_t* px(int x, int y) const { return rgb.data() + 3 * ((size_t)y * w + x); }
    uint8_t* px(int x, int y) { return rgb.data() + 3 * ((size_t)y * w + x); }
};

// Where the board is, how big its squares are, and the two square colours
// (A = squares where row+col is even, B = the odd ones).
struct BoardRect {
    bool ok = false;
    int x = 0;
    int y = 0;
    int cell = 0;
    int colA[3] = {0, 0, 0};
    int colB[3] = {0, 0, 0};
    float score = 0;    // 1 = a perfect two-colour checkerboard
    int covered = 0;    // squares sitting behind our own window (0 when ok)
    // How the search ended, in one line for the log: how well the best grid
    // scored, how many candidates there were, whether our own window was in
    // the way and whether the marker stripe was even seen. "No chess board
    // found" on its own never says which of those it was.
    std::string why;
};

// The 64 squares read off the screen: a FEN for `side` to move, the raw board
// (square 0 = a1) for en passant inference, and how sure the reader was.
struct ReadResult {
    bool ok = false;
    std::string fen;
    Piece board[64] = {};
    float confidence = 0;
    int kings = 0;
    // One line per rank saying how every square was read -- filled when the
    // read is refused, or when the caller asks for it outright with
    // `wantDetail`: a refusal explains itself, the confident mistake does
    // not. The caller prints them; nothing here writes to the terminal.
    std::vector<std::string> detail;
    // Further lines, same condition: the squares decided by the narrowest
    // margins broken into the six measurements their scores add up to -- up
    // to twelve of them, plus the read nobody scored well. A total says who
    // won; this says why.
    std::vector<std::string> terms;
};

// The arrow the overlay draws: recognition has to know about it, because its
// pixels sit on top of two squares.
struct ArrowGeom {
    bool on = false;
    SDL_Color color = {0, 0, 0, 255};
    SDL_Point shaft[4] = {};
    SDL_Point head[3] = {};
    int bbox[4] = {0, 0, 0, 0};  // x, y, w, h (screen coordinates)
};

// Geometry of the best-move arrow for `from`->`to` on board `b`, in screen
// coordinates. The drawing side and the reader share it so the pixels that are
// drawn are exactly the pixels that get ignored while reading.
ArrowGeom makeArrow(const BoardRect& b, bool blackAtBottom, int from, int to, int thick,
                    SDL_Color color);

// Call once before SDL gets near Xlib: capture and the overlay window run on
// threads of their own and Xlib is only safe when XInitThreads() came first.
void x11InitThreads();

// Which capture path the next captureScreen() takes: "x11", a tool name, or
// "auto" until the first frame has decided.
const char* captureBackend();
// Non-empty when no capture path can work on this machine.
std::string captureProblem();

// Grab the desktop. X11 root window when that works, otherwise a screenshot
// tool (KDE's spectacle, gnome-screenshot, ImageMagick's import).
bool captureScreen(ScreenImage& out, std::string* err);

// The frame the watcher is working from, written out as a BMP: the log says
// where the grid sat and what it read, this says what was actually there.
// The overlay calls it on every capture when OV_DUMP names a path, so the
// file always holds the latest frame and the numbers have a picture behind
// them.
bool saveScreenImage(const ScreenImage& im, const char* path);

// Find an 8x8 two-colour checkerboard. Squares whose centre falls inside one
// of `skip` (our own window) disqualify a candidate: they cannot be read.
bool findBoard(const ScreenImage& img, const std::vector<SDL_Rect>& skip, BoardRect& out);

// The finder's own checkerboard score for a grid at exactly this position
// and square size, so a caller holding two opinions about the same board --
// the lattice's rectangle and the model's -- can score both and take the
// better. Squares our own window covers score zero.
float scoreGridAt(const ScreenImage& img, int x, int y, int cell, const std::vector<SDL_Rect>& skip);

// Locate our own window from the marker stripe drawn while the overlay runs --
// window positions are not available when SDL runs on Wayland.
bool findSelfWindow(const ScreenImage& img, const SDL_Rect& marker, int winW, int winH,
                    SDL_Rect& out);

// Turn the 64 squares into a FEN. `blackAtBottom` says which way the board is
// shown, `prev` (optional) is the board before the last change so a two-square
// pawn push can be given its en passant square, and pixels under `arrow` are
// left out of the read. `wantDetail` (optional) fills `detail` and `terms`
// even when the read is accepted: a wrong answer that believes itself is the
// one worth having numbers for, and refusals are the only ones that explain
// themselves without being asked.
ReadResult readBoard(const ScreenImage& img, const BoardRect& b, bool blackAtBottom, Color side,
                     const ArrowGeom& arrow, const Piece* prev, bool wantDetail = false);
