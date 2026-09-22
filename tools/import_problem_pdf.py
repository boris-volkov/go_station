#!/usr/bin/env python3
"""
Import a Go problem book from a LaTeX-built PDF into a local puzzle set the
ogs_client can practise ([<BOOK NAME>] in the puzzle browser).

Why this works at all: books typeset with the `igo` LaTeX package draw their
diagrams with a *font*, not with images — one glyph per board intersection.
So the position is real text sitting in the PDF's content stream, and every
stone can be read back exactly, with no OCR and nothing to guess. The answer
key at the back is plain text too ("(1) B1 D1 D2 H1 C1"), which gives us the
solution line move for move.

Diagram alphabet (verified against rendered pages):
    @ black stone     ! white stone     + empty      * star point
    [ left edge       ) bottom edge     , bottom-left corner

Every extracted problem is replayed under full Go rules (captures, suicide,
ko-free repetition) before it is written. Anything that does not replay is
reported and skipped rather than silently producing a broken puzzle — see
--strict/--report.

Usage:
    python import_problem_pdf.py <book.pdf> [--name "Common Life & Death"]
                                            [--out problem_sets] [--report]

Requires: pip install pymupdf
"""

import argparse
import io
import os
import re
import sys

try:
    import pymupdf
except ImportError:                                   # pymupdf <1.24 module name
    try:
        import fitz as pymupdf
    except ImportError:
        sys.exit("need pymupdf:  pip install pymupdf")

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# Board columns as printed in answer keys — 'I' is skipped, as everywhere in Go.
COLS = "ABCDEFGHJKLMNOPQRST"
BOARD = 19

# Glyphs that may appear inside a diagram line. A line made only of these (and
# long enough not to be a stray word) is a diagram row.
BLACK, WHITE = "@", "!"
EMPTY_ANY    = "+*[),"                 # empty point, incl. edge/corner/star variants
DIAG_CHARS   = set(BLACK + WHITE + EMPTY_ANY)

# Local puzzle sets get synthetic OGS-style ids so the app's per-puzzle solved
# tracking (solved_puzzles.txt, keyed by id) works for them. Real OGS puzzle
# ids are in the thousands and collection ids in the hundreds, so a base up at
# 1e9 will not collide, and one million ids per book leaves plenty of room.
ID_BASE   = 1_000_000_000
ID_STRIDE = 1_000_000


# ── extraction ───────────────────────────────────────────────────────────────

def parse_pdf(path):
    """-> (problems {n: [row strings, top row first]}, answers {n: [moves]})."""
    doc = pymupdf.open(path)
    text = "".join(doc[p].get_text() for p in range(doc.page_count))

    problems, rows = {}, []
    for line in text.split("\n"):
        s = line.strip()
        if len(s) > 5 and all(c in DIAG_CHARS for c in s):
            rows.append(s)
        elif re.fullmatch(r"\d+", s) and rows:
            # A bare number right after a run of diagram rows is that diagram's
            # problem label. Page numbers never land here — they are not
            # preceded by diagram rows.
            problems[int(s)] = rows
            rows = []
        elif rows and s:
            rows = []          # some other text broke the run — not a diagram

    # Answer key: "(12) A1 A4 A3 D1 C1", possibly wrapped across lines.
    answers = {int(n): re.findall(r"[A-Z]\d+", body)
               for n, body in re.findall(r"\((\d+)\)\s*([^()]*)", text)}
    return problems, answers


def crop_geometry(rows):
    """Validate the diagram crop and return (width, height).

    Only bottom-left-anchored crops are supported, which is what `igo` books
    produce when every problem is normalised into one corner. Bail loudly on
    anything else rather than mirroring a position the wrong way round.
    """
    if not rows:
        raise ValueError("empty diagram")
    w = len(rows[0])
    if any(len(r) != w for r in rows):
        raise ValueError("ragged diagram rows")
    bottom = rows[-1]
    if not (bottom[0] in ",@!" and set(bottom[1:]) <= set(")@!")):
        raise ValueError("bottom row is not a bottom edge — unsupported crop")
    if any(r[0] not in "[@!" for r in rows[:-1]):
        raise ValueError("left column is not a left edge — unsupported crop")
    return w, len(rows)


