#!/usr/bin/env python3
"""The overlay's AI piece finder: one YOLO pass over the frame, board and pieces.

Model: yolo26n-finetuned.onnx from AndrewSpano/2d-chess-ocr on GitHub/Hugging
Face (detects the chessboard, all twelve piece types, and a few site markers),
run through onnxruntime -- no torch, no ultralytics. The .onnx is in models/;
this script only needs a Python with onnxruntime, numpy and opencv.

Protocol with the app (binary stdin/stdout, one exchange per frame):

  app -> "FRAME <w> <h> <blackAtBottom 0|1> <nskip> [<x y w h> ...]\n"
         then exactly w*h*3 bytes of packed RGB24.
  us  -> "B <x> <y> <w> <h> <conf> <fen-field0>\n"   board found
       | "N <why>\n"                                nothing usable
  app -> "QUIT\n" to exit.

field0 is FEN's piece placement, always from white's point of view: the frame's
rows are read top-down and flipped first when black sits at the bottom, which
the app knows from the colour the viewer picked in the menu. The board rect is
in frame pixels so the app can put its window and arrows over it.

Standalone: `piece_ai.py image.png` prints the same answer plus timings, for
looking at the model without the app in the way.
"""
import ast
import os
import struct
import sys
import time

import cv2
import numpy as np
import onnxruntime as ort

MODEL = os.environ.get("OV_AI_MODEL", "models/yolo26n-finetuned.onnx")
SIZE = 640           # the model's fixed input, from its metadata
CONF_BOARD = 0.30    # below this a "chessboard" box is site furniture
CONF_PIECE = 0.12    # detections under this are arrows, highlights, noise;
                     # edge pieces of some boards speak as softly as ~0.13
QUEEN_FLOOR = 0.05   # a queen opinion at or above this beats a king's on the
                     # same square: kings draw the queen class at ~0.00

# class id -> piece letter; ids from the model's own metadata (0 = chessboard,
# 13..15 = a "1" coordinate glyph and the last-move highlight, both ignored).
LETTERS = {
    "black_pawn": "p", "black_rook": "r", "black_bishop": "b",
    "black_knight": "n", "black_king": "k", "black_queen": "q",
    "white_pawn": "P", "white_rook": "R", "white_bishop": "B",
    "white_knight": "N", "white_king": "K", "white_queen": "Q",
}


def load_model(path):
    sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    # Ultralytics stores {"0": "chessboard", ...} as a Python dict repr.
    try:
        names = ast.literal_eval(sess.get_modelmeta().custom_metadata_map["names"])
    except Exception:
        names = {}
    id2letter = {}
    for cid, name in names.items():
        if name in LETTERS:
            id2letter[int(cid)] = LETTERS[name]
    if not id2letter:  # the class order above, if metadata ever goes missing
        id2letter = {i + 1: LETTERS[n] for i, n in enumerate(
            ["black_pawn", "black_rook", "black_bishop", "black_knight",
             "black_king", "black_queen", "white_pawn", "white_rook",
             "white_bishop", "white_knight", "white_king", "white_queen"])}
    board_id = next((int(c) for c, n in names.items() if n == "chessboard"), 0)
    return sess, id2letter, board_id


def letterbox(rgb, size=SIZE):
    """Fit the frame into size x size, keeping aspect, centred on grey 114."""
    h, w = rgb.shape[:2]
    s = min(size / w, size / h)
    nw, nh = int(round(w * s)), int(round(h * s))
    left = (size - nw) // 2
    top = (size - nh) // 2
    canvas = np.full((size, size, 3), 114, dtype=np.uint8)
    resized = cv2.resize(rgb, (nw, nh), interpolation=cv2.INTER_LINEAR)
    canvas[top:top + nh, left:left + nw] = resized
    return canvas, s, left, top


