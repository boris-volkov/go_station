#!/usr/bin/env python3
"""
Annotate an imported problem set with KataGo: flag book moves that give away
points, and grow refutation branches so a wrong guess gets punished instead of
dropping the solver into free play.

What it does NOT do is re-solve the problems. A printed problem carries an
objective ("black to live") that the diagram alone doesn't encode, and an
engine left to its own devices optimises score instead — on these cropped
corner diagrams that means running the group out through the gap the book
never had to draw, which is best play but not the problem. So the book's line
stays the spine and KataGo only annotates and branches off it.

Three things make the engine usable on tsumego at all:

  * Confine it. Search is restricted to the book's own stones and answer
    points dilated by one ring — typically ~50 points. Confinement goes
    through avoidMoves on both players, because allowMoves takes only a
    single entry and so can only ever constrain one colour.
  * Ignore winrate. It sits pinned at 0.000 on an otherwise empty board.
    Only scoreLead differences and group ownership carry signal.
  * Judge with a margin. Per-move scoreLead out of one search is noisy, so a
    move is only called loose past --loss-threshold, and flagged cases get
    re-checked by playing the candidates out.

Usage:
    python katago_annotate.py problem_sets/common_life_death [--limit 20]
                              [--jobs 4] [--visits 2500] [--dry-run]

Requires KataGo; paths are read from apps/ogs_client/config.ini by default.
"""

import argparse
import io
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from collections import deque

COLS = "ABCDEFGHJKLMNOPQRST"
SIZE = 19

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


# ── coordinates ──────────────────────────────────────────────────────────────

def sgf_to_xy(s):
    """SGF 'cc' (row counted from the top) -> (x, y counted from the bottom)."""
    return ord(s[0]) - 97, SIZE - 1 - (ord(s[1]) - 97)


def xy_to_sgf(x, y):
    return chr(97 + x) + chr(97 + SIZE - 1 - y)


def xy_to_gtp(x, y):
    return COLS[x] + str(y + 1)


def gtp_to_xy(v):
    return COLS.index(v[0]), int(v[1:]) - 1


def sgf_to_gtp(s):
    return xy_to_gtp(*sgf_to_xy(s))


def gtp_to_sgf(v):
    return xy_to_sgf(*gtp_to_xy(v))


# ── SGF ──────────────────────────────────────────────────────────────────────

class Node:
    __slots__ = ("color", "point", "comment", "correct", "children")

    def __init__(self, color=None, point=None, comment=None, correct=False):
        self.color = color          # 'B' / 'W'
        self.point = point          # SGF 'cc'
        self.comment = comment
        self.correct = correct
        self.children = []


def _read_value(s, i):
    """i points at '['. Returns (value, index past ']')."""
    i += 1
    out = []
    while i < len(s) and s[i] != "]":
        if s[i] == "\\":
            i += 1
            if i < len(s):
                out.append(s[i])
                i += 1
        else:
            out.append(s[i])
            i += 1
    return "".join(out), i + 1


def _read_props(s, i):
    props = {}
    while i < len(s):
        while i < len(s) and s[i].isspace():
            i += 1
        if i >= len(s) or not s[i].isupper():
            break
        j = i
        while j < len(s) and s[j].isalpha():
            j += 1
        ident, i = s[i:j], j
        vals = []
        while i < len(s) and s[i] == "[":
            v, i = _read_value(s, i)
            vals.append(v)
            while i < len(s) and s[i].isspace():
                i += 1
        props[ident] = vals
    return props, i


def _read_tree(s, i):
    """i points past '('. Returns (node_chain, subtrees, index past ')')."""
    chain, subs = [], []
    while i < len(s):
        c = s[i]
        if c == ";":
            props, i = _read_props(s, i + 1)
            chain.append(props)
        elif c == "(":
            n, sb, i = _read_tree(s, i + 1)
            subs.append((n, sb))
        elif c == ")":
            return chain, subs, i + 1
        else:
            i += 1
    return chain, subs, i


