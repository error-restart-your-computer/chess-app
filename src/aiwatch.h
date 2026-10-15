#pragma once
// aiwatch.h -- the AI piece finder: a YOLO chess model in a sidecar process
//
// tools/piece_ai.py finds the board and reads its pieces in one model pass
// (yolo26n-finetuned.onnx from AndrewSpano/2d-chess-ocr on GitHub, in models/,
// run through onnxruntime -- no torch). Script, interpreter and model are all
// optional: when one is missing this says so in the log and the template
// reader keeps the floor, so nothing here can stop the overlay from working.
//
// The sidecar runs in a child process because a model's Python stack has no
// business inside the app: one frame goes out over a pipe, one answer comes
// back, and a wedged or dead child is replaced rather than obeyed.
#include "screen.h"

#include <string>
#include <vector>

// Ask about one frame. True = the model vouched for a board and sent a piece
// placement; br (frame coordinates, score = the board detection's confidence)
// and field0 (FEN placement, white's point of view) are then filled. False =
// no sidecar, or this frame had nothing; *why says which, and is "" when
// OV_AI=0 turned the finder off on purpose (that choice owes no complaint).
bool aiAskBoard(const ScreenImage& img, const std::vector<SDL_Rect>& skip, bool blackAtBottom,
                BoardRect& br, std::string& field0, float& conf, std::string* why);

// The model's placement dressed as a ReadResult for `side` to move: board
// squares, king count, castling rights derived from where the back-rank pieces
// stand (a screenshot cannot tell "never moved" from "moved and came back" --
// the same limit the template reader lives under). ok only when the placement
// parsed and has two kings: a refusal the caller can fall back from.
void aiToReadResult(const std::string& field0, float conf, Color side, ReadResult& rd);

// Stop the sidecar. Safe to call when none was ever started.
void aiShutdown();