def infer(sess, rgb):
    """One model pass -> [(x1,y1,x2,y2,conf,cls)] in that image's own coords."""
    canvas, s, left, top = letterbox(rgb)
    blob = canvas.astype(np.float32).transpose(2, 0, 1)[None] / 255.0
    out = sess.run(None, {sess.get_inputs()[0].name: blob})[0]
    rows = []
    for r in out[0]:  # [N, 6] end-to-end: x1 y1 x2 y2 conf cls, in 640-space
        rows.append(((float(r[0]) - left) / s, (float(r[1]) - top) / s,
                     (float(r[2]) - left) / s, (float(r[3]) - top) / s,
                     float(r[4]), int(r[5])))
    return rows


def detect(sess, id2letter, board_id, rgb):
    """-> (board box in frame px or None, {square index 0..63: letter}, conf).

    Two passes, because a whole desktop squeezed into 640 px leaves a square
    about nine pixels across -- too little to tell a king from a queen, which
    is exactly how a start position turns into three kings. Pass one finds the
    board; pass two reads the pieces on a full-resolution crop of just that
    board, where a square gets about eighty pixels, the size the fixtures are
    at and where the model is dependable."""
    h, w = rgb.shape[:2]
    boards = [r for r in infer(sess, rgb) if r[5] == board_id and r[4] >= CONF_BOARD]
    if not boards:
        return None, {}, 0.0
    b = max(boards, key=lambda r: r[4])
    bx1, by1 = max(0.0, b[0]), max(0.0, b[1])
    bx2, by2 = min(float(w), b[2]), min(float(h), b[3])
    bw, bh = bx2 - bx1, by2 - by1
    if bw < SIZE / 8 or bh < SIZE / 8:  # smaller than any real board
        return None, {}, 0.0

    # The crop runs a little wide: piece boxes touch the edge of the board.
    mx, my = bw * 0.05, bh * 0.05
    cx1, cy1 = int(max(0.0, bx1 - mx)), int(max(0.0, by1 - my))
    cx2, cy2 = int(min(float(w), bx2 + mx)), int(min(float(h), by2 + my))
    crop = rgb[cy1:cy2, cx1:cx2]

    # One letter per square, decided after all the opinions are in. Nothing
    # under QUEEN_FLOOR is even carried to the vote -- a real queen's weakest
    # voice is ~0.09 and a king's phantom queen draws ~0.00, so that floor
    # already tells them apart -- while CONF_PIECE still guards every ordinary
    # square against arrows, highlights and noise.
    cell_w, cell_h = bw / 8.0, bh / 8.0
    per = {}
    for x1, y1, x2, y2, conf, cid in infer(sess, crop):
        if cid not in id2letter or conf < QUEEN_FLOOR:
            continue
        cx, cy = (x1 + x2) / 2.0 + cx1, (y1 + y2) / 2.0 + cy1
        col = int((cx - bx1) / cell_w)
        row = int((cy - by1) / cell_h)
        if not (0 <= col < 8 and 0 <= row < 8):
            continue
        per.setdefault(row * 8 + col, []).append((float(conf), id2letter[cid]))

    picked = {}
    confs = []
    for idx, dets in per.items():
        # A queen is a king with a crown, and both classes fire on the same
        # box: on some piece sets the king's opinion is even the stronger one
        # (the French queen's ball on top reads as a cross, 0.94 against
        # 0.09). So when a square draws both royal classes, the queen wins
        # the square, and counts even under CONF_PIECE -- it beat a confident
        # king, which means the square holds that colour's royal piece for
        # sure, and a queen is what fits.
        kinds = {l.lower() for _, l in dets}
        royal = "k" in kinds and "q" in kinds
        if royal:
            dets = [(c, l) for c, l in dets if l.lower() != "k"]
        strong = [(c, l) for c, l in dets if c >= (QUEEN_FLOOR if royal else CONF_PIECE)]
        if strong:
            c, letter = max(strong, key=lambda d: d[0])
            picked[idx] = letter
            confs.append(c)
    grid = [picked.get(i, "") for i in range(64)]
    conf = sum(confs) / len(confs) if confs else 0.0
    return (int(round(bx1)), int(round(by1)), int(round(bw)), int(round(bh))), grid, conf