def setup_stones(rows):
    """Diagram rows -> {(x, y): colour}, y counted from the bottom edge (y=0 is row 1)."""
    h = len(rows)
    out = {}
    for r, line in enumerate(rows):
        for x, ch in enumerate(line):
            if ch == BLACK:
                out[(x, h - 1 - r)] = 1
            elif ch == WHITE:
                out[(x, h - 1 - r)] = 0
    return out


def move_coord(token):
    """'B1' -> (x, y) with y counted from the bottom edge."""
    return COLS.index(token[0]), int(token[1:]) - 1


# ── rules (validation only) ──────────────────────────────────────────────────

class Board:
    """Just enough Go to prove a solution line is playable."""

    def __init__(self, stones, size=BOARD):
        self.size = size
        self.b = dict(stones)

    def _neighbours(self, x, y):
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < self.size and 0 <= ny < self.size:
                yield nx, ny

    def _group(self, x, y):
        colour = self.b[(x, y)]
        seen, stack, libs = {(x, y)}, [(x, y)], set()
        while stack:
            cx, cy = stack.pop()
            for n in self._neighbours(cx, cy):
                v = self.b.get(n)
                if v is None:
                    libs.add(n)
                elif v == colour and n not in seen:
                    seen.add(n)
                    stack.append(n)
        return seen, libs

    def play(self, x, y, colour):
        """-> None if legal (applied), else a reason string (board untouched)."""
        if (x, y) in self.b:
            return "point already occupied"
        self.b[(x, y)] = colour
        captured = 0
        for n in self._neighbours(x, y):
            if self.b.get(n) == 1 - colour:
                group, libs = self._group(*n)
                if not libs:
                    captured += len(group)
                    for p in group:
                        del self.b[p]
        if not captured and not self._group(x, y)[1]:
            del self.b[(x, y)]
            return "suicide"
        return None


def validate(rows, moves):
    """-> None if the whole answer line replays cleanly, else why not."""
    w, h = crop_geometry(rows)
    stones = setup_stones(rows)

    # A black group running off the crop would have liberties the book never
    # shows: on a real 19x19 it could simply escape, so the problem as printed
    # is not self-contained. (The source book clips a handful of these.)
    for (x, y), colour in stones.items():
        if colour == 1 and (x == w - 1 or y == h - 1):
            return "black group runs past the edge of the printed diagram"

    if not moves:
        return "no answer given"

    board = Board(stones)
    for i, token in enumerate(moves):
        x, y = move_coord(token)
        if x >= w or y >= h:
            return "move %d (%s) falls outside the printed diagram" % (i + 1, token)
        why = board.play(x, y, 1 if i % 2 == 0 else 0)
        if why:
            return "move %d (%s): %s" % (i + 1, token, why)
    return None


# ── SGF output ───────────────────────────────────────────────────────────────

def sgf_point(x, y, height):
    """Board (x, y from bottom) -> SGF 'cc', which counts rows from the top."""
    return chr(ord("a") + x) + chr(ord("a") + (height - 1 - y))


def sgf_escape(s):
    return s.replace("\\", "\\\\").replace("]", "\\]")


def write_sgf(path, rows, moves, name, comment, puzzle_id, collection_id, rank):
    """One problem as the app's local-puzzle SGF: setup + a single judged line.

    The crop is dropped into the bottom-left corner of a full 19x19 board,
    which is where the book draws it, so the problem sits at the edge and
    corner distances it was composed for.
    """
    stones = setup_stones(rows)
    black = "".join(sgf_point(x, y, BOARD) for (x, y), c in sorted(stones.items()) if c == 1)
    white = "".join(sgf_point(x, y, BOARD) for (x, y), c in sorted(stones.items()) if c == 0)

    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("(;GM[1]FF[4]CA[UTF-8]SZ[%d]" % BOARD)
        for coords, prop in ((black, "AB"), (white, "AW")):
            if coords:
                f.write(prop + "".join("[%s]" % coords[i:i + 2]
                                       for i in range(0, len(coords), 2)))
        f.write("PL[B]")
        f.write("GN[%s]" % sgf_escape(name))
        f.write("C[%s]" % sgf_escape(comment))
        f.write("ZI[%d]ZC[%d]" % (puzzle_id, collection_id))
        if rank:
            f.write("ZR[%d]" % rank)
        for i, token in enumerate(moves):
            x, y = move_coord(token)
            f.write(";%s[%s]" % ("B" if i % 2 == 0 else "W", sgf_point(x, y, BOARD)))
        # The book prints one line per problem, so its last move is the end of
        # the solution — that is the node the app judges as solved. It is
        # usually Black's winning move; where the book carries the line one
        # move further to show White's reply, the mark rides on that instead.
        f.write("C[RIGHT])\n")