def load_problem(path):
    """-> (root props, main-line [(color, sgf point)]).

    Only the main line is read, so re-running on an already-annotated file
    rebuilds from the book's spine rather than compounding old branches.
    """
    text = io.open(path, encoding="utf-8").read()
    i = text.index("(")
    chain, subs, _ = _read_tree(text, i + 1)
    root = chain[0] if chain else {}
    line = []
    for props in chain[1:]:
        for c in ("B", "W"):
            if c in props:
                line.append((c, props[c][0]))
    while subs:                       # first subtree = main line
        chain, subs = subs[0]
        for props in chain:
            for c in ("B", "W"):
                if c in props:
                    line.append((c, props[c][0]))
    return root, line


def esc(s):
    return s.replace("\\", "\\\\").replace("]", "\\]")


def _ser(node):
    """Serialise a node's children, matching the app's own writer: a lone
    child chains inline, siblings each get their own parenthesised branch."""
    kids = node.children
    if not kids:
        return ""
    def one(k):
        out = ";%s[%s]" % (k.color, k.point)
        if k.correct:
            out += "C[RIGHT]"
        elif k.comment:
            out += "C[%s]" % esc(k.comment)
        return out + _ser(k)
    if len(kids) == 1:
        return one(kids[0])
    return "".join("(" + one(k) + ")" for k in kids)


def save_problem(path, root_props, tree):
    keep = ["GM", "FF", "CA", "SZ", "AB", "AW", "PL", "GN", "C", "DT",
            "ZI", "ZC", "ZR"]
    out = ["(;"]
    for k in keep:
        if k in root_props:
            out.append(k + "".join("[%s]" % esc(v) for v in root_props[k]))
    out.append(_ser(tree))
    out.append(")\n")
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(out))


# ── KataGo ───────────────────────────────────────────────────────────────────

class Kata:
    """Analysis engine with many queries in flight, routed back by id.

    Serialising queries wastes most of the GPU: the engine is built to work on
    several at once, which is the difference between a day-long batch and a
    couple of hours.
    """

    def __init__(self, exe, model, cfg, threads=6, verbose=False):
        self.p = subprocess.Popen(
            [exe, "analysis", "-model", model, "-config", cfg,
             "-override-config", "numAnalysisThreads=%d" % threads],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1)
        self.q = queue.Queue()
        self.verbose = verbose
        self.n = 0
        threading.Thread(target=self._out, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()

    def _out(self):
        for line in self.p.stdout:
            try:
                self.q.put(json.loads(line))
            except ValueError:
                pass
        self.q.put(None)

    def _err(self):
        for line in self.p.stderr:
            if self.verbose or re.search(r"error|uncaught", line, re.I):
                print("  [katago] " + line.rstrip(), file=sys.stderr, flush=True)

    def send(self, q):
        self.p.stdin.write(json.dumps(q) + "\n")
        self.p.stdin.flush()

    def recv(self, timeout=1800):
        r = self.q.get(timeout=timeout)
        if r is None:
            raise RuntimeError("katago exited")
        return r

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=15)
        except Exception:
            self.p.kill()


class Board:
    """Board state with captures — needed to know what the fight actually is."""

    def __init__(self, black=(), white=()):
        self.b = {}
        for v in black:
            self.b[gtp_to_xy(v)] = 1
        for v in white:
            self.b[gtp_to_xy(v)] = 0

    def nbrs(self, x, y):
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if 0 <= x + dx < SIZE and 0 <= y + dy < SIZE:
                yield x + dx, y + dy

    def group(self, p):
        col = self.b[p]
        seen, stack, libs = {p}, [p], set()
        while stack:
            c = stack.pop()
            for n in self.nbrs(*c):
                v = self.b.get(n)
                if v is None:
                    libs.add(n)
                elif v == col and n not in seen:
                    seen.add(n)
                    stack.append(n)
        return seen, libs

    def play(self, v, color):
        p = gtp_to_xy(v)
        if p in self.b:
            return
        self.b[p] = 1 if color == "B" else 0
        for n in self.nbrs(*p):
            if self.b.get(n) == (0 if color == "B" else 1):
                g, l = self.group(n)
                if not l:
                    for q in g:
                        del self.b[q]
        if p in self.b and not self.group(p)[1]:
            del self.b[p]

    def fight_points(self, anchor):
        """The points a solver would actually consider.

        The liberties of the problem's main black group, plus the liberties of
        the white groups pressing on it. That is where a life-and-death fight
        lives; everything else inside a dilated bounding box is open board,
        which is how an engine ends up "refuting" a move by playing a big
        point eight lines away.
        """
        blacks = [p for p, c in self.b.items() if c == 1 and p in anchor]
        if not blacks:
            return set()
        seen, groups = set(), []
        for p in blacks:
            if p in seen:
                continue
            g, libs = self.group(p)
            seen |= g
            groups.append((g, libs))
        target, libs = max(groups, key=lambda gl: len(gl[0]))
        pts = set(libs)
        for p in target:                      # white groups in contact with it
            for n in self.nbrs(*p):
                if self.b.get(n) == 0:
                    pts |= self.group(n)[1]
        return {q for q in pts if q in anchor and q not in self.b}


