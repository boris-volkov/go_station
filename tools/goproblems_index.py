"""Index a goproblems.com SGF download by difficulty, easiest first.

The download is one <id>.sgf per problem, each carrying its rank in the root
node (DI[12k] / DI[3d]) and genre (GE[life and death]). The app lists the set
in difficulty order, which would mean opening all ~13,000 files every time the
puzzle browser opens, so this reads them once and writes the order out:

    <folder>/goproblems_index.tsv    id  level  genre

level is the rank as one number on the app's OGS scale: 1 = 29k ... 29 = 1k,
30 = 1d (30k is stored as 0). Rows are sorted by level, then id — older
problems first within a rank, which on goproblems means the long-standing
classics. Problems with no RIGHT line anywhere can't be solved in the app and
are left out.

The file's presence is also what tells the app this folder is the goproblems
set rather than an imported book.

Problems easier than --min-rank (default 20k) are left out of the index too —
the files stay on disk, the app just never lists them.

Usage:  python tools/goproblems_index.py [folder] [--min-rank 20k]
        (default folder: problem_sets/goproblemsSGF)
"""

import argparse
import os
import re
import sys

ROOT    = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DEFAULT = os.path.join(ROOT, "problem_sets", "goproblemsSGF")
RANK    = re.compile(r"DI\[\s*(\d+)\s*([kdp])", re.I)
GENRE   = re.compile(r"GE\[([^\]]*)\]")


def level_of(value, unit):
    unit = unit.lower()
    if unit == "k":
        return max(0, 30 - value)
    if unit == "d":
        return 29 + value
    return 38 + value   # pro


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder", nargs="?", default=DEFAULT)
    ap.add_argument("--min-rank", default="20k", help="easiest rank to keep, e.g. 20k or 1d")
    args = ap.parse_args()
    folder = args.folder
    m = re.fullmatch(r"\s*(\d+)\s*([kdp])\s*", args.min_rank, re.I)
    if not m:
        sys.exit("bad --min-rank %r (want e.g. 20k, 3d)" % args.min_rank)
    min_level = level_of(int(m.group(1)), m.group(2))

    rows, skipped, too_easy = [], [], 0
    for name in os.listdir(folder):
        stem, ext = os.path.splitext(name)
        if ext.lower() != ".sgf" or not stem.isdigit():
            continue
        with open(os.path.join(folder, name), encoding="utf-8", errors="replace") as f:
            sgf = f.read()
        rank = RANK.search(sgf)
        if not rank or "RIGHT" not in sgf:
            skipped.append(name)
            continue
        level = level_of(int(rank.group(1)), rank.group(2))
        if level < min_level:
            too_easy += 1
            continue
        genre = GENRE.search(sgf)
        genre = genre.group(1).strip().replace("\t", " ") if genre else ""
        rows.append((int(stem), level, genre))

    rows.sort(key=lambda r: (r[1], r[0]))
    out = os.path.join(folder, "goproblems_index.tsv")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("id\tlevel\tgenre\n")
        for r in rows:
            f.write("%d\t%d\t%s\n" % r)
    print("indexed %d problems -> %s" % (len(rows), os.path.normpath(out)))
    print("left out %d easier than %s" % (too_easy, args.min_rank))
    if skipped:
        print("skipped %d (no rank or no RIGHT line): %s" % (len(skipped), " ".join(sorted(skipped)[:20])))


if __name__ == "__main__":
    main()