def placement(grid, black_at_bottom):
    """grid (screen order, 0 = top-left) -> FEN field 0, white's point of view."""
    ranks = []
    for k in range(8):  # k = 0 is rank 8
        row = ""
        empty = 0
        for f in range(8):  # f = 0 is file a
            g = (7 - k) * 8 + (7 - f) if black_at_bottom else k * 8 + f
            ch = grid[g]
            if not ch:
                empty += 1
            else:
                if empty:
                    row += str(empty)
                    empty = 0
                row += ch
        if empty:
            row += str(empty)
        ranks.append(row)
    return "/".join(ranks)


def inside(rect, skips):
    x, y, w, h = rect
    cx, cy = x + w / 2.0, y + h / 2.0
    return any(sx <= cx <= sx + sw and sy <= cy <= sy + sh for sx, sy, sw, sh in skips)


def serve(sess, id2letter, board_id, inp, out):
    out.write(b"READY\n")
    out.flush()
    while True:
        line = inp.readline()
        if not line:
            return
        parts = line.split()
        if not parts:
            continue
        if parts[0] == b"QUIT":
            return
        if parts[0] != b"FRAME" or len(parts) < 4:
            out.write(b"N bad request\n")
            out.flush()
            continue
        w, h, flip = int(parts[1]), int(parts[2]), int(parts[3])
        nskip = int(parts[4]) if len(parts) > 4 else 0
        skips = []
        for i in range(nskip):
            o = 5 + i * 4
            if len(parts) >= o + 4:
                skips.append(tuple(int(v) for v in parts[o:o + 4]))
        need = w * h * 3
        buf = bytearray()
        while len(buf) < need:
            chunk = inp.read(need - len(buf))
            if not chunk:
                return
            buf += chunk
        rgb = np.frombuffer(bytes(buf), dtype=np.uint8).reshape(h, w, 3)
        t0 = time.time()
        box, grid, conf = detect(sess, id2letter, board_id, rgb)
        took = time.time() - t0
        if box is None:
            out.write(b"N no board\n")
        elif inside(box, skips):
            out.write(b"N only our own window\n")
        else:
            field0 = placement(grid, flip == 1)
            out.write(("B %d %d %d %d %.3f %s\n" % (box + (conf, field0))).encode())
        if os.environ.get("OV_AI_MS"):  # a number in the log, not one per frame
            sys.stderr.write("piece_ai: %.0f ms\n" % (took * 1000))
            sys.stderr.flush()
        out.flush()


def main():
    if len(sys.argv) > 1:  # standalone: answer for one image, no protocol
        img = cv2.imread(sys.argv[1], cv2.IMREAD_COLOR)
        if img is None:
            print("cannot read %s" % sys.argv[1], file=sys.stderr)
            return 1
        rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        sess, id2letter, board_id = load_model(MODEL)
        t0 = time.time()
        box, grid, conf = detect(sess, id2letter, board_id, rgb)
        took = (time.time() - t0) * 1000
        if box is None:
            print("N no board (%.0f ms)" % took)
            return 0
        for flip in (0, 1):
            print("flip=%d %s  (box %dx%d at %d,%d conf %.3f, %.0f ms)"
                  % (flip, placement(grid, flip == 1), box[2], box[3], box[0],
                     box[1], conf, took))
        return 0

    sess, id2letter, board_id = load_model(MODEL)
    try:
        serve(sess, id2letter, board_id, sys.stdin.buffer, sys.stdout.buffer)
    except KeyboardInterrupt:
        pass  # the app is going away; no traceback owed to anybody
    return 0


if __name__ == "__main__":
    sys.exit(main())
