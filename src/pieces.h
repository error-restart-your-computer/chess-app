#pragma once
// pieces.h -- vector-drawn Staunton style chess pieces (no image assets required)
#include "common.h"
#include <SDL.h>

// Which drawing style a piece is rendered in. `classic` is our own Staunton
// art. `foreign` draws queen and king the way the other sets on the internet
// draw them -- a crown or dome that is the widest part of the piece, body
// tapering straight to a slab no wider than the crown -- so the reader has
// both silhouettes to compare a screen against. Types with no second
// drawing look the same either way.
enum class PieceStyle { classic, foreign };

// Draws `piece` fitted into `box`.
void drawPiece(SDL_Renderer* r, Piece piece, const SDL_Rect& box,
               PieceStyle st = PieceStyle::classic);

// Same, but only the outline/silhouette used for small icons in the capture tray.
void drawPieceIcon(SDL_Renderer* r, Piece piece, const SDL_Rect& box);

// Helpers used for move-target markers.
void drawFillCircle(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c);
void drawStrokeCircle(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c, int width);
