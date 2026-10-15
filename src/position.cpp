#include "position.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

struct Zobrist {
    uint64_t psq[64][12];
    uint64_t ep[8];
    uint64_t castle[16];
    uint64_t side;

    Zobrist() {
        uint64_t s = 0x9E3779B97F4A7C15ULL;
        auto rnd = [&s]() {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            return s * 2685821657736338717ULL;
        };
        for (int i = 0; i < 64; ++i)
            for (int j = 0; j < 12; ++j) psq[i][j] = rnd();
        for (int i = 0; i < 8; ++i) ep[i] = rnd();
        for (int i = 0; i < 16; ++i) castle[i] = rnd();
        side = rnd();
    }
};

const Zobrist& zobrist() {
    static const Zobrist z;
    return z;
}

const int KNIGHT_D[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
const int KING_D[8][2] = {{1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}, {0, 1}};
const int DIAG_D[4][2] = {{1, 1}, {1, -1}, {-1, -1}, {-1, 1}};
const int ORTHO_D[4][2] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}};

inline int zPieceIndex(Piece p) {
    PieceType t = typeOf(p);
    return (colorOf(p) == WHITE ? 0 : 6) + (int(t) - 1);
}

void addPawnMove(std::vector<Move>& out, int from, int to, int targetRank, Color us) {
    if (targetRank == 0 || targetRank == 7) {
        static const PieceType promos[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
        for (PieceType pr : promos) out.push_back(Move{(uint8_t)from, (uint8_t)to, pr, false, false});
    } else {
        out.push_back(Move{(uint8_t)from, (uint8_t)to, NO_PIECE, false, false});
    }
    (void)us;
}

}  // namespace

uint64_t Position::computeKey() const {
    const Zobrist& z = zobrist();
    uint64_t k = 0;
    for (int sq = 0; sq < 64; ++sq)
        if (board[sq]) k ^= z.psq[sq][zPieceIndex(board[sq])];
    if (side == BLACK) k ^= z.side;
    if (epSquare >= 0) k ^= z.ep[fileOf(epSquare)];
    k ^= z.castle[castling];
    return k;
}

void Position::clear() {
    std::memset(board, 0, sizeof(board));
    side = WHITE;
    castling = 0;
    epSquare = -1;
    halfmoveClock = 0;
    fullmove = 1;
    kingSq[0] = 4;
    kingSq[1] = 60;
    history.clear();
    key = computeKey();
    history.push_back(key);
}

void Position::setFen(const std::string& fen) {
    clear();
    std::string boardPart, sidePart, castlePart, epPart;
    int half = 0, full = 1;
    {
        int idx = 0;
        auto next = [&]() {
            while (idx < (int)fen.size() && fen[idx] == ' ') ++idx;
            int start = idx;
            while (idx < (int)fen.size() && fen[idx] != ' ') ++idx;
            return fen.substr(start, idx - start);
        };
        boardPart = next();
        sidePart = next();
        castlePart = next();
        epPart = next();
        std::string h = next();
        std::string f = next();
        if (!h.empty()) half = std::atoi(h.c_str());
        if (!f.empty()) full = std::atoi(f.c_str());
    }

    int rank = 7, file = 0;
    for (char c : boardPart) {
        if (c == '/') {
            --rank;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
        } else {
            Color col = (c >= 'a') ? BLACK : WHITE;
            PieceType t = NO_PIECE;
            switch (char(c | 32)) {
                case 'p': t = PAWN; break;
                case 'n': t = KNIGHT; break;
                case 'b': t = BISHOP; break;
                case 'r': t = ROOK; break;
                case 'q': t = QUEEN; break;
                case 'k': t = KING; break;
                default: break;
            }
            if (t != NO_PIECE && onBoard(file, rank)) {
                int sq = makeSquare(file, rank);
                board[sq] = makePiece(t, col);
                if (t == KING) kingSq[col] = (int8_t)sq;
            }
            ++file;
        }
    }

    side = (sidePart == "b") ? BLACK : WHITE;
    castling = 0;
    for (char c : castlePart) {
        if (c == 'K') castling |= CASTLE_WK;
        else if (c == 'Q') castling |= CASTLE_WQ;
        else if (c == 'k') castling |= CASTLE_BK;
        else if (c == 'q') castling |= CASTLE_BQ;
    }
    if (epPart.size() == 2 && epPart[0] >= 'a' && epPart[0] <= 'h')
        epSquare = (int8_t)makeSquare(epPart[0] - 'a', epPart[1] - '1');
    halfmoveClock = (uint8_t)std::max(0, half);
    fullmove = (int16_t)std::max(1, full);

    history.clear();
    key = computeKey();
    history.push_back(key);
}