def slugify(name):
    return re.sub(r"_+", "_", re.sub(r"[^\w]+", "_", name.strip())).strip("_").lower()


def allocate_collection_id(out_root, slug):
    """Stable id per set: reuse this book's own if re-imported, else next free slot."""
    used = set()
    if os.path.isdir(out_root):
        for entry in sorted(os.listdir(out_root)):
            meta = os.path.join(out_root, entry, "set.txt")
            if not os.path.isfile(meta):
                continue
            with io.open(meta, encoding="utf-8") as f:
                for line in f:
                    if line.startswith("id="):
                        cid = int(line[3:].strip())
                        if entry == slug:
                            return cid
                        used.add(cid)
    slot = 0
    while ID_BASE + slot * ID_STRIDE in used:
        slot += 1
    return ID_BASE + slot * ID_STRIDE


# ── driver ───────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pdf")
    ap.add_argument("--name", help="display name for the set (default: PDF title or filename)")
    ap.add_argument("--out", default="problem_sets", help="root folder for imported sets")
    ap.add_argument("--rank", type=int, default=0, help="difficulty 1-30, shown by the app")
    ap.add_argument("--report", action="store_true", help="list every skipped problem and why")
    ap.add_argument("--dry-run", action="store_true", help="parse and validate, write nothing")
    args = ap.parse_args()

    problems, answers = parse_pdf(args.pdf)
    if not problems:
        sys.exit("no igo-font diagrams found — is this a LaTeX-typeset problem book?")

    name = args.name or os.path.splitext(os.path.basename(args.pdf))[0].replace("-", " ").title()
    slug = slugify(name)

    numbers = sorted(problems)
    print("%s: %d diagrams, %d answer keys" % (args.pdf, len(problems), len(answers)))
    missing = [n for n in numbers if n not in answers]
    if missing:
        print("  %d diagrams have no answer key: %s" % (len(missing), missing[:10]))

    good, skipped = [], []
    for n in numbers:
        moves = answers.get(n, [])
        try:
            why = validate(problems[n], moves)
        except ValueError as e:
            why = str(e)
        (skipped if why else good).append((n, why) if why else (n, moves))

    print("  %d importable, %d skipped" % (len(good), len(skipped)))
    if skipped and (args.report or len(skipped) <= 10):
        for n, why in skipped:
            print("    #%-4d %s" % (n, why))
    elif skipped:
        print("    (re-run with --report to see them)")

    if args.dry_run:
        return

    out_root = args.out
    out_dir = os.path.join(out_root, slug)
    os.makedirs(out_dir, exist_ok=True)
    collection_id = allocate_collection_id(out_root, slug)

    width = max(3, len(str(numbers[-1])))
    for n, moves in good:
        # Zero-padded so the app's name-ordered list runs 1, 2, 3 … not 1, 10, 100.
        fname = "problem-%0*d.sgf" % (width, n)
        write_sgf(os.path.join(out_dir, fname), problems[n], moves,
                  name="%s %d" % (name, n),
                  comment="BLACK TO PLAY",
                  puzzle_id=collection_id + n,
                  collection_id=collection_id,
                  rank=args.rank)

    with io.open(os.path.join(out_dir, "set.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("name=%s\n" % name)
        f.write("id=%d\n" % collection_id)
        f.write("source=%s\n" % os.path.basename(args.pdf))

    print("  wrote %d problems to %s  (collection id %d)"
          % (len(good), out_dir, collection_id))


if __name__ == "__main__":
    main()
