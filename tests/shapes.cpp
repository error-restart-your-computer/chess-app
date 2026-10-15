// tests/shapes.cpp -- renders every piece type at a large size for visual inspection
#include <SDL.h>
#include <cstdio>

#include "../src/pieces.h"

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "%s\n", SDL_GetError());
        return 1;
    }
    const int cell = 220;
    const int cols = 6, rows = 2;
    SDL_Window* w = SDL_CreateWindow("shapes", 0, 0, cell * cols, cell * rows, SDL_WINDOW_HIDDEN);
    SDL_Renderer* r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    if (!r) {
        std::fprintf(stderr, "%s\n", SDL_GetError());
        return 1;
    }

    SDL_SetRenderDrawColor(r, 118, 150, 98, 255);
    SDL_RenderClear(r);
    static const PieceType types[6] = {PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING};
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            SDL_Rect box{col * cell + 10, row * cell + 10, cell - 20, cell - 20};
            Piece p = makePiece(types[col], row == 0 ? WHITE : BLACK);
            drawPiece(r, p, box);
        }
    }

    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, cell * cols, cell * rows, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_SaveBMP(s, "shapes.bmp");
    SDL_FreeSurface(s);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