std::string Position::toFen() const {
    std::string out;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            Piece p = board[makeSquare(f, r)];
            if (isEmpty(p)) {
                ++empty;
                continue;
            }
            if (empty) {
                out += char('0' + empty);
                empty = 0;
            }
            static const char letters[7] = {' ', 'p', 'n', 'b', 'r', 'q', 'k'};
            char c = letters[typeOf(p)];
            if (colorOf(p) == WHITE) c = char(c - 32);
            out += c;
        }
        if (empty) out += char('0' + empty);
        if (r) out += '/';
    }
    out += side == WHITE ? " w " : " b ";
    if (!castling) out += '-';
    if (castling & CASTLE_WK) out += 'K';
    if (castling & CASTLE_WQ) out += 'Q';
    if (castling & CASTLE_BK) out += 'k';
    if (castling & CASTLE_BQ) out += 'q';
    out += ' ';
    out += epSquare >= 0 ? squareName(epSquare) : std::string("-");
    out += ' ' + std::to_string(halfmoveClock) + ' ' + std::to_string(fullmove);
    return out;
}

int8_t Position::findKing(Color c) const {
    Piece k = makePiece(KING, c);
    for (int sq = 0; sq < 64; ++sq)
        if (board[sq] == k) return (int8_t)sq;
    return (int8_t)kingSq[c];
}

int Position::countPieces(Color c, PieceType t) const {
    Piece p = makePiece(t, c);
    int n = 0;
    for (int sq = 0; sq < 64; ++sq)
        if (board[sq] == p) ++n;
    return n;
}

