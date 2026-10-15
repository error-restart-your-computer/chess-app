#pragma once
// common.h -- shared chess types and small helpers
#include <cstdint>
#include <string>

enum Color : uint8_t { WHITE = 0, BLACK = 1, COLOR_NB = 2 };

enum PieceType : int8_t {
    NO_PIECE = 0,
    PAWN = 1,
    KNIGHT = 2,
    BISHOP = 3,
    ROOK = 4,
    QUEEN = 5,
    KING = 6
};

// Piece encoding: 0 = empty, 1..6 = white pieces, -1..-6 = black pieces
using Piece = int8_t;

constexpr Piece makePiece(PieceType t, Color c) { return Piece(c == WHITE ? t : -t); }
constexpr PieceType typeOf(Piece p) { return p < 0 ? PieceType(-p) : PieceType(p); }
constexpr Color colorOf(Piece p) { return p < 0 ? BLACK : WHITE; }
constexpr bool isEmpty(Piece p) { return p == 0; }
constexpr Color operator~(Color c) { return Color(int(c) ^ 1); }

struct Move {
    uint8_t from = 0;
    uint8_t to = 0;
    PieceType promotion = NO_PIECE;
    bool enPassant = false;
    bool castle = false;

    bool operator==(const Move& o) const {
        return from == o.from && to == o.to && promotion == o.promotion;
    }
    bool operator!=(const Move& o) const { return !(*this == o); }
};

constexpr int fileOf(int sq) { return sq & 7; }
constexpr int rankOf(int sq) { return sq >> 3; }
constexpr int makeSquare(int f, int r) { return r * 8 + f; }
constexpr bool onBoard(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

inline std::string squareName(int sq) {
    std::string s;
    s += char('a' + fileOf(sq));
    s += char('1' + rankOf(sq));
    return s;
}

// Returns index into a "white perspective, rank 8 first" piece-square table.
inline int pstIndex(int sq) { return (7 - rankOf(sq)) * 8 + fileOf(sq); }

inline char pieceLetter(PieceType t) {
    switch (t) {
        case KNIGHT: return 'N';
        case BISHOP: return 'B';
        case ROOK:   return 'R';
        case QUEEN:  return 'Q';
        case KING:   return 'K';
        default:     return '?';
    }
}