def dilate(xys, rings=1):
    cur = set(xys)
    for _ in range(rings):
        nxt = set(cur)
        for x, y in cur:
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    if 0 <= x + dx < SIZE and 0 <= y + dy < SIZE:
                        nxt.add((x + dx, y + dy))
        cur = nxt
    return cur


def make_query(black, white, moves, inside, visits, komi=6.5, rules="japanese"):
    """`black`/`white`/`moves` in GTP vertices; `inside` is the playable set."""
    allpts = {xy_to_gtp(x, y) for x in range(SIZE) for y in range(SIZE)}
    outside = sorted(allpts - inside)
    return {
        "initialStones": [["B", p] for p in black] + [["W", p] for p in white],
        "moves": [list(m) for m in moves],
        "rules": rules, "komi": komi,
        "boardXSize": SIZE, "boardYSize": SIZE,
        "maxVisits": visits,
        "includeOwnership": True,
        "avoidMoves": [{"player": "B", "moves": outside, "untilDepth": 60},
                       {"player": "W", "moves": outside, "untilDepth": 60}],
    }


def own_of(resp, verts):
    o = resp["ownership"]
    if not verts:
        return 0.0
    return sum(o[(SIZE - 1 - gtp_to_xy(v)[1]) * SIZE + gtp_to_xy(v)[0]] for v in verts) / len(verts)


def rank(resp, player, min_visits):
    s = 1.0 if player == "B" else -1.0
    ms = [m for m in resp["moveInfos"] if m["visits"] >= min_visits]
    ms.sort(key=lambda m: -s * m["scoreLead"])
    return ms


# ── the per-problem job ──────────────────────────────────────────────────────

