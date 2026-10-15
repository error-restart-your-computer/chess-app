# Chess (C++ / SDL2)

A complete two-player chess game with an optional built-in AI, rendered with SDL2
and FreeType. The board shows the standard Wikipedia/Cburnett piece images (the
set Wikipedia and lichess use, shipped in `pieces/`), and falls back to its own
vector-drawn pieces when no complete set is on disk — no image assets are
required. Stockfish is used for both playing and judging moves when it is
available.

## Features

- Full chess rules: castling, en passant, promotion (with a picker), checks,
  checkmate, stalemate, the 50-move rule, threefold repetition and insufficient
  material.
- Move list in SAN with scroll, last-move highlight, legal-move dots for the
  selected piece, capture tray with material advantage.
- Built-in engine: iterative-deepening alpha-beta search with quiescence,
  null-move pruning, principal variation search, late move reductions,
  aspiration windows, a transposition table and a piece-square + pawn
  structure + mobility evaluation, three difficulty levels, running on a
  background thread so the UI never freezes (search is cancelled when you
  move or undo).
- **Stockfish for play and analysis**: a Stockfish built from source inside the
  repo is started as a UCI child process — one instance plays your games, a
  second one judges moves in the background, a third (started the first time you
  turn the hints on) ranks the candidate moves (UCI is strictly sequential, so
  they never share a process). Each level picks its own depth/movetime/thread
  count. The built-in engine is the fallback whenever no Stockfish is found.
- **chess.com-style move classification**: every move in the list is graded
  (Brilliant `!!`, Great `!`, Best `★`, Excellent `✓`, Good `•`, Inaccuracy `?!`,
  Mistake `?`, Blunder `??`, Missed Win `✗`) with matching colours, a floating
  badge over the board, and a running tally above the move list.
- **Best-move hints**: ask for the 1–5 strongest moves and they appear as
  numbered, colour-coded arrows (captures ringed) with a ranked list of SAN and
  evaluation in a card on the board. A third Stockfish instance answers with a
  MultiPV search on its own thread.
- **Board-on-screen overlay**: watch a chess board anywhere else on this screen
  (another window, a browser tab, another program) and paint the engine's best
  move over it — the position is read off the pixels themselves, by a small
  chess-detection model when it is there and by the built-in template reader
  when it is not — and the advice is drawn in a shaped, click-through X11
  window that never blocks the board underneath. You pick your colour in
  `Overlay setup`.
- Human vs AI, AI vs AI, or two humans on one board.
- **AI vs AI "watch" mode**: both engines play each other at a watchable pace,
  with a live evaluation bar, a sliding move animation, an animated thinking
  indicator and an automatic rematch a few seconds after each game.
- Click or drag pieces to move; flip the board to either side.

## Building

Requires a C++17 compiler, SDL2 and FreeType2 development packages:

```sh
# Debian/Ubuntu
sudo apt install build-essential libsdl2-dev libfreetype-dev pkg-config

make          # builds ./chess
make stockfish  # optional: build Stockfish from source into stockfish/
```

