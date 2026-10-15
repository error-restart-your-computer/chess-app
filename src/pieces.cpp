#include "pieces.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;

// Shapes are authored in a 0..100 square (y grows downwards) and scaled to the box.
struct Shape {
    const double (*pts)[2];
    int n;
};

const double BASE[4][2] = {{26, 80}, {74, 80}, {78, 89}, {22, 89}};

const double PAWN_HEAD[1][2] = {{50, 32}};
const double PAWN_BODY[4][2] = {{42, 43}, {58, 43}, {64, 74}, {36, 74}};
const double PAWN_COLLAR[4][2] = {{34, 74}, {66, 74}, {66, 80}, {34, 80}};

const double ROOK_TOP[12][2] = {{28, 20}, {40, 20}, {40, 27}, {46, 27}, {46, 20}, {54, 20},
                                {54, 27}, {60, 27}, {60, 20}, {72, 20}, {72, 33}, {28, 33}};
const double ROOK_NECK[4][2] = {{34, 33}, {66, 33}, {64, 42}, {36, 42}};
const double ROOK_BODY[4][2] = {{36, 42}, {64, 42}, {60, 74}, {40, 74}};
const double ROOK_COLLAR[4][2] = {{32, 74}, {68, 74}, {68, 80}, {32, 80}};

const double KNIGHT_HEAD[15][2] = {{46, 12}, {53, 26}, {60, 13}, {68, 26}, {74, 44}, {84, 70}, {78, 78}, {36, 78},
                                   {30, 62}, {23, 57}, {12, 51}, {9, 44}, {19, 37}, {29, 30}, {38, 20}};
const double KNIGHT_COLLAR[4][2] = {{32, 76}, {80, 76}, {80, 81}, {32, 81}};
const double KNIGHT_MANE[3][2] = {{65, 32}, {71, 48}, {77, 64}};

// Every other set we meet -- chess.com's, the purple board's -- draws this
// piece as a broad teardrop: a small finial on a dome that flares downward
// to nearly the width of its slab, row after row, with no waist at all.
// Ours had a point-topped body whose narrow throat left the middle two
// fifths of the square almost empty (6 cells of 40 where theirs had 27),
// and the reader, comparing silhouettes, gave those squares to whatever
// rival shape filled them -- a queen or a king. The flare below is fitted
// to those boards' row widths; the collar is the waist theirs step their
// slab behind, kept so the base still reads as a separate foot.
const double BISHOP_BODY[12][2] = {{59, 30}, {65, 38}, {70, 46}, {73, 54}, {71, 62}, {68, 71},
                                    {32, 71}, {29, 62}, {27, 54}, {30, 46}, {35, 38}, {41, 30}};
const double BISHOP_COLLAR[4][2] = {{33, 71}, {67, 71}, {67, 74}, {33, 74}};
const double BISHOP_BASE[4][2] = {{24, 74}, {76, 74}, {78, 89}, {22, 89}};

// A Staunton set draws its king and queen the widest pieces on the board --
// near three quarters of the square -- while ours drew them barely half, and
// the reader stretches whatever it finds on the screen onto the box each
// template occupies. A piece that needs more stretching than its rivals has
// its finer details smeared before the comparison even starts and loses
// squares its shape should win. These two get their own bases (the shared
// BASE below suits pawn and rook, whose widths match other sets already --
// the bishop has its own above, taller than this, for the same reason) and
// everything else of theirs is the old drawing scaled about the middle with
// the cross left as it was: a small cross over a wide crown is what the
// other sets look like too. The king's crown itself was later redrawn to
// match the boards' far more closely -- that comment sits at its own arrays.
const double QUEEN_CROWN[11][2] = {{26.4, 56}, {16.2, 30}, {34.65, 46}, {32.1, 24}, {41.55, 46},
                                   {50, 18},   {58.45, 46}, {67.9, 24}, {73.35, 46}, {83.8, 30},
                                   {78.6, 56}};
