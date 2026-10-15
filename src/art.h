#pragma once

// art.h -- piece images from disk, for our own window
//
// The pieces in pieces.cpp are vector art: recognizable, and precise enough to
// match a screen against, but not the pieces people expect a chess program to
// show them. This loads a standard set from files instead (the Wikipedia /
// Cburnett shapes by default: pieces/wK.png ... pieces/bP.png) and hands them
// to the renderer.
//
// Nothing here is required: with no complete set on disk the caller keeps
// drawing the vector pieces and the app looks after itself, same as always.
// Only the GUI uses this -- the screen reader still builds its templates from
// the drawn pieces, so what the reader matches against never depends on which
// files happen to be lying around.
#include "common.h"

#include <SDL.h>

// Loads all twelve pieces from <dir> ("" = $PIECES, else "pieces"). True when
// a complete set was found and turned into textures; a partial set is treated
// as none at all -- half the pieces in one style and half in another is worse
// than all of them drawn.
bool artLoad(SDL_Renderer* r, const char* dir);

// True while a complete set is loaded.
bool artReady();

// Draws `piece` fitted into `box`. Does nothing when nothing was loaded.
void artDraw(SDL_Renderer* r, Piece piece, const SDL_Rect& box);

// Frees the textures. Safe to call when nothing was ever loaded.
void artShutdown();
