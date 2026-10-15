// art.cpp -- loading and drawing the piece images (see art.h)

#include "art.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

SDL_Texture* tex[2][6] = {};  // [color][type - PAWN]
bool ready = false;

const char* typeLetter(PieceType t) {
    switch (t) {
        case PAWN: return "P";
        case KNIGHT: return "N";
        case BISHOP: return "B";
        case ROOK: return "R";
        case QUEEN: return "Q";
        case KING: return "K";
        default: return "";
    }
}

void clear() {
    for (int c = 0; c < 2; ++c)
        for (int t = 0; t < 6; ++t) {
            if (tex[c][t]) SDL_DestroyTexture(tex[c][t]);
            tex[c][t] = nullptr;
        }
    ready = false;
}

}  // namespace

bool artLoad(SDL_Renderer* r, const char* dir) {
    clear();
    if (!r) return false;
    const char* env = std::getenv("PIECES");
    std::string base = dir && *dir ? std::string(dir) : (env && *env ? std::string(env)
                                                                     : std::string("pieces"));
    for (int ci = 0; ci < 2; ++ci) {
        const Color c = ci == 0 ? WHITE : BLACK;
        for (int ti = 0; ti < 6; ++ti) {
            const PieceType t = PieceType(ti + PAWN);
            const std::string path =
                base + "/" + (c == WHITE ? "w" : "b") + typeLetter(t) + ".png";
            int w = 0, h = 0, n = 0;
            unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
            if (!px) {
                // One missing piece and the whole set is off: a board half in
                // one artist's hand and half in another's is not a look.
                clear();
                return false;
            }
            SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
            if (s) {
                // Same byte order stb hands over (RGBA), one row at a time so
                // surface padding never enters into it.
                unsigned char* dst = (unsigned char*)s->pixels;
                const size_t row = (size_t)w * 4;
                for (int y = 0; y < h; ++y) std::memcpy(dst + (size_t)y * s->pitch, px + y * row, row);
                tex[ci][ti] = SDL_CreateTextureFromSurface(r, s);
                SDL_FreeSurface(s);
            }
            stbi_image_free(px);
            if (!tex[ci][ti]) {
                clear();
                return false;
            }
            SDL_SetTextureBlendMode(tex[ci][ti], SDL_BLENDMODE_BLEND);
        }
    }
    ready = true;
    return true;
}

bool artReady() { return ready; }

void artDraw(SDL_Renderer* r, Piece piece, const SDL_Rect& box) {
    if (!ready || isEmpty(piece)) return;
    SDL_Texture* t = tex[colorOf(piece) == BLACK][int(typeOf(piece)) - PAWN];
    if (!t) return;
    SDL_RenderCopy(r, t, nullptr, &box);
}

void artShutdown() { clear(); }
