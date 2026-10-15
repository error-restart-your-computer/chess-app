#pragma once
// position.h -- board state, move generation, SAN, game status
#include "common.h"
#include <string>
#include <vector>

#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

// Castling right bits
enum CastleRight : uint8_t {
    CASTLE_WK = 1,
    CASTLE_WQ = 2,
    CASTLE_BK = 4,
    CASTLE_BQ = 8
};

struct UndoInfo {
    Piece captured = 0;
    uint8_t castling = 0;
    int8_t epSquare = -1;
    uint8_t halfmoveClock = 0;
    int16_t fullmove = 1;
    uint64_t key = 0;
    int8_t kingSq[2] = {4, 60};
};

enum class GameStatus {
    ONGOING,
    CHECKMATE,
    STALEMATE,
    FIFTY_MOVE,
    THREEFOLD,
    INSUFFICIENT_MATERIAL
};

class Position {
public:
    Piece board[64] = {};
    Color side = WHITE;
    uint8_t castling = 0;
    int8_t epSquare = -1;
    uint8_t halfmoveClock = 0;
    int16_t fullmove = 1;
    int8_t kingSq[2] = {4, 60};
    uint64_t key = 0;
    std::vector<uint64_t> history;  // keys of current position and all ancestors

    void clear();
    void setFen(const std::string& fen);
    void setStartPosition() { setFen(START_FEN); }

    void doMove(const Move& m, UndoInfo& u);
    void undoMove(const Move& m, const UndoInfo& u);

    // A null move hands the turn back: nothing on the board moves, so only
    // the side to move, the e.p. square and the key change. The history is
    // deliberately not extended -- a pass is not a chess position and must
    // never count towards a repetition. This is what the search's null-move
    // pruning plays out.
    void doNullMove(UndoInfo& u);
    void undoNullMove(const UndoInfo& u);

    // Generates only fully legal moves (filters out self-check moves).
    std::vector<Move> legalMoves();
    // Pseudo-legal moves, used internally by legalMoves().
    void generatePseudo(std::vector<Move>& out) const;

    bool isAttacked(int sq, Color by) const;
    bool inCheck(Color c) const { return isAttacked(kingSq[c], ~c); }

    int repetitionCount() const;
    bool isRepetitionDraw() const { return repetitionCount() >= 2; }
    bool insufficientMaterial() const;

    GameStatus status(const std::vector<Move>& legal) const;
    GameStatus status();

    std::string toSan(const Move& m);          // needs legal moves internally
    uint64_t computeKey() const;

    Piece at(int sq) const { return board[sq]; }
    int countPieces(Color c, PieceType t) const;

    std::string toFen() const;

private:
    int8_t findKing(Color c) const;
};