The board-on-screen overlay additionally wants the X11 headers (`libx11-dev
libxext-dev`, for reading the root window and for the shaped window it draws
in). They are optional: without them the app still builds and everything else
still works, and the overlay reports that it cannot run. Under Wayland the
capture goes through a screenshot tool (`spectacle`, `gnome-screenshot` or
ImageMagick's `import`) instead of the root window, so one of those should be
around there; the log says so when none is.

The overlay's AI piece finder is Python, not C++ — a Python 3 with
`onnxruntime`, `numpy` and `opencv-python-headless`, plus a 9.8 MB model file
in `models/`. Both are optional as well; see *AI piece finder* below.

### Stockfish

Stockfish is cloned into `stockfish/` and built from source (no root, no
packages needed):

```sh
make stockfish          # clones sf_19 if it is not there yet, then builds it
```

`make stockfish` also links the binary to `bin/stockfish`, which is the first
place the app looks. Engine discovery order:

1. `--engine <path>`
2. `$STOCKFISH`
3. `<dir of ./chess>/bin/stockfish` and `<dir of ./chess>/stockfish/src/stockfish`
4. `./bin/stockfish` and `./stockfish/src/stockfish` (relative to the working directory)
5. `$PATH`, then `/usr/games/stockfish`

`--builtin` skips all of that and uses the engine compiled into the app. The
startup line tells you which engine was picked:

```sh
chess: Stockfish 19 plays and judges (./bin/stockfish)
```

Each difficulty level tunes Stockfish separately — Easy plays at depth 6 /
400 ms / 1 thread, Medium at 14 / 1000 ms / 2, Hard at 22 / 2800 ms / 4 — while
the judge always analyses at depth 16 / 1000 ms / 2 threads and the hint
instance (started on demand) answers MultiPV queries at depth 16 / 700 ms /
1 thread / 32 MB.

### AI piece finder

The overlay can read a board with a small object-detection model: one pass
over the frame finds the chessboard and all twelve piece types, and the
placement comes back as a FEN — the template reader stays behind it as the
fallback. The model runs in a child Python process — a model's stack has no
business inside the app — one frame out over a pipe, one answer back, and a
wedged or dead child is replaced rather than obeyed.

Everything is optional. The reader looks for `tools/piece_ai.py` next to the
app (so start the app from the project directory), for its interpreter in
`venv/bin/python`, `aivenv/bin/python`, `~/aivenv/bin/python`,
`~/.venv/bin/python` and finally `python3`, and for the model in
`models/yolo26n-finetuned.onnx`. Any piece missing is reported once in the
log as `piece finder: ...`, and the template reader keeps the floor — the
overlay works without Python, without onnxruntime, and without the model.

```sh
python3 -m venv venv        # a project venv is the first interpreter tried
venv/bin/pip install onnxruntime numpy opencv-python-headless

# the model: yolo26n-finetuned.onnx, 9.8 MB, MIT — the small ONNX export of
# AndrewSpano/2d-chess-ocr on Hugging Face:
venv/bin/pip install huggingface_hub
venv/bin/python -c "from huggingface_hub import hf_hub_download as d; \
    d('AndrewSpano/2d-chess-ocr', 'yolo26n-finetuned.onnx', \
      repo_type='model', local_dir='models')"
```

The finder can be tried on its own, without the app in the way:

```sh
venv/bin/python tools/piece_ai.py tests/data/board104.bmp
# flip=0 rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR  (box 835x835 at 49,59 conf 0.877, 88 ms)
```

| Environment | What it does |
|---|---|
| `OV_AI=0` | turn the finder off; the template reader answers |
| `OV_AI=<path>` | use this script instead of `tools/piece_ai.py` |
| `OV_AI_PY=<path>` | use exactly this Python interpreter |
| `OV_AI_MODEL=<path>` | the model file (default `models/yolo26n-finetuned.onnx`) |
| `OV_AI_MS=1` | log each inference's time (`piece_ai: 88 ms`) |

The model comes from
[AndrewSpano/2d-chess-ocr](https://github.com/AndrewSpano/2d-chess-ocr), an
MIT-licensed recipe for building chess-piece detectors with almost no manual
labelling; only its exported ONNX model is used here, run through
`onnxruntime` — no torch, no ultralytics.

## Running

```sh
./chess
```

### Controls

| Input | Action |
|---|---|
| Click piece, click square | Move |
| Drag piece | Move |
| Click `New game` / `Undo` | Restart / take back |
| Click `AI: Off/White/Black/Both` | Cycle who the engine plays (Both = watch mode) |
| Click `Level:` | Cycle Easy / Medium / Hard |
| Click `View:` | Flip the board |
| Click `Copy FEN` | Copy the current position to the clipboard |
| Click `Pause` / `Resume` | Hold or release the engines (watch mode) |
| Click `Pace:` | Cycle Slow / Normal / Fast delay between engine moves |
| Click `Best moves:` | Show or hide the strongest-move arrows and their list |
| Click `Show:` | Cycle how many moves are shown (1–5) |
| Click `Overlay: On/Off` | Watch a board elsewhere on this screen |
| Click `Overlay setup` | Colour, view, copying and rescan for that overlay |
| Mouse wheel | Scroll the move list |
| `n` | New game |
| `u` or `Ctrl+Z` | Undo (also pauses watch mode) |
| `a` | Cycle AI side |
| `v` | Jump straight into AI vs AI watch mode |
| `p` | Cycle the watch pace |
| `h` | Toggle the best-move hints |
| `Shift+H` | Cycle how many moves the hints show (1–5) |
| `o` | Toggle the board-on-screen overlay |
| `Shift+O` | Open / close the overlay setup card |
| `Space` | Pause / resume the engines |
| `1` `2` `3` | Set level (Easy / Medium / Hard) |
| `f` | Flip board |
| `q` / `Esc` | Quit |

### Watching the engines play

Cycle `AI:` to **AI: Both** (or press `v`, or launch with `--watch`). Both sides
are then played by the engine: pieces glide to their targets, the evaluation bar
in the gutter eases toward the latest verdict, and the status line animates while
a search is running. `Pause` freezes the show so you can look around, `Undo`
steps back one ply (and pauses), and when a game ends a banner counts down to an
automatic rematch.

### Move classification

Every move is judged against the strongest alternative the engine can find and
given a chess.com-style badge, shown next to its SAN in the move list, as a
floating toast over the board, and in a tally above the list. `loss` below is
the centipawns the move gives up compared with the best move.

| Badge | Symbol | When |
|---|---|---|
| Brilliant | `!!` | Best move that also gives material away (≥ 1½ pawns by static exchange evaluation, and not already losing) |
| Great | `!` | Best move clearly ahead of every alternative (1½ pawns better than second best, position still normal) |
| Best | `★` | Best move |
| Excellent | `✓` | Loses ≤ 25 cp |
| Good | `•` | Loses ≤ 60 cp |
| Inaccuracy | `?!` | Loses ≤ 120 cp |
| Missed Win | `✗` | The position was winning (≥ +3.00) and the move stays ≥ 0 — a win left on the table |
| Mistake | `?` | Loses ≤ 255 cp |
| Blunder | `??` | Loses more than that |

A move that is the only legal one is never judged. Judgement runs on a single
background worker (queue of 8, stale results dropped after an undo or a new
game), or inline for `--snap --analyze`.

### Best-move hints

`Best moves: On` (or press `h`, or launch with `--hints`) asks for the strongest
moves in the position and draws them on the board:

- one arrow per move, weakest first so the pick of the position lies on top,
  each in its own colour and numbered 1..N — the same colour and number as its
  line in the list;
- a capture gets a ring on the square it lands on, like the selection marker;
- a `BEST MOVES` card in the board's lower-left corner listing the moves as
  `1. Nf3 +0.35`, with a right-aligned evaluation in White's point of view (the
  eval bar's convention) and the depth the scores came from.

`Show:` (`Shift+H`, `--top <1-5>`) chooses how many moves are shown, one to
five. Hints follow the side to move — in watch mode they keep up with the
engines — and are asked for again after every move, undo and new game.

Stockfish answers with a MultiPV search (depth 16 / 700 ms / 1 thread / 32 MB)
in a third process of its own, so the moves it plays and the badges it judges
never queue up behind it. A request overtaken by a newer position is cancelled
outright, which is why the card can be showing a shallower depth while the
engines rattle through a game. With no Stockfish the built-in engine ranks the
moves itself with a rooted search, and the card quotes its depth instead.

### How the built-in bot works

The project ships its own engine in `src/engine.cpp`, so hints, badges and a
full game all work with no Stockfish anywhere near the machine. It has two
halves.

**Evaluation — a static score of the position.** Material on the classic scale
(pawn 100, knight 320, bishop 330, rook 500, queen 900) plus a piece-square
table per piece saying where that piece likes to stand: knights want the
centre, rooks like the seventh rank, the king is dragged to safety by the
midgame table and forward to the fight by the endgame one — the endgame table
takes over once at most 2500 centipawns of non-pawn material are left. The
bishop pair adds 30. On top of that the pawn skeleton is read file by file:

- **doubled** pawns cost 14 each beyond the first on their file, **isolated**
  pawns (no friendly pawn on a neighbouring file) 14;
- **passed** pawns — no enemy pawn ahead on their file or the neighbours —
  pay 5/10/20/40/70/110 by how far they have advanced, which is what makes
  the bot race a passer instead of wandering in the endgame;
- a **rook on an open file** (no pawn of its own in the way) adds 25, on a
  half-open file 12.

- **Mobility**: the squares each knight/bishop/rook/queen can actually step
  or slide to — 4 per knight square, 5 per bishop, 2 per rook, 1 per queen —
  so pieces that came out to play outvalue pieces locked behind their own
  pawns. The term is colour-symmetric in the starting position (still a net
  zero there).

The total is returned from the side to move's point of view, ready for
negamax. (None of the structural terms fire in the starting position, so it
still evaluates to exactly 0 — a claim the engine tests lean on.)

**Search — alpha-beta with a clock.** Depth-first negamax with alpha-beta
pruning, plus every standard way of visiting far fewer nodes per point of
depth:

- **Quiescence.** At the depth limit the bot does not stop on the raw static
  score; it plays on through captures and promotions (up to 6 plies) until the
  position is quiet, so it never crashes a piece into a recapture because the
  clock ran out at exactly the wrong ply. **In check there is no standing
  pat**: every move is in scope, not only captures (up to 12 plies), so a
  checking chain is always played out instead of ending on a number the
  checked side never agreed to. A capture that could not raise the score even
  if it went through for free is skipped instead (**delta pruning**).
- **Move ordering.** Moves are tried most-likely-first: the transposition
  table's remembered move for the position, then big captures (`10 × victim −
  attacker`, so the queen-takes-pawn sacrifice and its recapture lead), then
  the two **killer moves** — quiet moves that already cut the search off at
  this ply — then every quiet move by a **history** count of from-to successes
  accumulated during this search.
- **Null-move pruning.** When the static score already says the position is
  at least beta, the bot hands the turn over (a null move — nothing moves,
  only the side flips, implemented in `Position::doNullMove`) and searches a
  couple of plies less from the other side. If passing still leaves the
  position fine, there was no cutoff to fear. It never tries this in check,
  for a mate score, or in a bare endgame without a piece to move — that is
  exactly where passing loses.
- **Null windows and reductions.** Everything after the first move is first
  asked the narrow question "is this better than alpha?" — a null-window
  search that costs a fraction of a full one — and only searched in full when
  the answer could be yes (principal variation search). On top of that, a
  quiet move far down the well-ordered list starts one ply lower (two past
  move six at depth six). One re-search covers both: the instant a reduced or
  narrowed result beats alpha, the move is looked at again in full depth and
  full window. Captures, moves giving check and exact mode keep full windows
  throughout.
- **Check extensions.** A node in check gains a ply instead of stopping, so a
  forcing line is read until it runs dry (repetition, the 50-move rule and a
  128-ply ceiling still bound it).
- **A transposition table.** One table per thread (no locking, each thread
  drives its own engine), keyed by the Zobrist key the position already
  carries, storing the best move plus a lower bound or exact score — never a
  mate score, whose distance is ply-relative. Fail-lows store nothing: there
  is no sound value to keep.
- **Iterative deepening.** Depth 1 first — so a legal move exists even if the
  deadline hits immediately — then deeper and deeper until the level's depth
  ceiling is done or the wall-clock deadline expires (checked every 2048
  nodes), re-rooting each pass by the previous pass's scores. From depth 3
  each pass runs inside an **aspiration window**: ±50 centipawns around the
  previous score, widening through ±150 and ±450 to the full range when the
  score slips out, with the depth re-searched whenever the window blows. A
  typical pass then only pays for the moves that matter. Finding a forced
  mate ends the search early.
- **Draws are scored inside the search** (repetition, 50-move rule), which is
  what stops it from shuffling into a repetition it could have avoided.
- **Noise** shakes every evaluation by ±N centipawns and the move is then
  picked uniformly among all moves within N of the best — that randomness is
  the handicap: Easy plays depth 1 with 60 cp of noise, while Medium (up to
  8, 1.2 s) and Hard (up to 14, 3 s) are clock-bound — pruning makes the
  budget, not the ceiling, the real limit. The table lives in
  `src/levels.h`, shared with the GUI and the Elo harness below.

Two rooted uses fall out of the same search, and they run in a stricter mode:
**exact mode** turns *off* null-move pruning, reductions, transposition
bounds and quiescence delta pruning — anything that may bend a score —
because the badge and the hint promise exact numbers. `rankMoves` scores *every* root move with a wide-open
window (so each score is exact rather than a bound) and sorts them — that is
what draws the hint arrows with no outside engine — and `assess` searches the
played move first (with the window still wide it cannot fail low, so its
score is exact) and compares it against the rest to place the badge, with a
static-exchange evaluation deciding whether the move was a real sacrifice
worth a `!!`.

### Board on screen (overlay)

With the app open and the overlay enabled, the watcher looks at the desktop
itself for a chess board that is not ours — a browser tab, another program, a
friend's stream — and draws the engine's advice straight over it.

`Overlay: On` (or press `o`, or launch with `./chess --overlay`) starts it. One
background thread searches the screen for the board, locks onto the rectangle
it found, and re-checks that area about every second — the search is the
expensive half of a look, so it comes back only when the lock stops answering
(three reads in a row that are not a chess position), when our own window is
dragged across the board, when `Overlay setup` asks for a rescan, and once
every quarter minute so a board that drifted still follows. Each look:

1. **captures the desktop** — on X11 the root window; on a Wayland session a
   screenshot tool first (`spectacle`, `gnome-screenshot` or ImageMagick's
   `import`), because a Wayland session's X11 root window reads just fine and
   still shows only Xwayland's own clients: the window being watched is a
   native one and never appears in it. A failed path is never retried;
2. **finds the board** — two eyes on it: the model, which knows a chessboard
   when it sees one from the same pass that will read the pieces, and the
   8×8 two-colour grid search at any size and position, scored by how cleanly
   64 samples fall into exactly two colours. The grid's rectangle is snapped
   to the squares and is what the overlay draws over, so it stands when both
   mean the same board; the model's box stands in when the search finds
   nothing. Our own board is refused by the rectangle the marker stripe puts
   around our window — and, when the stripe itself cannot be seen, by our own
   colours — and any candidate whose squares sit behind our own window is
   reported as hidden instead of half read;
3. **reads the 64 squares** — the model's answer comes first: the placement
   from that same pass, its confidence the card's `conf`. When the finder is
   missing, refused (fewer than two kings, say) or was looking at another
   board, the template reader answers instead — and when both saw *this*
   board, the log keeps the model's FEN beside the grid reader's answer,
   because a model that miscounts kings and a search that liked this
   checkerboard are two independent opinions. The template reader masks each
   square against its own background colour (only pixels clearly brighter
   than the square, or far darker, count: what falls in between is the shadow
   the piece stands in, and the board keeps that), with only the largest
   connected blob of what is left kept (a piece is one shape: the coordinate
   letter a site paints into an edge square's corner is a second, far smaller
   one, and it used to drag whole back ranks over to the wrong piece), then
   matched against the pieces rendered at that size on six measures — pixel
   overlap, tried both ways round so a knight drawn facing the other way
   still matches; the row-by-row width profile; the colour it is painted in;
   and the geometry no art style can change: how big a box it fills, how thin
   its top is, how far it leans. The pieces are the twelve, drawn ones — what
   our own window happens to be showing is decoration and never enters into
   this — plus a second
   drawing of queen and king for the sets that crown those two the widest
   part of the piece — whichever drawing fits the square better names the
   type. Arrow pixels are excluded, pawns on the back rank discarded, and
   fewer than two kings taken as "not a chess position". Side to move and en
   passant come from the change that just happened, and a new board is
   believed only after it has been seen twice in a row (so a piece still
   sliding into place is not read);
4. **asks the engine** — Stockfish on its own process (started on first use),
   the built-in engine when there is none — and publishes the best move plus up
   to four candidates. The advice is always **your** colour, the one picked in
   `Overlay setup`: the watcher can know the board says it is the opponent's
   move and still answer with your moves, because "what do I play" is the
   question being asked. The side to move stays as read for everything else —
   it is what decides which pawn push is offering an en passant capture.

What comes back is drawn in a separate X11 window laid over the board: an
arrow, and a chip with `Nf6 +0.32` and the candidate moves behind it. The
window is cut to the alpha of what is drawn (nothing undrawn ever covers the
board) and its input region is empty, so clicks keep reaching the board
underneath. A four-colour stripe in our own window's bottom margin marks it in
every screenshot, which is how the watcher avoids reading its own overlay — or
its own board — as the one being watched.

`Overlay setup` (or `Shift+O`) opens a card over the board:

| Item | What it does |
|---|---|
| `Watch this screen` / `Stop watching` | Same as the `Overlay:` button |
| `I play White` / `I play Black` | Which colour you are: it decides which way the board is read, and whose move is searched first |
| `Board view: ...` | Turn the watched board the other way up |
| `Look again now` | Forget everything read so far and start over |
| `Copy the board into this window: On/Off` | Copy each accepted position into our own window — the watched game, redrawn with our board, our pieces and (with `Best moves:` on) our arrows. The engines stay out of it while this is on: the real moves are made in the other program |
| `Close` | Close the card (a click outside it does the same) |

The card's status lines say where it got to: the position it read as a FEN, how
sure the piece reading was, which engine answered, and whether some squares are
hidden behind our own window. The panel's status line shows the same while the
overlay runs, and the terminal gets each accepted read as one line, saying which
reader answered:

```text
chess: overlay: read 2450,905 cell 56 grid 0.94 conf 0.93 [ai]:
    rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1
```

`[ai]` is the model's answer, `[grid]` the template reader's, and `conf` how
sure the reading was.

When the watcher gives up, the terminal gets the numbers behind it — how well
the best grid scored against the 0.62 it needs, how many candidates survived
the coarse pass, how many of those our own window stood on, where that window
actually sits, and whether the marker stripe was in the picture at all — plus,
when the finder had its own say about the frame, `| ai: ...` behind it:

```text
chess: overlay: marker at 2325,649 1052x736
chess: overlay: no board: best grid scored 0.41, 0.62 needed - 12 candidates,
    3 of them under our window (marker found) | our window 2325,649 1052x736
    | ai: no board
chess: overlay: not a position: 0 kings, conf 0.96, board 492,128 cell 104
    grid 0.91, our window 2325,649 1052x736: <the FEN it thought it saw>
```

The last one means a board *was* found and its squares were read; the FEN says
which pieces came out wrong, and behind it one line per rank says how each
square scored, so a narrow miss and an unreadable square can be told apart
(ranks with nothing on them print no line at all). These lines belong to the
template reader — the model answers in its single `conf` — and it prints them
whenever the read was refused, or whenever asked:

```text
chess: overlay: squares rank 1: a1 R81~Q74~B59~N55 b1 N80~Q65~R58 c1 Q77~K76
    d1 N76~Q66 e1 N73~Q62~K58 f1 K76~Q76~B61 g1 N80~Q64 h1 R78~Q74~B59
chess: overlay: squares rank 2: a2 P81~B78~R61^4 b2 P83~B79~R60 ...
chess: overlay: squares rank 3: d3 ?4
```

`e1 N73~Q62~K58` reads: e1 came out a knight at 0.73, a queen was the best of
*its* kind at 0.62, and the king scored 0.58. Every piece type is listed, in
order, because that is the number a misread turns on — a king losing by one
point is a tuning problem, losing by thirty means something else broke. Types
scoring under 0.35 are left out: they were never in the running, and naming
them would only say that they were not. A trailing `^7` means 7% of that
square's pixels left with a smaller separate blob, which is what a coordinate
letter in the corner of an edge square looks like once it lands inside the
square with the piece; `d3 ?4` says 4% of the square had something on it and
none of it was a piece. `grid 0.91` in the line above says how sure the
*detector* was that this is a checkerboard at all — separate from how sure the
piece reading was.

The squares that were nearly wrong then come apart all the way, into the six
measurements their scores are built from: every square the winner beat the next
type by fewer than five points — a misread lives in a narrow win — plus the
square nobody scored well, closest first, capped so a board where everything is
close does not print a novel:

```text
chess: overlay: terms e1 o80x86:K91[i89 p93 c94 s96 t82 l86 b78x88]
    Q87[i82 p92 c94 s84 t78 l87 b68x81] B85[i76 p87 c94 s78 t93 l90 b60x82]
```

`i` is the pixel overlap, `p` the row by row width profile, `c` the colour the
piece is painted in, `s` how big a box the shape fills, `t` how wide its top
is and `l` how lopsided it is — each on the same 0..1 scale as the total,
which is `0.30i + 0.17p + 0.25c + 0.16s + 0.07t + 0.05l`. The types are the
winner first and the nearest rivals behind it, types under 0.35 left out;
`o80x86` is the box measured off the square itself, against that template's
own `b78x88` — the two `s` compares before any weighting touched them. A
square that reads wrong can then be pinned on one weight instead of on chess
in general.

Numbers only go so far: `OV_DUMP=<path>` makes the watcher write out what it is
working from, so the log can be checked against the picture it came from:

```sh
OV_DUMP=/tmp/opencode/cap.png ./chess --overlay   # then look at cap.png
```

What lands in the file follows what the search found: a board, cropped out at
full size — a piece set is argued about at the size the pieces are drawn — or
the whole frame when there was none. Once a board frame has been written,
frames without a board stop replacing it, because the last capture of a session
is usually the one where the board has already moved away. The format follows
the name: `.png` for a picture, anything else for the BMP SDL writes with no
help from anyone.

`OV_DEBUG=1` asks for the squares and the terms even when the read was
*accepted*: a refusal explains itself, and the confident mistake is exactly the
one that does not. (Both lines are the template reader's; a read the model
answered prints its one `read ... [ai]` line instead, and every occupied square
gets the treatment rather than only the narrow ones.)

```sh
OV_DEBUG=1 OV_DUMP=/tmp/opencode/cap.png ./chess --overlay
```

Worth knowing: the watcher starts from the colour you picked and corrects the
side to move at the first move it sees; castling rights are inferred from a
king and rook still at home; and nothing is ever captured or analysed until you
turn the overlay on.

### Command-line options

```sh
./chess --fen "<fen>"          # start from a position
./chess --moves "e2e4 e7e5"    # apply coordinate moves first
./chess --flip                 # view from Black's side
./chess --watch                # start in AI vs AI mode
./chess --level 2              # pick the engine level (0/1/2)
./chess --engine ./bin/stockfish  # use a specific UCI engine
./chess --builtin              # ignore Stockfish, use the built-in engine
./chess --hints                # start with the best-move hints showing
./chess --top 5                # how many moves the hints show (1-5, default 3)
./chess --overlay              # watch a board elsewhere on this screen
./chess --perft 5              # move-generation benchmark, then exit
./chess --snap out.bmp         # render one frame and exit (headless-friendly)
./chess --snap out.bmp --analyze  # ...and grade the --moves played first
./chess --snap out.bmp --select e2   # highlight a square's legal moves
./chess --snap out.bmp --promo      # open the promotion picker in the snapshot
```

## Testing

```sh
make check
```

Runs the headless suite (`tests/engine_test.cpp`: SAN generation and
disambiguation, status rules, search sanity, mate-in-1, cancellation, the
best-move ranking behind the hints — every legal move scored and sorted, mating
move first — and move classification: Brilliant, Blunder, Missed Win, Best,
even-trade-not-Brilliant, forced-move-not-judged), then `tests/screen_test.cpp`
(the overlay's eye, on a desktop rendered in memory: the board found at two
sizes and both ways up, the position read back field by field, the wrong view
reading as a different position, our own colours refused, a board behind a
window reported as hidden, the marker stripe found and a random stripe not,
the arrow ignored while reading and noticed when it is not, en passant worked
out from the position before it, a gradient not mistaken for a board, and two
saved screenshots of real boards in two different piece sets —
`tests/data/board104.bmp`, a green set with the site's move arrows lying over
the empty middle of a starting position, and `tests/data/cap2.bmp`, a purple
one two moves in — each read back to the exact position it was taken from),
then perft comparisons against known-good values from the Chess Programming
Wiki (start position and Kiwipete, depth 4). The AI piece finder runs outside
that suite, through its sidecar: `python tools/piece_ai.py
tests/data/board104.bmp` answers with the fixture's placement on its own.

A longer, fully headless AI vs AI smoke test (also exercises the watch-mode
thread, pacing, Stockfish and auto-rematch):

```sh
SDL_VIDEODRIVER=dummy ./chess --watch --plies 40 --level 0
```

With the hints on it additionally starts the third engine, cancels a search on
every move and tears all three down at the end — a good run reaches three
`stockfish` processes at once and leaves none behind:

```sh
SDL_VIDEODRIVER=dummy ./chess --watch --plies 40 --hints --top 5
```

### Measuring the bot's Elo

`make elo` builds and runs `tests/elo_test.cpp`, which rates the built-in bot
against Stockfish's own strength ladder. With `UCI_LimitStrength` on, our
in-tree Stockfish accepts a `UCI_Elo` anywhere in 1320..3190 and then
deliberately plays a weaker root move — picked by a formula the Stockfish
developers calibrated against games between Stockfish and the Stash engine
(CCRL-blitz scale). Those fixed ratings are the anchors:

- every (level, anchor) pair plays 16 games (default) from an eight-line
  opening book with colours alternating — balanced 8/8 per cell, and the
  second pass through the book flips colours so the same opening is seen
  from both sides;
- Stockfish always gets a search depth past its anchor's skill pick (the pick
  happens at exactly depth `1 + int(level)`), so a weak anchor really plays
  the weakness it claims instead of being cut off first;
- one rating per level is fitted over all of that level's game points by
  maximum likelihood under the standard logistic curve
  `P(point | anchor) = 1 / (1 + 10^((anchor - E) / 400))`, draws worth half a
  point (the likelihood is concave in `E`, so a bisection on its derivative
  finds the unique optimum); the standard error comes from the Fisher
  information at the fit.

Results of `build/elo_test --games 16 --workers 6` (240 games, 40.6 minutes on
6 workers). Cells are the bot's W-D-L and points% (win 1, draw 1/2); the last
column pools all five anchors of that row into one fitted rating:

| level | vs 1320 | vs 1600 | vs 1900 | vs 2200 | vs 2500 | fitted rating |
|---|---|---|---|---|---|---|
| Easy | 4-1-11 · 28.1% | 1-3-12 · 15.6% | 0-2-14 · 6.2% | 0-1-15 · 3.1% | 0-0-16 · 0.0% | **1182 ± 78** (95% CI 1029..1335) |
| Medium | 16-0-0 · 100% | 10-5-1 · 78.1% | 10-4-2 · 75.0% | 2-5-9 · 28.1% | 1-8-7 · 31.2% | **2078 ± 58** (95% CI 1964..2192) |
| Hard | 16-0-0 · 100% | 16-0-0 · 100% | 16-0-0 · 100% | 10-4-2 · 75.0% | 7-6-3 · 62.5% | **2550 ± 71** (95% CI 2411..2688) |

The same harness, before the engine upgrade, measured **994 ± 112 / 1586 ± 59 /
2185 ± 59** at the same three levels (that measurement was itself replicated
within a standard error) — so the transposition table, null-move pruning,
late move reductions, check extensions and pawn-structure terms bought
**+188 / +492 / +365 Elo** with no time budget increased: the same 0.5 s /
1.2 s / 3 s per move, spent deeper. Runs take longer than the old ones
because Medium and Hard now use their whole budgets instead of stopping early.
A second independent run of the upgraded engine (its first 211 games before an
interrupted session) fitted 1140 ± 84 / 2063 ± 61 / 2479 ± 105 — every level
within one standard error of the table above, the same run-to-run spread seen
before the upgrade.

Reading it: Easy trades punches with the weakest anchor Stockfish can be asked
for (4 wins in 16) but still loses to everything above, so its 1182 is barely
above the ladder's 1320 floor and mostly extrapolated downward. Medium sweeps
1320 and scores ~75–78% between 1600 and 1900 — its 50% point lies between the
1900 and 2200 anchors. Hard goes 100% through 1900 and still takes 62.5% off
the 2500 anchor, with real wins among the draws, so its 2550 is an
interpolation near the top of the calibrated ladder.

Knobs: `--games`, `--anchors` (1320..3190), `--workers`, `--movetime`,
`--max-plies` (draw cap for endless shuffles), `--sf`. The harness needs
`bin/stockfish` (`make stockfish`) and prints one line per game as it finishes
plus the result tables at the end.

## Project layout

```
src/common.h      square/piece helpers, Zobrist seeds, constants
src/position.*    board representation, FEN, move generation, make/unmake, SAN, status
src/engine.*      evaluation (piece-square tables, pawn structure, mobility), negamax
                  with null-move pruning, reductions and a transposition table, time control,
                  sacrifice detection, the move classifier and the built-in move ranking
src/levels.h      the Easy/Medium/Hard table: shared by the GUI, the watch mode and
                  the Elo harness so everyone plays the same settings
src/uci.*         UCI child process: fork/exec Stockfish, MultiPV assessment and queries
src/pieces.*      piece artwork as polygons, drawn with SDL primitives (what the
                  screen reader matches against, and the fallback for the window)
src/art.*         piece images from disk for our own window (stb_image), all or
                  nothing: a complete set or none
src/text.*        FreeType text rendering (SDL2_ttf not required)
src/screen.*      desktop capture, finding a chess board on it and reading its 64
                  squares back into a FEN (used by the overlay, tested headlessly)
src/aiwatch.*     the AI piece finder's sidecar: one frame out over a pipe to the
                  Python reader, one answer back, deadlines everywhere
src/xoverlay.*    the shaped, click-through X11 window the overlay draws in
src/main.cpp      window, input, layout, snapshots, AI thread, badges, hint worker,
                  the arrow/card drawing and the board-on-screen overlay
tools/            piece_ai.py — the model reader the sidecar drives (onnxruntime)
models/           yolo26n-finetuned.onnx — the optional piece finder model (MIT,
                  from AndrewSpano/2d-chess-ocr)
pieces/           the twelve Wikipedia/Cburnett piece PNGs (192px, GPLv2+, Colin
                  M. Burnett — see pieces/CREDIT). Any same-named set takes their
                  place, or $PIECES points at a directory holding one; without a
                  complete set the drawn pieces are used and none of this matters
third_party/      stb_image.h (public domain; PNG decoding for the piece images)
tests/            engine_test (rules, search, classification), screen_test (board
                  detection and piece reading, on in-memory desktops and two saved
                  screenshots), elo_test (the built-in bot against Stockfish's
                  UCI_Elo ladder), debug/rootscore helpers
stockfish/        Stockfish clone, built in-tree by `make stockfish`
```
