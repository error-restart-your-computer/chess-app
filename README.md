# Chess

An SDL chess game with a built-in engine, chess.com-style move badges (Brilliant … Blunder), hint arrows, watch mode, and a screen overlay that finds a board anywhere on your desktop and coaches you through it. Everything works without Stockfish; it is used automatically when available.

## Features

- **Full rules**: castling, en passant, promotion picker, check/checkmate, stalemate, 50-move rule, threefold repetition — with legal-move dots, an SAN move list and a capture tray.
- **Built-in engine**: iterative-deepening alpha-beta search (quiescence, null-move pruning, PVS, reductions, aspiration windows, transposition table) over a piece-square, pawn-structure and mobility evaluation. No external program needed for play, hints or badges.
- **Move badges**: every move is judged Best / Great / Brilliant / Inaccuracy / Mistake / Blunder by an exact-score rooted search.
- **Hints**: ranked arrows for the strongest moves, in our window or over a board found on screen.
- **Watch mode**: both engines play each other while every move is graded live.
- **Screen overlay**: locks onto any chessboard visible on the desktop (a browser tab, another app, a stream) and draws the advice over it.
- **Stockfish integration**: plays, judges and answers hint queries over UCI when a build is found (`bin/stockfish`, `$STOCKFISH`, or `$PATH`).

## Installation

Needs a C++17 compiler, SDL2 and FreeType dev packages:

```bash
sudo apt install build-essential libsdl2-dev libfreetype-dev libx11-dev
make            # builds ./chess
make check      # headless tests: rules, engine, board reader
make stockfish  # optional: clone and build the bundled Stockfish 19
```

Optional and skipped gracefully when missing: a piece image set in `pieces/`, the ONNX piece-finder model in `models/`.

## Usage

```bash
./chess              # play — pick a level, click to move
./chess --watch      # engines play each other, every move graded
./chess --hints      # ranked hint arrows (also: H while running)
./chess --overlay    # coach a board found elsewhere on the screen
make elo             # rate the built-in bot against Stockfish's UCI_Elo ladder
```

Levels: Easy, Medium (1.2 s/move), Hard (3 s/move). In game: `n` new game, `u` undo, `h` hints, `o` overlay, `f` flip, `1`–`3` pick a level. `./chess --help` lists every command-line flag.

## Built-in engine's rating

Measured with `make elo`: the bot plays Stockfish 19's calibrated `UCI_Elo` ladder (8-line opening book, colours alternating, rating fitted by maximum likelihood over all games).

| level | rating |
| --- | --- |
| Easy | **1182 ± 78** |
| Medium | **2078 ± 58** |
| Hard | **2550 ± 71** |