const double QUEEN_BAND[4][2] = {{18.8, 56}, {81.2, 56}, {81.2, 64}, {18.8, 64}};
const double QUEEN_BODY[4][2] = {{24, 64}, {76, 64}, {68.2, 74}, {32.8, 74}};
const double QUEEN_COLLAR[4][2] = {{24, 74}, {76, 74}, {76, 80}, {24, 80}};
const double QUEEN_JEWEL[5][2] = {{16.2, 30}, {32.1, 24}, {50, 18}, {67.9, 24}, {83.8, 30}};
const double QUEEN_BASE[4][2] = {{18.8, 80}, {81.2, 80}, {86.4, 89}, {13.6, 89}};

const double KING_CROSS_V[4][2] = {{46, 4}, {54, 4}, {54, 30}, {46, 30}};
const double KING_CROSS_H[4][2] = {{36, 12}, {64, 12}, {64, 20}, {36, 20}};
// The crown, redrawn from the boards -- the old one was this shape inside
// out. Their king wears a solid dome from just under the cross, its widest
// rows (90% of the piece) at a third of the height, and only below that
// opens into three prongs with a window between each; ours drew three thin
// spikes above a solid base, and began three rows lower than theirs does.
// On the green board that put the template's pixels where the observed king
// had almost none, and none where it had its fattest: rows 10..17 read
// 5..21 cells against the screen's 21..36, a deficit of two hundred cells
// that no other term could pay back -- the king lost its own squares to a
// queen drawn for a different set. The dome's widths are read straight off
// that board's profile (21, 30, 32, 34, 35..36 cells at rows 10..17), the
// windows off its rows 18..24 (runs of about 10, 12, 6 cells with 4-cell
// gaps), and the flare closes them at row 25 exactly where the screen's
// does. The crown is one contour: the windows are notches cut down from its
// top edge, so the dome above and the skirt below still close them.
const double KING_DOME[13][2] = {{16, 40},     {16.5, 35.9}, {17.5, 33.7}, {19.4, 31.6},
                                 {21.3, 29.5}, {30.5, 26.4}, {50, 25.6},   {69.5, 26.4},
                                 {78.7, 29.5}, {80.6, 31.6}, {82.5, 33.7}, {83.5, 35.9},
                                 {84, 40}};
const double KING_CROWN[12][2] = {{16, 40},  {28.7, 40}, {28.7, 57},   {40.8, 57},
                                  {40.8, 40}, {59.3, 40}, {59.3, 57},   {70.8, 57},
                                  {70.8, 40}, {84, 40},   {84, 57},     {16, 57}};
const double KING_SKIRT[6][2] = {{18, 57.6},   {82, 57.6},   {80.4, 60.5}, {76.4, 64},
                                 {23.6, 64},   {19.6, 60.5}};
const double KING_BAND[4][2] = {{23.6, 57.6}, {76.4, 57.6}, {76.4, 64}, {23.6, 64}};
const double KING_BODY[4][2] = {{26.24, 64}, {73.76, 64}, {68.48, 74}, {33.52, 74}};
const double KING_COLLAR[4][2] = {{23.6, 74}, {76.4, 74}, {76.4, 80}, {23.6, 80}};
const double KING_BASE[4][2] = {{18.3, 80}, {81.7, 80}, {87, 89}, {13, 89}};

// The other two sets read off screens -- chess.com's own, the purple board's
// -- crown their royals where ours bands them: the crown or dome reaches the
// full width of the piece at a third of its height, the body then tapers in
// one continuous line to a waist, flares once for the foot, and the slab
// stays narrower than the crown. Our classic drawing has the slab widest of
// all and a broad band under the crown, so on those boards the queen's
// silhouette overlaps our *bishop* more than our queen and loses the square
// to it. These are a second opinion, not a replacement: whichever of the
// two drawings scores higher for a type is the one the reader compares.
//
// The crown's valley tiers sit below the queens' own top-fifth row: their
// queens measure 0.40 and 0.33 in the widest row of the box's top fifth,
// the green board's pawns 0.54, and the widest row decides `t` -- with the
// ball above them the fifth ends while the spikes are still separate
// (0.40, their queens to within a rounding) instead of closing into the
// 0.51 that beat our pawn on green and nearly lost the purple squares to
// our bishop. The valleys at y34.5 keep that merge just past the fifth and
// reopen the outer notches at y36, where their crowns are widest.
const double FQ_CROWN[11][2] = {{24, 49},   {17.5, 36}, {30, 38.5}, {37.5, 25}, {44, 34.5},
                                {50, 21.5}, {56, 34.5}, {62.5, 25}, {70, 38.5}, {82.5, 36},
                                {76, 49}};