def annotate_job(path, opt):
    """A coroutine: yields KataGo queries, receives responses, returns a report.

    Written as a generator so the driver can keep several problems in flight
    at once against one engine.
    """
    root, line = load_problem(path)
    if not line:
        return {"path": path, "skipped": "no main line"}

    black = [sgf_to_gtp(v) for v in root.get("AB", [])]
    white = [sgf_to_gtp(v) for v in root.get("AW", [])]
    solver = "B"

    anchor = {gtp_to_xy(v) for v in black + white}
    anchor |= {sgf_to_xy(p) for _, p in line}
    anchor = dilate(anchor, opt.rings)
    min_vis = max(20, opt.visits // 50)

    board = Board(black, white)   # advanced move by move as the line is walked

    book_pts = {sgf_to_xy(p) for _, p in line}
    # Search region and judging region are deliberately different sizes.
    #
    # The search needs room: confine it to a dozen points and the reply it
    # wants is often banned, so it mis-scores the whole line. The dilated
    # anchor (~50 points) is wide enough to evaluate honestly.
    #
    # Judging needs the opposite. Ranked over the whole search region the
    # "alternatives" include running the group out into open board — best play
    # on a real 19x19, but not what the problem asks, and useless as drill
    # feedback. So candidates are filtered down to the actual fight before
    # anything is flagged or refuted.
    inside = {xy_to_gtp(x, y) for x, y in anchor}

    def fight():
        pts = board.fight_points(anchor)
        pts |= {p for p in book_pts if p not in board.b}
        return {xy_to_gtp(x, y) for x, y in pts}

    def local(cands):
        f = fight()
        picked = [m for m in cands if m["move"] in f]
        return picked or cands

    def q(moves, visits=None):
        return make_query(black, white, moves, inside, visits or opt.visits)

    # Reading a group's life/death off ownership barely needs search — it is
    # mostly the raw net's own judgement — so verdict-only queries run cheap.
    # Ranking and point comparisons still get the full visit count.
    cheap = max(200, opt.visits // 6)

    report = {"path": path, "moves": [], "refutations": 0, "queries": 0}
    tree = Node()
    cursor = tree
    played = []
    group = list(black)          # black's stones, grown as the line goes

    for idx, (color, pt) in enumerate(line):
        vert = sgf_to_gtp(pt)
        last = idx == len(line) - 1
        node = Node(color, pt, correct=last)
        alt_node = None       # a better final move, added beside the book's

        if color == solver:
            resp = yield q(played)
            report["queries"] += 1
            cands = local(rank(resp, color, min_vis))
            by_move = {m["move"]: m for m in cands}
            best = cands[0] if cands else None
            mine = by_move.get(vert)

            # Only audit points once the group's fate is already decided.
            #
            # While life and death is still open, "better" legitimately
            # includes running the group out through the gap the book didn't
            # draw — genuinely the higher-scoring move on a real 19x19, and
            # genuinely not what the problem asks. No amount of tuning
            # separates those two, so the honest move is to not offer an
            # opinion there. Once the group is settled the remaining choice is
            # purely about points, which the engine reads reliably and which
            # is exactly the class of mistake worth flagging: the book ending
            # its line on a move that concedes a couple of points.
            settled = abs(own_of(resp, group)) >= opt.settled

            # Never decide from the root ranking alone. Spread over ~50 moves a
            # 1200-visit search orders near-equal moves more or less at random,
            # and a book move that spuriously ranks first would never get
            # checked at all — which is exactly how the known-loose C1 in
            # problem 1 slipped through. So always play the book's move and its
            # best local rival out and compare those two positions directly,
            # where the visits are concentrated and the number means something.
            sign = 1.0 if color == "B" else -1.0
            # A "better move" several lines away is a different plan, not a
            # correction — and on these cropped diagrams it is usually the
            # group running out through the gap the book never drew, which
            # scores well and answers a question the problem didn't ask.
            # Suggesting it would tell the reader their correct answer is
            # wrong, so only nearby refinements are offered.
            bx, by = gtp_to_xy(vert)

            def near(mv):
                x, y = gtp_to_xy(mv)
                return abs(x - bx) + abs(y - by) <= opt.max_alt_distance

            rivals = [m["move"] for m in cands
                      if m["move"] != vert and near(m["move"])][:2] if settled else []
            if rivals:
                ra = yield q(played + [(color, vert)])
                report["queries"] += 1
                mine_score = sign * ra["rootInfo"]["scoreLead"]
                rb = yield q(played + [(color, rivals[0])])
                report["queries"] += 1
                scored = [(sign * rb["rootInfo"]["scoreLead"], rivals[0])]
                # Only once it looks loose is it worth paying to find out which
                # alternative is actually the best one to name.
                if scored[0][0] - mine_score >= opt.loss_threshold and len(rivals) > 1:
                    rc = yield q(played + [(color, rivals[1])])
                    report["queries"] += 1
                    scored.append((sign * rc["rootInfo"]["scoreLead"], rivals[1]))
                scored.sort(reverse=True)
                top_score, top_move = scored[0]
                loss = top_score - mine_score
                if loss >= opt.loss_threshold and opt.verify_visits > opt.visits:
                    # A flag becomes a sentence the solver reads while drilling,
                    # so it has to be worth more than a cheap search's opinion.
                    # Measured against a re-check at 8000 visits, roughly a
                    # third of flags raised at 1200 either shrank to nothing or
                    # reversed outright — the book had been right. Only flagged
                    # moves pay for this, so it costs little.
                    ra2 = yield q(played + [(color, vert)], opt.verify_visits)
                    rb2 = yield q(played + [(color, top_move)], opt.verify_visits)
                    report["queries"] += 2
                    loss = sign * (rb2["rootInfo"]["scoreLead"]
                                   - ra2["rootInfo"]["scoreLead"])
                if loss >= opt.loss_threshold:
                    if node.correct:
                        # The last move of the line is the one marked C[RIGHT],
                        # and in the app's format a node carries either the
                        # correct flag or comment text, never both — so a note
                        # left here is silently thrown away. That is exactly
                        # where a book is most likely to be loose (it has
                        # already solved the problem and is finishing
                        # carelessly), so instead the note goes on the
                        # opponent's move just before, and the better move is
                        # added as an alternative that also solves.
                        if cursor.color:
                            cursor.comment = (
                                "Book finishes with %s here - %s is %.1f points better."
                                % (vert, top_move, loss))
                        alt_node = Node(color, gtp_to_sgf(top_move), correct=True)
                    else:
                        node.comment = ("Book move. %s is better by %.1f points."
                                        % (top_move, loss))
                    report["moves"].append(
                        {"n": idx + 1, "played": vert, "best": top_move,
                         "loss": round(loss, 2), "final": node.correct})

            # Refutations: plausible-looking moves that actually lose. Kept to
            # one punishing reply — the comment carries the reason, and a
            # solver who is already lost shouldn't have to play out the loss.
            if not last and opt.refutations:
                losers = [m for m in cands
                          if m["move"] != vert
                          and (best["scoreLead"] - m["scoreLead"]) >= opt.wrong_threshold]
                for m in losers[:opt.refutations]:
                    wrong = Node(color, gtp_to_sgf(m["move"]))
                    rr = yield q(played + [(color, m["move"])])
                    report["queries"] += 1
                    punish = local(rank(rr, "W" if color == "B" else "B", min_vis))
                    if not punish:
                        continue
                    pv = punish[0]["move"]
                    r2 = yield q(played + [(color, m["move"]),
                                           ("W" if color == "B" else "B", pv)], cheap)
                    report["queries"] += 1
                    o = own_of(r2, group + [m["move"]])
                    drop = best["scoreLead"] - m["scoreLead"]
                    why = ("the group dies" if o < -0.6
                           else "%.1f points worse" % drop)
                    # Plain ASCII in comments: these strings go through the app's
                    # own bitmap font, where an em-dash is not worth the risk.
                    wrong.comment = "Loses %.1f points - %s instead." % (drop, vert)
                    reply = Node("W" if color == "B" else "B", gtp_to_sgf(pv),
                                 comment="White punishes - %s." % why)
                    wrong.children.append(reply)
                    cursor.children.append(wrong)
                    report["refutations"] += 1

        parent = cursor
        cursor.children.append(node)
        # The book's move must stay the main line: the app and this tool both
        # treat the first branch as the spine, so move it to the front.
        cursor.children.insert(0, cursor.children.pop())
        if alt_node is not None:
            parent.children.append(alt_node)
        cursor = node
        played.append((color, vert))
        board.play(vert, color)
        if color == "B":
            group.append(vert)

    resp = yield q(played, cheap)
    report["queries"] += 1
    report["final_ownership"] = round(own_of(resp, group), 3)
    report["tree"] = tree
    report["root"] = root
    return report


# ── driver ───────────────────────────────────────────────────────────────────

def read_config(repo_root):
    cfg = {}
    p = os.path.join(repo_root, "apps", "ogs_client", "config.ini")
    if os.path.isfile(p):
        for ln in io.open(p, encoding="utf-8", errors="replace"):
            if "=" in ln and not ln.strip().startswith("#"):
                k, v = ln.split("=", 1)
                cfg[k.strip()] = v.strip()
    return cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("set_dir")
    ap.add_argument("--limit", type=int, default=0, help="only the first N problems")
    ap.add_argument("--jobs", type=int, default=4, help="problems analysed concurrently")
    ap.add_argument("--threads", type=int, default=6, help="katago numAnalysisThreads")
    ap.add_argument("--visits", type=int, default=1200,
                    help="visits for ranking/scoring; verdict-only queries use a sixth of this")
    ap.add_argument("--rings", type=int, default=1, help="dilation around the problem")
    ap.add_argument("--loss-threshold", type=float, default=1.0,
                    help="points a book move must give away before it is flagged")
    ap.add_argument("--wrong-threshold", type=float, default=2.5,
                    help="points a move must lose to be worth refuting")
    ap.add_argument("--refutations", type=int, default=2, help="wrong branches per solver turn")
    ap.add_argument("--max-alt-distance", type=int, default=2,
                    help="how far from the book's move a suggested improvement may be")
    ap.add_argument("--settled", type=float, default=0.6,
                    help="group ownership needed before point advice is offered at all")
    ap.add_argument("--verify-visits", type=int, default=8000,
                    help="re-measure a flagged move at this depth before writing the "
                         "comment; 0 disables (about a third of 1200-visit flags do "
                         "not survive the re-check)")
    ap.add_argument("--dry-run", action="store_true", help="analyse and report, write nothing")
    ap.add_argument("--report", default="", help="write the JSON report here")
    ap.add_argument("--verbose", action="store_true")
    opt = ap.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cfg = read_config(repo)
    exe = cfg.get("katago_exe")
    model = cfg.get("katago_model")
    kcfg = cfg.get("katago_config")
    if not (exe and model and kcfg) or not os.path.isfile(exe):
        sys.exit("katago_exe/katago_model/katago_config not usable in apps/ogs_client/config.ini")

    paths = sorted(os.path.join(opt.set_dir, f)
                   for f in os.listdir(opt.set_dir) if f.lower().endswith(".sgf"))
    if opt.limit:
        paths = paths[:opt.limit]
    if not paths:
        sys.exit("no SGFs in " + opt.set_dir)
    print("%d problems, %d concurrent, %d visits/query" % (len(paths), opt.jobs, opt.visits),
          flush=True)

    k = Kata(exe, model, kcfg, threads=opt.threads, verbose=opt.verbose)
    pending = deque(annotate_job(p, opt) for p in paths)
    active, done, t0, qid = {}, [], time.time(), 0

    def finish(r):
        """Record one finished problem, writing its SGF straight away.

        A multi-hour unattended batch should not be able to lose four hours of
        work to a crash in the fifth, and the job re-derives from the book's
        main line every time, so re-running over a partial set is safe.
        """
        done.append(r)
        if not opt.dry_run and "tree" in r:
            save_problem(r["path"], r["root"], r["tree"])
        r.pop("tree", None)
        r.pop("root", None)
        n = len(done)
        if n % 10 == 0 or n == len(paths):
            el = time.time() - t0
            eta = el / n * (len(paths) - n)
            print("  %d/%d  %.0fs elapsed, %.1fs/problem, ~%.0f min left"
                  % (n, len(paths), el, el / n, eta / 60), flush=True)

    def pump():
        nonlocal qid
        while pending and len(active) < opt.jobs:
            job = pending.popleft()
            try:
                query = next(job)
            except StopIteration as stop:
                if stop.value:
                    finish(stop.value)
                continue
            qid += 1
            query["id"] = str(qid)
            active[query["id"]] = job
            k.send(query)

    pump()
    while active:
        resp = k.recv()
        job = active.pop(resp.get("id", ""), None)
        if job is None:
            continue
        if "error" in resp:
            print("  katago error: %s" % resp["error"], file=sys.stderr, flush=True)
            pump()
            continue
        try:
            query = job.send(resp)
        except StopIteration as stop:
            if stop.value:
                finish(stop.value)
            pump()
            continue
        qid += 1
        query["id"] = str(qid)
        active[query["id"]] = job
        k.send(query)
        pump()
    k.close()

    loose = [r for r in done if r.get("moves")]
    total_flags = sum(len(r["moves"]) for r in loose)
    refs = sum(r.get("refutations", 0) for r in done)
    qs = sum(r.get("queries", 0) for r in done)
    el = time.time() - t0
    print("\n%d problems in %.0fs (%.1fs each, %d queries)" % (len(done), el, el / max(1, len(done)), qs))
    print("%d problems have a loose book move (%d moves total)" % (len(loose), total_flags))
    print("%d refutation branches added" % refs)

    worst = [(m, r["path"]) for r in loose for m in r["moves"]]
    worst.sort(key=lambda t: -t[0]["loss"])
    print("\nbiggest giveaways:")
    for m, p in worst[:15]:
        print("   %-40s move %2d  %s -> %s  loses %.1f"
              % (os.path.basename(p), m["n"], m["played"], m["best"], m["loss"]))

    if not opt.dry_run:
        print("\nrewrote %d SGFs" % sum(1 for r in done if "path" in r))

    if opt.report:
        with io.open(opt.report, "w", encoding="utf-8") as f:
            json.dump(done, f, indent=1)
        print("report: " + opt.report)


if __name__ == "__main__":
    main()
