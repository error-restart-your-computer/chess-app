// tests/screen_test.cpp -- the overlay's eye: finding a chess board on a
// screen and reading its 64 squares back off it.
//
// Nothing here photographs a real desktop. A synthetic one is rendered instead
// -- gradient background, flat "windows", a board drawn where a website would
// put it -- and then the very code the overlay runs is pointed at it: the
// finder, the reader, the marker that tells our own window apart, and the arrow
// that has to be ignored while reading because it lies over two squares.
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/aiwatch.h"
#include "../src/pieces.h"
#include "../src/position.h"
#include "../src/screen.h"

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

#define CHECKF(cond, what, detail)                                       \
    do {                                                                 \
        ++checks;                                                        \
        if (!(cond)) {                                                   \
            std::printf("FAIL: %s -- %s (line %d)\n", what, detail, __LINE__); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

// ------------------------------------------------------------- rendering ----

static const int SCR_W = 800;
static const int SCR_H = 600;
// The palette of some other chess site -- deliberately NOT the app's own
// (238,230,206 / 118,150,98). findBoard refuses a board in our own colours
// while it has no idea where our own window is, so a board drawn here in those
// colours is invisible by default. Check 4 below uses them for exactly that
// reason -- and then says what changes once the window is known.
static const SDL_Color LIGHT{240, 217, 181, 255};
static const SDL_Color DARK{181, 136, 99, 255};

// A plain desktop: a vertical gradient (deliberately not a checkerboard) with
// one flat "window" on it.
static void paintDesktop(SDL_Renderer* r, int w, int h) {
    for (int y = 0; y < h; y += 8) {
        const int v = 34 + (y * 46) / h;
        SDL_SetRenderDrawColor(r, (Uint8)(v / 3), (Uint8)(v + 16), (Uint8)(v + 52), 255);
        SDL_Rect band{0, y, w, 8};
        SDL_RenderFillRect(r, &band);
    }
    SDL_SetRenderDrawColor(r, 26, 28, 34, 255);
    SDL_Rect win{676, 30, 110, 90};
    SDL_RenderFillRect(r, &win);
}

// Draws `pos` the way a chess website would: squares, pieces, no decoration.
static void renderPosition(SDL_Surface* s, const Position& pos, int ox, int oy, int cell,
                           bool blackAtBottom, SDL_Color light, SDL_Color dark) {
    SDL_Renderer* r = SDL_CreateSoftwareRenderer(s);
    if (!r) return;
    paintDesktop(r, s->w, s->h);
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            const bool lightSq = ((row + col) & 1) == 0;
            const SDL_Color c = lightSq ? light : dark;
            SDL_Rect q{ox + col * cell, oy + row * cell, cell, cell};
            SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
            SDL_RenderFillRect(r, &q);
        }
    }
    for (int sq = 0; sq < 64; ++sq) {
        const Piece p = pos.at(sq);
        if (isEmpty(p)) continue;
        const int f = fileOf(sq), rk = rankOf(sq);
        const int col = blackAtBottom ? 7 - f : f;
        const int row = blackAtBottom ? rk : 7 - rk;
        SDL_Rect box{ox + col * cell, oy + row * cell, cell, cell};
        drawPiece(r, p, box);
    }
    SDL_RenderPresent(r);
    SDL_DestroyRenderer(r);  // the surface it was given stays ours
}

static ScreenImage toImage(SDL_Surface* s) {
    ScreenImage im;
    im.reset(s->w, s->h);
    const Uint32* px = (const Uint32*)s->pixels;
    const int pitch = s->pitch / 4;
    for (int y = 0; y < s->h; ++y)
        for (int x = 0; x < s->w; ++x) {
            const Uint32 v = px[(size_t)y * pitch + x];
            uint8_t* d = im.px(x, y);
            d[0] = (uint8_t)(v >> 16);
            d[1] = (uint8_t)(v >> 8);
            d[2] = (uint8_t)v;
        }
    return im;
}