const double FQ_BODY[8][2] = {{25, 46}, {75, 46}, {71, 55}, {69, 63},
                              {68.5, 70}, {31.5, 70}, {31, 63}, {29, 55}};
const double FQ_SKIRT[4][2] = {{31, 69}, {69, 69}, {76, 77}, {24, 77}};
const double FQ_SLAB[4][2] = {{23.5, 77}, {76.5, 77}, {77.5, 89}, {22.5, 89}};
const double FQ_JEWEL[3][2] = {{37.5, 26.5}, {50, 21.5}, {62.5, 26.5}};

const double FK_CROSS_V[4][2] = {{44, 10}, {56, 10}, {56, 40}, {44, 40}};
const double FK_CROSS_H[4][2] = {{35.5, 16}, {64.5, 16}, {64.5, 26}, {35.5, 26}};
const double FK_DOME[14][2] = {{32, 36}, {22, 39}, {19, 47},  {21, 55}, {25, 61}, {28, 67},
                               {31, 71}, {69, 71}, {72, 67},  {75, 61}, {79, 55}, {81, 47},
                               {78, 39}, {68, 36}};
const double FK_CAP[4][2] = {{41, 29}, {59, 29}, {59, 37}, {41, 37}};
const double FK_LEDGE[4][2] = {{25, 71}, {75, 71}, {76, 79}, {24, 79}};
const double FK_SLAB[4][2] = {{23.5, 79}, {76.5, 79}, {77.5, 89}, {22.5, 89}};

struct Palette {
    SDL_Color fill;
    SDL_Color line;
    SDL_Color accent;
};

Palette paletteFor(Color c) {
    if (c == WHITE) return Palette{{250, 250, 246, 255}, {34, 34, 40, 255}, {34, 34, 40, 255}};
    return Palette{{44, 46, 53, 255}, {14, 14, 18, 255}, {238, 238, 232, 255}};
}