void Position::generatePseudo(std::vector<Move>& out) const {
    const Color us = side, them = ~side;

    for (int sq = 0; sq < 64; ++sq) {
        Piece p = board[sq];
        if (isEmpty(p) || colorOf(p) != us) continue;
        PieceType t = typeOf(p);
        int f = fileOf(sq), r = rankOf(sq);

        switch (t) {
            case PAWN: {
                const int dir = (us == WHITE) ? 1 : -1;
                const int startRank = (us == WHITE) ? 1 : 6;
                int r1 = r + dir;
                if (onBoard(f, r1) && isEmpty(board[makeSquare(f, r1)])) {
                    addPawnMove(out, sq, makeSquare(f, r1), r1, us);
                    int r2 = r + 2 * dir;
                    if (r == startRank && isEmpty(board[makeSquare(f, r2)]))
                        out.push_back(Move{(uint8_t)sq, (uint8_t)makeSquare(f, r2), NO_PIECE, false, false});
                }
                for (int df = -1; df <= 1; df += 2) {
                    int nf = f + df;
                    if (!onBoard(nf, r1)) continue;
                    int tgt = makeSquare(nf, r1);
                    Piece tp = board[tgt];
                    if (!isEmpty(tp) && colorOf(tp) == them) {
                        addPawnMove(out, sq, tgt, r1, us);
                    } else if (isEmpty(tp) && tgt == epSquare) {
                        out.push_back(Move{(uint8_t)sq, (uint8_t)tgt, NO_PIECE, true, false});
                    }
                }
                break;
            }
            case KNIGHT:
            case KING: {
                const int(*d)[2] = (t == KNIGHT) ? KNIGHT_D : KING_D;
                for (int i = 0; i < 8; ++i) {
                    int nf = f + d[i][0], nr = r + d[i][1];
                    if (!onBoard(nf, nr)) continue;
                    int tgt = makeSquare(nf, nr);
                    Piece tp = board[tgt];
                    if (isEmpty(tp) || colorOf(tp) == them)
                        out.push_back(Move{(uint8_t)sq, (uint8_t)tgt, NO_PIECE, false, false});
                }
                break;
            }
            case BISHOP:
            case ROOK:
            case QUEEN: {
                const bool diag = (t == BISHOP || t == QUEEN);
                const bool ortho = (t == ROOK || t == QUEEN);
                if (diag) {
                    for (int i = 0; i < 4; ++i) {
                        int nf = f + DIAG_D[i][0], nr = r + DIAG_D[i][1];
                        while (onBoard(nf, nr)) {
                            int tgt = makeSquare(nf, nr);
                            Piece tp = board[tgt];
                            if (isEmpty(tp)) {
                                out.push_back(Move{(uint8_t)sq, (uint8_t)tgt, NO_PIECE, false, false});
                            } else {
                                if (colorOf(tp) == them)
                                    out.push_back(Move{(uint8_t)sq, (uint8_t)tgt, NO_PIECE, false, false});
                                break;
                            }
                            nf += DIAG_D[i][0];
                            nr += DIAG_D[i][1];
                        }
                    }
                }
                if (ortho) {
                    for (int i = 0; i < 4; ++i) {
                        int nf = f + ORTHO_D[i][0], nr = r + ORTHO_D[i][1];
                        while (onBoard(nf, nr)) {
                            int tgt = makeSquare(nf, nr);
                            Piece tp = board[tgt];
                            if (isEmpty(tp)) {
                                out.push_back(Move{(uint8_t)sq, (uint8_t)tgt, NO_PIECE, false, false});
                            } else {
                                if (colorOf(tp) == them)
                                    out.push_back(Move{(uint8_t)sq, (uint8_t)tgt, NO_PIECE, false, false});
                                break;
                            }
                            nf += ORTHO_D[i][0];
                            nr += ORTHO_D[i][1];
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // Castling
    const Color opp = them;
    if (!inCheck(us)) {
        if (us == WHITE) {
            if ((castling & CASTLE_WK) && isEmpty(board[5]) && isEmpty(board[6]) &&
                board[7] == makePiece(ROOK, WHITE) && !isAttacked(5, opp) && !isAttacked(6, opp))
                out.push_back(Move{4, 6, NO_PIECE, false, true});
            if ((castling & CASTLE_WQ) && isEmpty(board[3]) && isEmpty(board[2]) && isEmpty(board[1]) &&
                board[0] == makePiece(ROOK, WHITE) && !isAttacked(3, opp) && !isAttacked(2, opp))
                out.push_back(Move{4, 2, NO_PIECE, false, true});
        } else {
            if ((castling & CASTLE_BK) && isEmpty(board[61]) && isEmpty(board[62]) &&
                board[63] == makePiece(ROOK, BLACK) && !isAttacked(61, opp) && !isAttacked(62, opp))
                out.push_back(Move{60, 62, NO_PIECE, false, true});
            if ((castling & CASTLE_BQ) && isEmpty(board[59]) && isEmpty(board[58]) && isEmpty(board[57]) &&
                board[56] == makePiece(ROOK, BLACK) && !isAttacked(59, opp) && !isAttacked(58, opp))
                out.push_back(Move{60, 58, NO_PIECE, false, true});
        }
    }
}

void Position::doMove(const Move& m, UndoInfo& u) {
    u.captured = 0;
    u.castling = castling;
    u.epSquare = epSquare;
    u.halfmoveClock = halfmoveClock;
    u.fullmove = fullmove;
    u.key = key;
    u.kingSq[0] = kingSq[0];
    u.kingSq[1] = kingSq[1];

    const Piece p = board[m.from];
    const Color us = side;
    const PieceType t = typeOf(p);

    if (m.enPassant) {
        int capSq = m.to + (us == WHITE ? -8 : 8);
        u.captured = board[capSq];
        board[capSq] = 0;
    } else {
        u.captured = board[m.to];
    }

    board[m.to] = (m.promotion != NO_PIECE) ? makePiece(m.promotion, us) : p;
    board[m.from] = 0;

    if (t == KING) {
        kingSq[us] = (int8_t)m.to;
        castling &= (us == WHITE) ? uint8_t(~(CASTLE_WK | CASTLE_WQ)) : uint8_t(~(CASTLE_BK | CASTLE_BQ));
        if (m.castle) {
            int rank = (us == WHITE) ? 0 : 7;
            int rookFrom = (fileOf(m.to) == 6) ? 7 : 0;
            int rookTo = (fileOf(m.to) == 6) ? 5 : 3;
            board[makeSquare(rookTo, rank)] = board[makeSquare(rookFrom, rank)];
            board[makeSquare(rookFrom, rank)] = 0;
        }
    }

    // Rook leaving or being captured on its home square clears castling rights.
    if (m.from == 0 || m.to == 0) castling &= uint8_t(~CASTLE_WQ);
    if (m.from == 7 || m.to == 7) castling &= uint8_t(~CASTLE_WK);
    if (m.from == 56 || m.to == 56) castling &= uint8_t(~CASTLE_BQ);
    if (m.from == 63 || m.to == 63) castling &= uint8_t(~CASTLE_BK);

    epSquare = -1;
    if (t == PAWN) {
        halfmoveClock = 0;
        if (std::abs(int(m.to) - int(m.from)) == 16) epSquare = (int8_t)((m.from + m.to) / 2);
    } else if (!isEmpty(u.captured)) {
        halfmoveClock = 0;
    } else {
        ++halfmoveClock;
    }

    if (us == BLACK) ++fullmove;
    side = ~side;
    key = computeKey();
    history.push_back(key);
}

void Position::undoMove(const Move& m, const UndoInfo& u) {
    side = ~side;
    const Color us = side;

    Piece moving = board[m.to];
    board[m.from] = (m.promotion != NO_PIECE) ? makePiece(PAWN, us) : moving;
    board[m.to] = 0;

    if (m.enPassant) {
        board[m.to + (us == WHITE ? -8 : 8)] = u.captured;
    } else {
        board[m.to] = u.captured;
    }

    if (m.castle) {
        int rank = (us == WHITE) ? 0 : 7;
        int rookFrom = (fileOf(m.to) == 6) ? 7 : 0;
        int rookTo = (fileOf(m.to) == 6) ? 5 : 3;
        board[makeSquare(rookFrom, rank)] = board[makeSquare(rookTo, rank)];
        board[makeSquare(rookTo, rank)] = 0;
    }

    castling = u.castling;
    epSquare = u.epSquare;
    halfmoveClock = u.halfmoveClock;
    fullmove = u.fullmove;
    kingSq[0] = u.kingSq[0];
    kingSq[1] = u.kingSq[1];
    key = u.key;
    if (!history.empty()) history.pop_back();
}

void Position::doNullMove(UndoInfo& u) {
    u = UndoInfo();
    u.castling = castling;
    u.epSquare = epSquare;
    u.halfmoveClock = halfmoveClock;
    u.fullmove = fullmove;
    u.key = key;
    u.kingSq[0] = kingSq[0];
    u.kingSq[1] = kingSq[1];

    epSquare = -1;  // the right to capture e.p. lives for exactly one move
    side = ~side;
    key = computeKey();
}

void Position::undoNullMove(const UndoInfo& u) {
    side = ~side;
    castling = u.castling;
    epSquare = u.epSquare;
    halfmoveClock = u.halfmoveClock;
    fullmove = u.fullmove;
    key = u.key;
    kingSq[0] = u.kingSq[0];
    kingSq[1] = u.kingSq[1];
}

bool Position::isAttacked(int sq, Color by) const {
    int f = fileOf(sq), r = rankOf(sq);

    // Pawns: a `by` pawn attacking `sq` sits one rank behind it.
    int pr = r + (by == WHITE ? -1 : 1);
    for (int df = -1; df <= 1; df += 2) {
        int nf = f + df;
        if (onBoard(nf, pr) && board[makeSquare(nf, pr)] == makePiece(PAWN, by)) return true;
    }

    for (int i = 0; i < 8; ++i) {
        int nf = f + KNIGHT_D[i][0], nr = r + KNIGHT_D[i][1];
        if (onBoard(nf, nr) && board[makeSquare(nf, nr)] == makePiece(KNIGHT, by)) return true;
    }
    for (int i = 0; i < 8; ++i) {
        int nf = f + KING_D[i][0], nr = r + KING_D[i][1];
        if (onBoard(nf, nr) && board[makeSquare(nf, nr)] == makePiece(KING, by)) return true;
    }

    for (int i = 0; i < 4; ++i) {
        int nf = f + DIAG_D[i][0], nr = r + DIAG_D[i][1];
        while (onBoard(nf, nr)) {
            Piece p = board[makeSquare(nf, nr)];
            if (!isEmpty(p)) {
                if (colorOf(p) == by && (typeOf(p) == BISHOP || typeOf(p) == QUEEN)) return true;
                break;
            }
            nf += DIAG_D[i][0];
            nr += DIAG_D[i][1];
        }
    }
    for (int i = 0; i < 4; ++i) {
        int nf = f + ORTHO_D[i][0], nr = r + ORTHO_D[i][1];
        while (onBoard(nf, nr)) {
            Piece p = board[makeSquare(nf, nr)];
            if (!isEmpty(p)) {
                if (colorOf(p) == by && (typeOf(p) == ROOK || typeOf(p) == QUEEN)) return true;
                break;
            }
            nf += ORTHO_D[i][0];
            nr += ORTHO_D[i][1];
        }
    }
    return false;
}

std::vector<Move> Position::legalMoves() {
    std::vector<Move> pseudo;
    pseudo.reserve(48);
    generatePseudo(pseudo);

    std::vector<Move> out;
    out.reserve(pseudo.size());
    const Color us = side;
    for (const Move& m : pseudo) {
        UndoInfo u;
        doMove(m, u);
        bool ok = !isAttacked(kingSq[us], ~us);
        undoMove(m, u);
        if (ok) out.push_back(m);
    }
    return out;
}

int Position::repetitionCount() const {
    int n = 0;
    for (uint64_t k : history)
        if (k == key) ++n;
    return n;
}

bool Position::insufficientMaterial() const {
    int minorCount = 0;
    int bishopColors = 0;  // bit0: light-square bishop present, bit1: dark-square
    bool onlyBishops = true;
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = board[sq];
        if (isEmpty(p)) continue;
        PieceType t = typeOf(p);
        if (t == KING) continue;
        ++minorCount;
        if (t == PAWN || t == ROOK || t == QUEEN) return false;
        if (t == KNIGHT) onlyBishops = false;
        if (t == BISHOP) {
            bool light = ((fileOf(sq) + rankOf(sq)) & 1) != 0;
            bishopColors |= light ? 1 : 2;
        }
    }
    if (minorCount == 0) return true;                        // K vs K
    if (minorCount == 1) return true;                        // K+B vs K or K+N vs K
    if (onlyBishops && bishopColors != 3) return true;       // bishops all on one colour
    return false;
}

GameStatus Position::status(const std::vector<Move>& legal) const {
    if (legal.empty()) return inCheck(side) ? GameStatus::CHECKMATE : GameStatus::STALEMATE;
    if (halfmoveClock >= 100) return GameStatus::FIFTY_MOVE;
    if (repetitionCount() >= 3) return GameStatus::THREEFOLD;
    if (insufficientMaterial()) return GameStatus::INSUFFICIENT_MATERIAL;
    return GameStatus::ONGOING;
}

GameStatus Position::status() {
    std::vector<Move> legal = legalMoves();
    return status(legal);
}

std::string Position::toSan(const Move& m) {
    std::string san;
    const Piece p = board[m.from];
    const PieceType t = typeOf(p);
    const bool capture = !isEmpty(board[m.to]) || m.enPassant;

    if (m.castle) {
        san = (fileOf(m.to) == 6) ? "O-O" : "O-O-O";
    } else if (t == PAWN) {
        if (capture) san += char('a' + fileOf(m.from)), san += 'x';
        san += squareName(m.to);
        if (m.promotion != NO_PIECE) {
            san += '=';
            san += pieceLetter(m.promotion);
        }
    } else {
        san += pieceLetter(t);
        // Disambiguation
        std::vector<Move> legal = legalMoves();
        bool sameFile = false, sameRank = false, otherExists = false;
        for (const Move& o : legal) {
            if (o.to != m.to || o.from == m.from) continue;
            if (typeOf(board[o.from]) != t || colorOf(board[o.from]) != colorOf(p)) continue;
            otherExists = true;
            if (fileOf(o.from) == fileOf(m.from)) sameFile = true;
            if (rankOf(o.from) == rankOf(m.from)) sameRank = true;
        }
        if (otherExists) {
            if (!sameFile) san += char('a' + fileOf(m.from));
            else if (!sameRank) san += char('1' + rankOf(m.from));
            else san += squareName(m.from);
        }
        if (capture) san += 'x';
        san += squareName(m.to);
    }

    // Check / mate suffix
    UndoInfo u;
    doMove(m, u);
    Color opp = side;
    if (isAttacked(kingSq[opp], ~opp)) {
        std::vector<Move> oppMoves = legalMoves();
        san += oppMoves.empty() ? '#' : '+';
    }
    undoMove(m, u);
    return san;
}