// Render a desktop with `fen` on it at (ox, oy), then look for the board.
static void shoot(const std::string& fen, int ox, int oy, int cell, bool blackAtBottom,
                  SDL_Color light, SDL_Color dark, ScreenImage& im, BoardRect& b,
                  const std::vector<SDL_Rect>& skip = std::vector<SDL_Rect>()) {
    SDL_Surface* s =
        SDL_CreateRGBSurfaceWithFormat(0, SCR_W, SCR_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    Position pos;
    pos.setFen(fen);
    renderPosition(s, pos, ox, oy, cell, blackAtBottom, light, dark);
    im = toImage(s);
    SDL_FreeSurface(s);
    findBoard(im, skip, b);
}

// A board photographed off a real desktop while the overlay watched, kept as
// BMP (SDL loads that without help). A rendered board only ever wears one
// site's art and one site's decorations; the questions that exist nowhere
// else -- does a foreign piece set read back right, what do a site's own move
// arrows do to an empty square -- need the real thing.
static bool loadFixture(const char* name, ScreenImage& im) {
    SDL_Surface* s = SDL_LoadBMP((std::string("tests/data/") + name).c_str());
    if (!s) return false;
    SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    if (!c) return false;
    im = toImage(c);
    SDL_FreeSurface(c);
    return true;
}

// ------------------------------------------------------------- the checks ----

static std::string field(const std::string& fen, int i) {
    size_t start = 0;
    for (int f = 0; f < i; ++f) {
        start = fen.find(' ', start);
        if (start == std::string::npos) return std::string();
        ++start;
    }
    const size_t end = fen.find(' ', start);
    return fen.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The r-th rank of FEN field 0 (r = 0 is rank 8), for judging a board rank
// by rank: a capture arrow lies over the middle of the board, so what the
// ranks with the setup in them say is the whole question there.
static std::string fenRank(const std::string& fen, int r) {
    size_t start = 0;
    for (int i = 0; i < r; ++i) {
        start = fen.find('/', start);
        if (start == std::string::npos) return std::string();
        ++start;
    }
    // The last rank has no slash after it and runs straight into the
    // side-to-move and the rest of the record; only the piece letters are
    // what this judges.
    const size_t end = std::min(fen.find('/', start), fen.find(' ', start));
    return fen.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// On failure -- or always, under OV_FIXTURE -- a refused or suspicious read
// prints what it saw per rank and how each square scored.
static void showRead(const char* what, const ReadResult& rd) {
    std::printf("--- %s: %s (%d kings, %.2f sure)\n", what, rd.fen.c_str(), rd.kings,
                rd.confidence);
    for (size_t i = 0; i < rd.detail.size(); ++i) std::printf("    %s\n", rd.detail[i].c_str());
    for (size_t i = 0; i < rd.terms.size(); ++i) std::printf("    %s\n", rd.terms[i].c_str());
}

// Find the board, read it back, and compare the first four FEN fields (the
// move counters are always written as "0 1" by the reader).
static void expectPosition(const char* what, const std::string& fen, int ox, int oy, int cell,
                           bool blackAtBottom, Color side) {
    ScreenImage im;
    BoardRect b;
    shoot(fen, ox, oy, cell, blackAtBottom, LIGHT, DARK, im, b);

    CHECKF(b.ok, what, "the board was not found");
    if (!b.ok) return;
    CHECKF(std::abs(b.x - ox) <= 3, what, "board x is off by more than 3 pixels");
    CHECKF(std::abs(b.y - oy) <= 3, what, "board y is off by more than 3 pixels");
    CHECKF(std::abs(b.cell - cell) <= 3, what, "square size is off by more than 3 pixels");

    const ReadResult rd = readBoard(im, b, blackAtBottom, side, ArrowGeom(), nullptr, true);
    CHECKF(rd.ok, what,
           (std::string("the position could not be read -- kings ") + std::to_string(rd.kings) +
            ", read " + rd.fen + ", " + std::to_string(rd.confidence) + " sure").c_str());
    if (!rd.ok) {
        for (size_t i = 0; i < rd.terms.size(); ++i) std::printf("    %s\n", rd.terms[i].c_str());
        return;
    }
    CHECKF(rd.kings == 2, what, "the two kings were not both seen");
    {
        const std::string d = "read " + rd.fen;
        const std::string got = field(rd.fen, 0), want = field(fen, 0);
        CHECKF(got == want, what, (std::string("pieces: got [") + got + "] want [" + want +
                                   "] -- " + d).c_str());
    }
    CHECKF(field(rd.fen, 1) == field(fen, 1), what, "the side to move came back wrong");
    {
        const std::string got = field(rd.fen, 2), want = field(fen, 2);
        CHECKF(got == want, what, (std::string("castling: got [") + got + "] want [" + want +
                                   "] -- read " + rd.fen).c_str());
    }
    {
        const std::string got = field(rd.fen, 3), want = field(fen, 3);
        CHECKF(got == want, what, (std::string("en passant: got [") + got + "] want [" + want +
                                   "] -- read " + rd.fen).c_str());
    }
    CHECKF(rd.confidence >= 0.55f, what, "the reader was not sure of itself");
}

// A convex polygon, the rule the reader excludes arrow pixels by.
static bool inPoly(const SDL_Point* p, int n, int x, int y) {
    int sign = 0;
    for (int i = 0; i < n; ++i) {
        const SDL_Point& a = p[i];
        const SDL_Point& b = p[(i + 1) % n];
        const int cr = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        if (cr == 0) continue;
        const int s = cr > 0 ? 1 : -1;
        if (sign == 0)
            sign = s;
        else if (s != sign)
            return false;
    }
    return true;
}

// Paint the arrow onto the frame in one colour, returning how many pixels of
// the square at `countSq` it covered.
static int paintArrow(ScreenImage& im, const ArrowGeom& ag, int countSq, int ox, int oy,
                      int cell) {
    int painted = 0, inCounted = 0;
    const int x1 = ag.bbox[0] + ag.bbox[2];
    const int y1 = ag.bbox[1] + ag.bbox[3];
    for (int y = ag.bbox[1]; y < y1; ++y)
        for (int x = ag.bbox[0]; x < x1; ++x) {
            if (x < 0 || y < 0 || x >= im.w || y >= im.h) continue;
            if (!inPoly(ag.shaft, 4, x, y) && !inPoly(ag.head, 3, x, y)) continue;
            uint8_t* p = im.px(x, y);
            p[0] = ag.color.r;
            p[1] = ag.color.g;
            p[2] = ag.color.b;
            ++painted;
            const int col = (x - ox) / cell;
            const int row = (y - oy) / cell;
            if (col >= 0 && col < 8 && row >= 0 && row < 8 && makeSquare(col, 7 - row) == countSq)
                ++inCounted;
        }
    return inCounted > 0 ? painted : 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // No display is needed for any of this: the boards are rendered in memory.
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    x11InitThreads();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // OV_TRY=<file.bmp> runs one saved image (an OV_DUMP from the live
    // overlay) through the whole grid-only pipeline -- findBoard, then
    // readBoard -- and reports where the board was, how its squares came
    // out, and the position that came back. No tests, no check count: this
    // is the offline half of a diagnosis, for when the screen itself is
    // unreachable.
    if (const char* tryFile = std::getenv("OV_TRY")) {
        SDL_Surface* s = SDL_LoadBMP(tryFile);
        ScreenImage im;
        bool have = false;
        if (s) {
            SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
            SDL_FreeSurface(s);
            if (c) {
                im = toImage(c);
                SDL_FreeSurface(c);
                have = true;
            }
        }
        if (!have) {
            std::printf("OV_TRY: cannot load %s\n", tryFile);
            return 1;
        }
        const uint8_t* p1 = im.px(std::min(100, im.w - 1), std::min(100, im.h - 1));
        const uint8_t* p2 = im.px(im.w / 2, im.h / 2);
        std::printf("OV_TRY: loaded %dx%d px(100,100)=%d,%d,%d px(mid)=%d,%d,%d\n", im.w, im.h,
                    p1[0], p1[1], p1[2], p2[0], p2[1], p2[2]);
        BoardRect b;
        if (!findBoard(im, std::vector<SDL_Rect>(), b)) {
            std::printf("OV_TRY: no board: %s\n", b.why.c_str());
            return 2;
        }
        std::printf("OV_TRY: board %d,%d cell %d grid %.2f\n", b.x, b.y, b.cell, b.score);
        const ReadResult rd = readBoard(im, b, false, WHITE, ArrowGeom(), nullptr, true);
        showRead("OV_TRY", rd);
        std::printf("OV_TRY: %s kings %d conf %.2f\n", rd.ok ? "accepted" : "REFUSED", rd.kings,
                    rd.confidence);
        return rd.ok ? 0 : 3;
    }

    const std::string start = START_FEN;
    const std::string midgame = "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 4 4";

    // 1. the starting position, both ways round
    expectPosition("start, White's view", start, 100, 80, 56, false, WHITE);
    expectPosition("start, Black's view", start, 100, 80, 56, true, WHITE);

    // 2. a position from the middle of a game, with bigger squares
    expectPosition("middle of a game", midgame, 150, 60, 64, false, WHITE);
    expectPosition("middle of a game, Black's view", midgame, 150, 60, 64, true, WHITE);

    // 3. the view matters: the same picture read the wrong way round is a
    //    different position, not this one
    {
        ScreenImage im;
        BoardRect b;
        shoot(start, 100, 80, 56, false, LIGHT, DARK, im, b);
        CHECK(b.ok, "board found");
        const ReadResult rd = readBoard(im, b, true, WHITE, ArrowGeom(), nullptr);
        CHECKF(rd.ok, "wrong view", "still readable");
        CHECKF(field(rd.fen, 0) != field(start, 0), "wrong view",
               "and it is not the position we drew");
    }

    // 4. our own board is never mistaken for the one being watched: the same
    //    position in our own palette has to be refused
    {
        ScreenImage im;
        BoardRect b;
        shoot(start, 100, 80, 56, false, LIGHT, DARK, im, b);
        CHECKF(b.ok, "our colours", "a normal board still shows up");

        SDL_Surface* s =
            SDL_CreateRGBSurfaceWithFormat(0, SCR_W, SCR_H, 32, SDL_PIXELFORMAT_ARGB8888);
        if (s) {
            Position pos;
            pos.setFen(start);
            renderPosition(s, pos, 100, 80, 56, false, SDL_Color{238, 230, 206, 255},
                           SDL_Color{118, 150, 98, 255});
            ScreenImage mine = toImage(s);
            SDL_FreeSurface(s);
            BoardRect r;
            const bool found = findBoard(mine, std::vector<SDL_Rect>(), r);
            CHECKF(!found, "our own board",
                   "the app's board must not be picked as the one to advise on");

            // Chess.com's default green is this same palette. Once the marker
            // stripe has said where our own window is, a board wearing those
            // colours anywhere else is somebody else's and has to be found.
            std::vector<SDL_Rect> elsewhere;
            elsewhere.push_back(SDL_Rect{0, 560, 400, 40});  // our window, over there
            BoardRect away;
            CHECKF(findBoard(mine, elsewhere, away), "green board outside our window",
                   "a board in our own colours but away from our window must be found");

            // Inside that window it is our own board again, marker or no marker.
            std::vector<SDL_Rect> ourWin;
            ourWin.push_back(SDL_Rect{90, 70, 470, 470});  // the board sits inside it
            BoardRect behind;
            CHECKF(!findBoard(mine, ourWin, behind), "our own board behind our window",
                   "the board inside our own window is never the one to advise on");
        }
    }

    // 5. a board behind our own window: not readable, but it says why
    {
        ScreenImage im;
        BoardRect b;
        std::vector<SDL_Rect> skip;
        skip.push_back(SDL_Rect{400, 60, 200, 500});
        shoot(start, 100, 80, 56, false, LIGHT, DARK, im, b, skip);
        CHECKF(!b.ok, "board behind a window", "it cannot be read from there");
        CHECKF(b.covered > 0, "board behind a window", "and it says so");
    }

    // 6. the marker stripe that finds our own window
    {
        ScreenImage im;
        im.reset(SCR_W, SCR_H);
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x) {
                uint8_t* p = im.px(x, y);
                p[0] = 20;
                p[1] = 22;
                p[2] = 26;
            }
        const SDL_Rect marker{1012, 722, 24, 6};  // the app's own, in window pixels
        const int mx = 300, my = 400;
        static const uint8_t cols[4][3] = {
            {255, 0, 128}, {0, 255, 64}, {64, 128, 255}, {255, 255, 0}};
        for (int i = 0; i < 4; ++i)
            for (int y = 0; y < 6; ++y)
                for (int x = 0; x < 6; ++x) {
                    uint8_t* p = im.px(mx + i * 6 + x, my + y);
                    p[0] = cols[i][0];
                    p[1] = cols[i][1];
                    p[2] = cols[i][2];
                }
        SDL_Rect win{0, 0, 0, 0};
        CHECKF(findSelfWindow(im, marker, 1080, 760, win), "marker", "the stripe was not found");
        CHECKF(win.x == mx - marker.x && win.y == my - marker.y, "marker",
               "the window origin came out wrong");
        CHECKF(win.w == 1080 && win.h == 760, "marker", "the window size came out wrong");

        // a stripe of some other colour is not ours
        for (int i = 0; i < 4; ++i)
            for (int y = 0; y < 6; ++y)
                for (int x = 0; x < 6; ++x) {
                    uint8_t* p = im.px(mx + i * 6 + x, my + y);
                    p[0] = 10;
                    p[1] = 10;
                    p[2] = 10;
                }
        SDL_Rect none{0, 0, 0, 0};
        CHECKF(!findSelfWindow(im, marker, 1080, 760, none), "marker",
               "a random stripe is not our window");
    }

    // 7. the arrow: it lies over two squares, and the reader must skip the
    //    pixels it covers while still reading the pieces around them
    {
        ScreenImage im;
        BoardRect b;
        shoot(start, 100, 80, 56, false, LIGHT, DARK, im, b);
        CHECK(b.ok, "board found for the arrow test");
        const ArrowGeom ag = makeArrow(b, false, 16, 32, 14, SDL_Color{0, 160, 255, 255});
        CHECK(ag.on, "arrow geometry");
        CHECK(ag.bbox[2] > 0 && ag.bbox[3] > 0, "arrow covers something");

        // the shaft starts inside the square it comes from (a3)
        const SDL_Rect a3{100 + 0 * 56, 80 + 5 * 56, 56, 56};
        bool startsInside = false;
        for (int i = 0; i < 4; ++i)
            if (ag.shaft[i].x >= a3.x && ag.shaft[i].x < a3.x + a3.w &&
                ag.shaft[i].y >= a3.y && ag.shaft[i].y < a3.y + a3.h)
                startsInside = true;
        CHECK(startsInside, "the shaft starts in the square it comes from");

        // a3 = square 16, a4 = 24, a5 = 32
        const int overA4 = paintArrow(im, ag, 24, 100, 80, 56);
        CHECKF(overA4 > 60, "arrow on the board", "the arrow misses a3/a4/a5");

        const ReadResult told = readBoard(im, b, false, WHITE, ag, nullptr);
        CHECKF(told.ok, "arrow told about", "the position is still readable");
        if (told.ok) {
            const std::string got = field(told.fen, 0), want = field(start, 0);
            CHECKF(got == want, "arrow told about",
                   (std::string("read ") + told.fen + " want " + start).c_str());
        }

        const ReadResult blind = readBoard(im, b, false, WHITE, ArrowGeom(), nullptr);
        CHECKF(blind.ok, "arrow hidden", "still readable");
        // Line 418 already counted the arrow's ink into a4. What matters
        // here is that ink is never art whoever is told about it: the colour
        // test (tooSaturated) stops it before a shape is measured, and the
        // disclosure only adds the exact geometry on top. So a reader left in
        // the dark still comes back with the start position.
        if (blind.ok)
            CHECKF(field(blind.fen, 0) == field(start, 0), "arrow hidden",
                   "arrow ink is never read as a piece");
    }

    // 8. en passant: nowhere on the board itself, so it can only be worked out
    //    from the position before it
    {
        const std::string before = "rnbqkbnr/ppp1pppp/8/8/3p4/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        const std::string after = "rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1";
        ScreenImage im;
        BoardRect b;
        shoot(after, 100, 80, 56, false, LIGHT, DARK, im, b);
        CHECK(b.ok, "board found for the en passant test");
        Position prev;
        prev.setFen(before);

        const ReadResult withPrev = readBoard(im, b, false, BLACK, ArrowGeom(), prev.board);
        CHECKF(withPrev.ok, "en passant", "the position is readable");
        if (withPrev.ok) {
            const std::string got = field(withPrev.fen, 3);
            CHECKF(got == "e3", "en passant",
                   (std::string("got [") + got + "] want [e3] -- read " + withPrev.fen).c_str());
        }

        const ReadResult noPrev = readBoard(im, b, false, BLACK, ArrowGeom(), nullptr);
        CHECKF(noPrev.ok, "en passant", "readable without the history too");
        if (noPrev.ok)
            CHECKF(field(noPrev.fen, 3) == "-", "en passant",
                   "without the board before it there is nothing to infer");
    }

    // 9. an empty screen is not a chess position
    {
        ScreenImage im;
        im.reset(SCR_W, SCR_H);
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x) {
                uint8_t* p = im.px(x, y);
                p[0] = (uint8_t)(30 + x / 8);
                p[1] = (uint8_t)(40 + y / 8);
                p[2] = 60;
            }
        BoardRect b;
        CHECKF(!findBoard(im, std::vector<SDL_Rect>(), b), "empty desktop",
               "a gradient is not a chess board");
    }

    // 10. Two boards photographed off a live desktop. The green one is
    //     chess.com's -- which is also the app's own palette, so findBoard
    //     may only call it somebody else's once it knows where our window
    //     is, exactly as the marker stripe tells it at run time.
    {
        ScreenImage im;
        const bool have = loadFixture("board104.bmp", im);
        CHECKF(have, "green fixture", "tests/data/board104.bmp could not be loaded");
        if (have) {
            std::vector<SDL_Rect> skip;
            skip.push_back(SDL_Rect{0, 0, 8, 8});  // our window is over there
            BoardRect b;
            CHECKF(findBoard(im, skip, b), "green fixture",
                   b.ok ? "the board was not found" : b.why.c_str());
            if (b.ok) {
                CHECKF(std::abs(b.cell - 104) <= 3, "green fixture",
                       (std::string("square size is ") + std::to_string(b.cell) + " not 104")
                           .c_str());
                const ReadResult rd = readBoard(im, b, false, WHITE, ArrowGeom(), nullptr, true);
                if (!rd.ok || std::getenv("OV_FIXTURE")) showRead("green fixture", rd);
                CHECKF(rd.ok, "green fixture",
                       (std::string("the position could not be read -- kings ") +
                        std::to_string(rd.kings) + ", read " + rd.fen)
                           .c_str());
                if (rd.ok) {
                    // The picture is the start of a game, with the site's own
                    // move arrows lying over the empty middle of the board.
                    // Those arrows are not pieces; the ranks holding the
                    // setup are what has to come back exact.
                    const char* want[4] = {"rnbqkbnr", "pppppppp", "PPPPPPPP", "RNBQKBNR"};
                    const int rank[4] = {0, 1, 6, 7};
                    for (int k = 0; k < 4; ++k) {
                        const std::string got = fenRank(rd.fen, rank[k]);
                        CHECKF(got == want[k], "green fixture",
                               (std::string("rank ") + std::to_string(8 - rank[k]) + ": got [" +
                                got + "] want [" + want[k] + "]")
                                   .c_str());
                    }
                    CHECKF(rd.kings == 2, "green fixture", "both kings were seen");
                }
            }
        }
    }

    // 11. The purple board: a foreign piece set again, this time with the
    //     whole position judged -- two moves played, nothing on the board
    //     that is not a piece.
    {
        ScreenImage im;
        const bool have = loadFixture("cap2.bmp", im);
        CHECKF(have, "purple fixture", "tests/data/cap2.bmp could not be loaded");
        if (have) {
            BoardRect b;
            CHECKF(findBoard(im, std::vector<SDL_Rect>(), b), "purple fixture",
                   b.ok ? "the board was not found" : b.why.c_str());
            if (b.ok) {
                CHECKF(std::abs(b.cell - 92) <= 3, "purple fixture",
                       (std::string("square size is ") + std::to_string(b.cell) + " not 92")
                           .c_str());
                const ReadResult rd = readBoard(im, b, false, WHITE, ArrowGeom(), nullptr, true);
                if (!rd.ok || std::getenv("OV_FIXTURE")) showRead("purple fixture", rd);
                CHECKF(rd.ok, "purple fixture",
                       (std::string("the position could not be read -- kings ") +
                        std::to_string(rd.kings) + ", read " + rd.fen)
                           .c_str());
                if (rd.ok) {
                    const std::string got = field(rd.fen, 0);
                    const std::string want = "rnbqkbnr/pppp1ppp/4p3/8/4P3/8/PPPP1PPP/RNBQKBNR";
                    CHECKF(got == want, "purple fixture",
                           (std::string("pieces: got [") + got + "] want [" + want + "]").c_str());
                }
            }
        }
    }

    // 12. The model's placement, parsed the way the overlay uses it. All
    //     eight ranks have to land where they belong -- the live run once
    //     stopped after rank 8 and then reported a start position with
    //     "1 kings" -- and a placement that does not add up must be refused
    //     even when it happens to hold two kings.
    {
        ReadResult rd;
        const std::string start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR";
        aiToReadResult(start, 0.9f, WHITE, rd);
        CHECKF(rd.ok, "ai placement",
               (std::string("the start position was refused -- kings ") +
                std::to_string(rd.kings) + ", fen " + rd.fen)
                   .c_str());
        CHECKF(rd.kings == 2, "ai placement", "both kings were counted");
        CHECKF(field(rd.fen, 0) == start, "ai placement",
               (std::string("placement: [") + field(rd.fen, 0) + "]").c_str());
        CHECKF(field(rd.fen, 1) == "w", "ai placement", "white to move came back wrong");
        CHECKF(field(rd.fen, 2) == "KQkq", "ai placement",
               (std::string("castling: [") + field(rd.fen, 2) + "]").c_str());
        CHECKF(rd.board[0] == makePiece(ROOK, WHITE) &&
                   rd.board[4] == makePiece(KING, WHITE) &&
                   rd.board[60] == makePiece(KING, BLACK) &&
                   rd.board[63] == makePiece(ROOK, BLACK),
               "ai placement", "the back-rank corners did not land where they belong");

        // A game on: white has only the king's rook left to castle with,
        // black both, and it is black to move.
        aiToReadResult("r3k2r/8/8/8/8/8/8/4K2R", 0.8f, BLACK, rd);
        CHECKF(rd.ok, "ai placement", "a two-rook placement was refused");
        CHECKF(field(rd.fen, 1) == "b", "ai placement", "black to move came back wrong");
        CHECKF(field(rd.fen, 2) == "Kkq", "ai placement",
               (std::string("castling: [") + field(rd.fen, 2) +
                "] -- empty a1/a8 corners must not earn a queen's rook")
                   .c_str());
        CHECKF(rd.board[0] == NO_PIECE, "ai placement", "a1 is empty in that placement");

        // Garbage the model could plausibly send: an impossible letter, a
        // rank too wide, a rank too narrow. The last one still holds both
        // kings -- only the square count gives it away.
        aiToReadResult("rnbqkbnX/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR", 0.9f, WHITE, rd);
        CHECKF(!rd.ok, "ai placement", "an unknown letter was let through");
        aiToReadResult("rnbqkbnrr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR", 0.9f, WHITE, rd);
        CHECKF(!rd.ok, "ai placement", "a nine-file rank was let through");
        aiToReadResult("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBN", 0.9f, WHITE, rd);
        CHECKF(rd.kings == 2 && !rd.ok, "ai placement",
               (std::string("a seven-file rank slipped past -- kings ") +
                std::to_string(rd.kings) + ", ok ")
                   .c_str());
    }

    // 13. An odd-sized board. The walk moves position and size one axis at
    //     a time, and a cell-63 board under a cell-64 grid scores better at
    //     neither step alone: (32,32) is the best position *for* 64, and 63
    //     only pays from (35,35), which the too-long grid cannot walk
    //     toward without reaching further past the board's far edge. The
    //     walk used to settle a pixel out that way, the corner samples then
    //     read the dark page past the board's edge, and the a- and
    //     h-files came back as one wrong colour with phantom pieces on
    //     them -- a live board read as seventeen kings. The joint retry of
    //     each neighbouring size at the end of findBoard has to land the
    //     odd size exactly, and the position reads back exact from there.
    {
        ScreenImage im;
        BoardRect b;
        shoot(start, 35, 35, 63, false, LIGHT, DARK, im, b);
        CHECKF(b.ok, "odd square size", b.ok ? "the board was not found" : b.why.c_str());
        if (b.ok) {
            CHECKF(b.cell == 63, "odd square size",
                   (std::string("square size is ") + std::to_string(b.cell) +
                    " at " + std::to_string(b.x) + "," + std::to_string(b.y) + ", wanted 63 at 35,35")
                       .c_str());
            CHECKF(std::abs(b.x - 35) <= 2 && std::abs(b.y - 35) <= 2, "odd square size",
                   (std::string("board at ") + std::to_string(b.x) + "," + std::to_string(b.y) +
                    " is off by more than 2 pixels")
                       .c_str());
            const ReadResult rd = readBoard(im, b, false, WHITE, ArrowGeom(), nullptr, true);
            if (!rd.ok || std::getenv("OV_FIXTURE")) showRead("odd square size", rd);
            CHECKF(rd.ok && rd.kings == 2, "odd square size",
                   (std::string("the position could not be read -- kings ") +
                    std::to_string(rd.kings) + ", read " + rd.fen)
                       .c_str());
            if (rd.ok)
                CHECKF(field(rd.fen, 0) == field(start, 0), "odd square size",
                       (std::string("pieces: got [") + field(rd.fen, 0) + "] want [" +
                        field(start, 0) + "]")
                           .c_str());
        }
    }

    SDL_Quit();
    std::printf("screen_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