void fillCircle(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c) {
    if (rad <= 0) return;
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    for (int dy = -rad; dy <= rad; ++dy) {
        int dx = (int)std::sqrt(double(rad) * rad - double(dy) * dy);
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

void strokeCircle(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c, int width) {
    if (rad <= 0) return;
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    const int segments = std::max(16, rad * 4);
    int prevX = cx + rad, prevY = cy;
    for (int i = 1; i <= segments; ++i) {
        double a = 2.0 * PI * double(i) / double(segments);
        int x = cx + (int)std::lround(std::cos(a) * rad);
        int y = cy + (int)std::lround(std::sin(a) * rad);
        SDL_RenderDrawLine(r, prevX, prevY, x, y);
        prevX = x;
        prevY = y;
    }
    (void)width;
}

void fillPolygon(SDL_Renderer* r, const SDL_Point* pts, int n, SDL_Color c) {
    if (n < 3) return;
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);

    int minY = pts[0].y, maxY = pts[0].y;
    for (int i = 1; i < n; ++i) {
        minY = std::min(minY, pts[i].y);
        maxY = std::max(maxY, pts[i].y);
    }

    std::vector<double> xhits;
    for (int y = minY; y <= maxY; ++y) {
        xhits.clear();
        const double yc = y + 0.5;
        for (int i = 0; i < n; ++i) {
            const SDL_Point& a = pts[i];
            const SDL_Point& b = pts[(i + 1) % n];
            if (a.y == b.y) continue;
            if ((yc >= a.y && yc < b.y) || (yc >= b.y && yc < a.y)) {
                double t = (yc - double(a.y)) / double(b.y - a.y);
                xhits.push_back(double(a.x) + t * double(b.x - a.x));
            }
        }
        if (xhits.empty()) continue;
        std::sort(xhits.begin(), xhits.end());
        for (size_t i = 0; i + 1 < xhits.size(); i += 2) {
            int x0 = (int)std::ceil(xhits[i] - 0.5);
            int x1 = (int)std::floor(xhits[i + 1] - 0.5);
            if (x1 < x0) x1 = x0;
            SDL_RenderDrawLine(r, x0, y, x1, y);
        }
    }
}

void strokePolygon(SDL_Renderer* r, const SDL_Point* pts, int n, SDL_Color c, int width) {
    if (n < 2) return;
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    for (int i = 0; i < n; ++i) {
        const SDL_Point& a = pts[i];
        const SDL_Point& b = pts[(i + 1) % n];
        // A couple of offset passes emulate a thicker pen without SDL_gfx.
        if (width <= 1) {
            SDL_RenderDrawLine(r, a.x, a.y, b.x, b.y);
        } else {
            for (int o = 0; o < width; ++o) SDL_RenderDrawLine(r, a.x + o, a.y, b.x + o, b.y);
        }
    }
}

// Open polyline (no closing segment), used for the knight's mane.
void strokePath(SDL_Renderer* r, const SDL_Point* pts, int n, SDL_Color c, int width) {
    if (n < 2) return;
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    for (int i = 0; i + 1 < n; ++i) {
        const SDL_Point& a = pts[i];
        const SDL_Point& b = pts[i + 1];
        if (width <= 1) SDL_RenderDrawLine(r, a.x, a.y, b.x, b.y);
        else
            for (int o = 0; o < width; ++o) SDL_RenderDrawLine(r, a.x + o, a.y, b.x + o, b.y);
    }
}

}  // namespace

void drawPiece(SDL_Renderer* r, Piece piece, const SDL_Rect& box, PieceStyle st) {
    if (isEmpty(piece) || box.w <= 0 || box.h <= 0) return;
    const Color col = colorOf(piece);
    const PieceType type = typeOf(piece);
    const Palette pal = paletteFor(col);

    auto X = [&](double v) { return box.x + (int)std::lround(v * box.w / 100.0); };
    auto Y = [&](double v) { return box.y + (int)std::lround(v * box.h / 100.0); };
    auto R = [&](double v) { return std::max(1, (int)std::lround(v * box.w / 100.0)); };

    const int lineW = std::max(1, box.w / 44);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    std::vector<SDL_Point> poly;
    auto shape = [&](const double (*pts)[2], int n) {
        poly.resize(n);
        for (int i = 0; i < n; ++i) {
            poly[i].x = X(pts[i][0]);
            poly[i].y = Y(pts[i][1]);
        }
        fillPolygon(r, poly.data(), n, pal.fill);
        strokePolygon(r, poly.data(), n, pal.line, lineW);
    };
    auto ball = [&](double cx, double cy, double rad, SDL_Color fill, SDL_Color line) {
        fillCircle(r, X(cx), Y(cy), R(rad), fill);
        strokeCircle(r, X(cx), Y(cy), R(rad), line, lineW);
    };

    // Drawn bottom-up so upper parts overlap the lower outlines.
    switch (type) {
        case PAWN:
            shape(BASE, 4);
            shape(PAWN_COLLAR, 4);
            shape(PAWN_BODY, 4);
            ball(PAWN_HEAD[0][0], PAWN_HEAD[0][1], 12, pal.fill, pal.line);
            break;

        case ROOK:
            shape(BASE, 4);
            shape(ROOK_COLLAR, 4);
            shape(ROOK_BODY, 4);
            shape(ROOK_NECK, 4);
            shape(ROOK_TOP, 12);
            break;

        case KNIGHT:
            shape(BASE, 4);
            shape(KNIGHT_COLLAR, 4);
            shape(KNIGHT_HEAD, 15);
            poly.resize(3);
            for (int i = 0; i < 3; ++i) {
                poly[i].x = X(KNIGHT_MANE[i][0]);
                poly[i].y = Y(KNIGHT_MANE[i][1]);
            }
            strokePath(r, poly.data(), 3, pal.line, lineW);
            fillCircle(r, X(36), Y(33), R(3), pal.accent);
            break;

        case BISHOP:
            shape(BISHOP_BASE, 4);
            shape(BISHOP_COLLAR, 4);
            shape(BISHOP_BODY, 12);
            ball(50, 30, 11, pal.fill, pal.line);
            {
                // Mitre slit, an upper-left chord through the mitre.
                SDL_SetRenderDrawColor(r, pal.line.r, pal.line.g, pal.line.b, 255);
                SDL_RenderDrawLine(r, X(43), Y(23), X(55), Y(35));
            }
            // The finial sits low enough to bite into the mitre's ball: two
            // circles that merely kiss at a pixel are two blobs to the
            // reader, and the smaller one is thrown away with the letter in
            // the corner -- which left this piece a fifth shorter than every
            // other set draws it. Drawn a touch broader than the old one:
            // their bishops wear a domed cap, not a pin.
            ball(50, 17, 7, pal.fill, pal.line);
            break;


        case QUEEN: {
            if (st == PieceStyle::foreign) {
                shape(FQ_SLAB, 4);
                shape(FQ_SKIRT, 4);
                shape(FQ_BODY, 8);
                shape(FQ_CROWN, 11);
                // The ball their queens wear above the crown. It is not
                // only likeness: the box's top fifth is where the `t` term
                // looks, and the crown's spikes close into one solid row a
                // little lower down -- with no ball the piece began at the
                // jewel, the fifth ended right at that solid row, and the
                // widest-top reading came out 0.51 where the queens on the
                // purple board measure 0.40 and a pawn 0.54 (so this
                // drawing beat our pawn on green and nearly lost the
                // purple squares to our bishop). The ball lifts the box's
                // top to y10.5, the fifth then ends above the merge, and
                // the reading lands on 0.40 -- their queens' own.
                ball(50, 16.5, 6, pal.fill, pal.line);
                for (int i = 0; i < 3; ++i)
                    ball(FQ_JEWEL[i][0], FQ_JEWEL[i][1], i == 1 ? 4.5 : 4, pal.fill, pal.line);
                break;
            }
            shape(QUEEN_BASE, 4);
            shape(QUEEN_COLLAR, 4);
            shape(QUEEN_BODY, 4);
            shape(QUEEN_BAND, 4);
            shape(QUEEN_CROWN, 11);
            for (int i = 0; i < 5; ++i)
                ball(QUEEN_JEWEL[i][0], QUEEN_JEWEL[i][1], i == 2 ? 4.5 : 4, pal.fill, pal.line);
            break;
        }

        case KING: {
            if (st == PieceStyle::foreign) {
                shape(FK_SLAB, 4);
                shape(FK_LEDGE, 4);
                shape(FK_DOME, 14);
                shape(FK_CAP, 4);
                shape(FK_CROSS_H, 4);
                shape(FK_CROSS_V, 4);
                break;
            }
            shape(KING_BASE, 4);
            shape(KING_COLLAR, 4);
            shape(KING_BODY, 4);
            shape(KING_BAND, 4);
            shape(KING_SKIRT, 6);
            shape(KING_CROWN, 12);
            shape(KING_DOME, 13);
            shape(KING_CROSS_H, 4);
            shape(KING_CROSS_V, 4);
            break;
        }

        default:
            break;
    }
}

void drawPieceIcon(SDL_Renderer* r, Piece piece, const SDL_Rect& box) {
    drawPiece(r, piece, box);
}

void drawFillCircle(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    fillCircle(r, cx, cy, rad, c);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void drawStrokeCircle(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c, int width) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    strokeCircle(r, cx, cy, rad, c, width);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}
