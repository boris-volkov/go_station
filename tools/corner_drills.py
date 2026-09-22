#!/usr/bin/env python3
"""
Generate life-and-death drills for the standard corner shapes.

The shapes are the ones catalogued at Sensei's Library (`Corner shapes`):
the L group, the L+1 and L+2 groups, the rectangular six, the bulky five and
so on. Each one becomes a drill SGF in the same format the OGS client writes
from its own drill editor, so they appear under [MY DRILLS] alongside
hand-made ones.

Nothing here is transcribed by hand. Every drill is grown from an exact
solver, which is the point: a drill with a wrong line in it teaches the wrong
thing, and these shapes are exactly the ones you are trying to learn *because*
reading them out at the board is hard.

How a shape becomes a drill
---------------------------
1. **Seal it.** Sensei's draws the open formations with their escape routes
   open and a couple of distant stones standing in for "white is strong
   outside". That is fine for a diagram and useless for a drill, which has to
   be a self-contained position. Sealing carries the wall the diagram already
   draws out to the edge it is heading for — nothing is ever placed inside the
   group's eye space, so the defender ends up with the same room or more.

   A shape whose openness is not a wall one stone short of the edge does not
   seal, and is skipped rather than quietly altered. The tripod group is the
   clearest case: its eye space genuinely runs out into the open corner, so
   any enclosure of it is a different problem.

2. **Solve it exactly.** Not with an engine — with a full search of the sealed
   region under Go rules, which for these eight-to-eleven point regions is
   feasible and gives a proof rather than an opinion. See `Solver`.

3. **Check the answer against Sensei's.** Each shape carries the status
   Sensei's states with each side to play. A disagreement means either the
   sealing changed the shape or one of the two is wrong, and both are reasons
   not to hand someone a drill — so the affected side is reported and no
   drill is written for it. The check is per side, because a defence drill
   rests only on what happens when the defender moves first and there is no
   reason to throw it away because the attacking side turns out to be a ko.

4. **Grow the tree.** Solver-side moves are only ones proved to reach the
   goal; opponent moves are the real tries, each answered, so grinding the
   drill covers the resistance. Lines end when the question is settled.

Usage
-----
    python tools/corner_drills.py --list
    python tools/corner_drills.py --out my_games/<you>/drills
    python tools/corner_drills.py --out ... --verify      # replay + KataGo
    python tools/corner_drills.py --check my_games/<you>/drills

`--verify` replays every line under Go rules and then asks KataGo, as a second
and independent opinion, whether each line marked correct really reaches the
claimed result. `--check` runs those same checks over drills that already
exist, hand-made ones included, and additionally re-judges every first move a
drill presents as a *mistake* — the one error a replay can never catch, since
a line meant to end without a verdict looks fine however wrong it is.

KataGo paths come from apps/ogs_client/config.ini, the same as
katago_annotate.py. Existing files are never overwritten unless you pass
--force.
"""

import argparse
import os
import re
import sys
import threading
import time
from collections import deque

BOARD = 19
EMPTY, BLACK, WHITE = 0, 1, 2
E, D, A = 0, 1, 2            # compact colours: empty, defender, attacker

DEAD, ALIVE, SEKI, KO, UNKNOWN = "DEAD", "ALIVE", "SEKI", "KO", "UNKNOWN"
KILL, SAVE, LIVE, REFUTE = "KILL", "SAVE", "LIVE", "REFUTE"

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


# ── board and rules ──────────────────────────────────────────────────────────

def neighbours(r, c):
    if r > 0:
        yield r - 1, c
    if r < BOARD - 1:
        yield r + 1, c
    if c > 0:
        yield r, c - 1
    if c < BOARD - 1:
        yield r, c + 1


def group_and_liberties(board, r, c):
    colour = board[r][c]
    if colour == EMPTY:
        return set(), set()
    seen, libs, stack = {(r, c)}, set(), [(r, c)]
    while stack:
        cr, cc = stack.pop()
        for nr, nc in neighbours(cr, cc):
            v = board[nr][nc]
            if v == EMPTY:
                libs.add((nr, nc))
            elif v == colour and (nr, nc) not in seen:
                seen.add((nr, nc))
                stack.append((nr, nc))
    return seen, libs


def play_stone(board, r, c, colour):
    """Place a stone with captures. Raises ValueError if the move is illegal."""
    if board[r][c] != EMPTY:
        raise ValueError("occupied")
    nb = [row[:] for row in board]
    nb[r][c] = colour
    other = BLACK if colour == WHITE else WHITE
    captured = set()
    for nr, nc in neighbours(r, c):
        if nb[nr][nc] == other:
            grp, libs = group_and_liberties(nb, nr, nc)
            if not libs:
                captured |= grp
    for pr, pc in captured:
        nb[pr][pc] = EMPTY
    if not captured and not group_and_liberties(nb, r, c)[1]:
        raise ValueError("suicide")
    return nb, captured


def sgf_coord(r, c):
    return chr(ord('a') + c) + chr(ord('a') + r)


def parse_coord(spec):
    return (ord(spec[1]) - ord('a'), ord(spec[0]) - ord('a'))


def render(board, size=8):
    return "\n".join(" ".join(".XO"[board[r][c]] for c in range(size))
                     for r in range(size))


# ── the position book ────────────────────────────────────────────────────────

class Shape:
    """A corner shape, its sealed position, and what Sensei's says about it.

    `attacker_first` / `defender_first` are the statuses Sensei's states with
    that side to play, transcribed from the page and never adjusted to match
    the solver — a disagreement has to be able to fire.

    `rows` is the sealed position: '.' empty, 'X' black, 'O' white, top-left
    corner of the board. Sealing stones are already included, so the file is
    the whole truth about the position and can be corrected by hand.
    """

    def __init__(self, key, name, rows, attacker_first, defender_first,
                 source, note="", defender=BLACK, drills=None):
        self.key = key
        self.name = name
        self.rows = rows
        self.attacker_first = attacker_first
        self.defender_first = defender_first
        self.source = source
        self.note = note
        self.defender = defender
        self.attacker = WHITE if defender == BLACK else BLACK
        self.drills = drills if drills is not None else self._default_drills()

    def _default_drills(self):
        """Which drills the status supports.

        Only sides with a clean outcome become drills. Where the best either
        side can get is a ko or a seki there is no move that is simply
        "right", so that side is left out rather than forced into a shape the
        drill format cannot express.
        """
        out = []
        if self.attacker_first == DEAD:
            # Refuting only makes sense for a shape that dies even when the
            # defender moves first; otherwise the drill is "kill it", with
            # the attacker on move.
            out.append(REFUTE if self.defender_first == DEAD else KILL)
        elif self.attacker_first == ALIVE:
            out.append(SAVE)
        if self.defender_first == ALIVE and self.attacker_first != ALIVE:
            out.append(LIVE)
        return out

    def board(self):
        b = [[EMPTY] * BOARD for _ in range(BOARD)]
        for r, row in enumerate(self.rows):
            for c, ch in enumerate(row.split()):
                if ch == "X":
                    b[r][c] = BLACK
                elif ch == "O":
                    b[r][c] = WHITE
        return b


# The shapes, with their positions already sealed. Keeping them here rather
# than re-deriving them from Sensei's on every run makes the drills
# reproducible offline and lets a position be read — or corrected — by hand.
#
# The statuses are transcribed from the Corner shapes page and are never
# adjusted to match the solver; a disagreement has to be able to fire, since
# that is the signal that an enclosure changed the shape. KO stands for "ko
# or seki", i.e. no clean answer for that side, and those sides are not
# turned into drills.
SHAPES = [
    Shape(
        "l-group", "THE L-GROUP",
        [
            ". . . O . . . .",
            ". . X O . O . .",
            ". . X O . . . .",
            ". X X O . . . .",
            "O O O O . . . .",
            ". . . . . . . .",
            ". O . . . . . .",
            ". . . . . . . .",
        ],
        DEAD, DEAD,
        source="Sensei's Library, Corner shapes — L group",
        note="dead as it stands; the 2x3 corner space is not enough",
    ),   # eye space 8 points, 4 liberties
    Shape(
        "l1-long-leg", "THE L+1 GROUP (LEG ON THE LONG SIDE)",
        [
            ". . . O . . . .",
            ". . X O . O . .",
            ". . X O . . . .",
            ". X X O . . . .",
            ". X O O . . . .",
            "O O O . . . . .",
            ". . . . . . . .",
            ". O . . . . . .",
        ],
        DEAD, ALIVE,
        source="Sensei's Library, Corner shapes — L+1 group",
        note="unsettled: it lives or dies according to who plays first",
    ),   # eye space 9 points, 5 liberties
    Shape(
        "l1-short-leg", "THE L+1 GROUP (LEG ON THE SHORT SIDE)",
        [
            ". . . . O . . .",
            ". . X X O . O .",
            ". . X O O . . .",
            ". X X O . . . .",
            "O O O O . . . .",
            ". . . . . . . .",
            ". O . . . . . .",
            ". . . . . . . .",
        ],
        DEAD, ALIVE,
        source="Sensei's Library, Corner shapes — L+1 group",
        note="unsettled: it lives or dies according to who plays first",
    ),   # eye space 9 points, 5 liberties
    Shape(
        "l2-group", "THE L+2 GROUP",
        [
            ". . . . O . . .",
            ". . X X O . O .",
            ". . X O O . . .",
            ". X X O . . . .",
            ". X O O . . . .",
            "O O O . . . . .",
            ". . . . . . . .",
            ". O . . . . . .",
        ],
        ALIVE, ALIVE,
        source="Sensei's Library, Corner shapes — L+2 group",
        note="alive as it stands",
    ),   # eye space 10 points, 6 liberties
    Shape(
        "bulky-five", "BULKY FIVE IN THE CORNER",
        [
            ". . X O . . . .",
            ". . X O . . . .",
            ". X X O . . . .",
            "X X O . . . . .",
            "O O . O . . . .",
            ". O . . . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
        ],
        DEAD, ALIVE,
        source="Sensei's Library, Corner shapes — Bulky five",
        note="the vital point decides it, exactly as for a bulky five anywhere else",
    ),   # eye space 5 points, 3 liberties
    Shape(
        "rect-six", "RECTANGULAR SIX IN THE CORNER",
        [
            ". . X O . . . .",
            ". . X O . . . .",
            ". . X O . . . .",
            "X X X O . . . .",
            "O O O O . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
        ],
        DEAD, ALIVE,
        source="Sensei's Library, Corner shapes — Rectangular six in the corner",
        note="with no outside liberties it can be killed outright",
    ),   # eye space 6 points, 4 liberties
    Shape(
        "bent-four", "BENT FOUR IN THE CORNER",
        [
            ". . X O . . . .",
            ". X X O . . . .",
            ". X O . O . . .",
            "X X O . . . . .",
            "O O . . . . . .",
            ". . O . . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
        ],
        KO, ALIVE,
        source="Sensei's Library, Corner shapes — Bent Four in the corner",
        note="black lives unconditionally on the vital point; white can only get a ko",
    ),   # eye space 4 points, 3 liberties
    Shape(
        "weak-carpenters-square", "THE WEAK CARPENTER'S SQUARE",
        [
            ". . . . O . . .",
            ". . . X O . . .",
            ". . . X O . . .",
            ". X X O . . . .",
            "O O O . O . . .",
            ". . . . . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
        ],
        DEAD, ALIVE,
        source="Sensei's Library, Corner shapes — Weak carpenter's square",
        note="unsettled: black lives on the vital point, white kills with almost anything",
    ),   # eye space 11 points, 5 liberties
    Shape(
        "j-group", "THE J-GROUP (BIG PIG'S SNOUT)",
        [
            ". . . O . . . .",
            ". . X O O O . .",
            ". . X O . . . .",
            "X . X O . . . .",
            ". X O . . . . .",
            "O O O . . . . .",
            ". . . . . . . .",
            ". O . . . . . .",
        ],
        DEAD, ALIVE,
        source="Sensei's Library, Corner shapes — J Group",
        note="same status as an L+1 group: it lives or dies according to sente",
    ),   # eye space 9 points, 6 liberties
    Shape(
        "carpenters-square", "THE CARPENTER'S SQUARE",
        [
            ". . . . O . . .",
            ". . . X O . . .",
            ". . . X O . . .",
            ". X X X O . . .",
            "O O O O O . . .",
            ". . . . . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
        ],
        KO, ALIVE,
        source="Sensei's Library, Corner shapes — Carpenter's Square",
        note="black can live with almost any move; white's best is a ko",
    ),   # eye space 11 points, 5 liberties
    Shape(
        "twisted-four", "TWISTED FOUR IN THE CORNER",
        [
            "X . X O . . . .",
            ". . X O . . . .",
            ". X X O . . . .",
            "X X O . . . . .",
            "O O . O . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
        ],
        KO, ALIVE,
        source="Sensei's Library, Corner shapes — Twisted four in the corner",
        note="with two or more outside liberties black is alive; otherwise white gets a ko",
    ),   # eye space 4 points, 4 liberties
    Shape(
        "long-l-group", "THE LONG L-GROUP",
        [
            ". . . O . . . .",
            ". . X O O . . .",
            ". . X O . . . .",
            ". . X O . . . .",
            ". X X O . . . .",
            "O O O O . . . .",
            ". O . . . . . .",
            ". . . . . . . .",
        ],
        KO, ALIVE,
        source="Sensei's Library, Corner shapes — Long L group",
        note="white's best is a ten thousand year ko, or seki with an outside liberty",
    ),   # eye space 10 points, 5 liberties
    Shape(
        "straight-j-group", "THE STRAIGHT J-GROUP",
        [
            ". . . O . . . .",
            ". . X O . O . .",
            ". . X O . . . .",
            ". . X O . . . .",
            "X X O . . . . .",
            "O O O . . . . .",
            ". . . . . . . .",
            ". O . . . . . .",
        ],
        KO, ALIVE,
        source="Sensei's Library, Corner shapes — Straight J Group",
        note="white playing first gets a ko at best",
    ),   # eye space 9 points, 5 liberties
    Shape(
        "weak-rect-six", "WEAK RECTANGULAR SIX IN THE CORNER",
        [
            ". . X O . . . .",
            ". . X O . . . .",
            ". . X O . . . .",
            "X X O O . . . .",
            ". O . . . . . .",
            "O . O . . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
        ],
        KO, ALIVE,
        source="Sensei's Library, Corner shapes — Weak rectangular six in the corner",
        note="the corner point is white, which makes the shape weaker",
    ),   # eye space 7 points, 5 liberties
    Shape(
        "big-p", "THE BIG P",
        [
            ". . X O . . . .",
            ". . X O . . . .",
            ". X X O . . . .",
            ". X O . . . . .",
            "X X O . . . . .",
            "O O . O . . . .",
            ". . . . . . . .",
            ". . . . . . . .",
        ],
        KO, ALIVE,
        source="Sensei's Library, Corner shapes — Big P",
        note="it looks like six points; black must play the vital point to keep five",
    ),   # eye space 6 points, 4 liberties
]


# ── the enclosed problem ─────────────────────────────────────────────────────

class Problem:
    """A shape framed as a life-and-death question over a sealed region."""

    def __init__(self, board, defender, name=""):
        self.board = board
        self.defender = defender
        self.attacker = BLACK if defender == WHITE else WHITE
        self.name = name
        self.target = {(r, c) for r in range(BOARD) for c in range(BOARD)
                       if board[r][c] == defender}
        if not self.target:
            raise ValueError("%s: no defender stones" % name)
        self.region = self._flood()

    def _flood(self):
        """Empty points the group can reach without crossing the wall."""
        region, queue = set(), deque()
        for (r, c) in self.target:
            for nr, nc in neighbours(r, c):
                if self.board[nr][nc] == EMPTY and (nr, nc) not in region:
                    region.add((nr, nc))
                    queue.append((nr, nc))
        while queue:
            r, c = queue.popleft()
            for nr, nc in neighbours(r, c):
                if (nr, nc) in region:
                    continue
                v = self.board[nr][nc]
                if v == EMPTY:
                    region.add((nr, nc))
                    queue.append((nr, nc))
                elif v == self.defender:
                    for pt in group_and_liberties(self.board, nr, nc)[1]:
                        if pt not in region:
                            region.add(pt)
                            queue.append(pt)
        return region

    def validate(self, max_region=12, min_wall_libs=2):
        """Reject positions the solver's assumptions would not hold for."""
        bad = []
        if len(self.region) > max_region:
            bad.append("region is %d points (leaks, or too big to solve)"
                       % len(self.region))
            return bad
        seen = set()
        for (r, c) in self.region:
            for nr, nc in neighbours(r, c):
                if self.board[nr][nc] == self.attacker and (nr, nc) not in seen:
                    grp, libs = group_and_liberties(self.board, nr, nc)
                    seen |= grp
                    if len(libs - self.region) < min_wall_libs:
                        bad.append("wall at %s has %d liberties outside the region"
                                   % (sgf_coord(*sorted(grp)[0]),
                                      len(libs - self.region)))
        return bad

    def liberties(self):
        libs = set()
        for (r, c) in self.target:
            for nr, nc in neighbours(r, c):
                if self.board[nr][nc] == EMPTY:
                    libs.add((nr, nc))
        return libs


# ── compact region model ─────────────────────────────────────────────────────

class Region:
    """Precomputed geometry of one sealed problem.

    The search runs on a tuple of colours over the region's points, with the
    wall folded in as a constant. That removes the 19x19 board copy that would
    otherwise happen at every node, which is the difference between minutes
    and hours on a ten-point region.

    The wall is attacker stones with liberties outside the region, treated as
    immortal — which is exactly what `Problem.validate` checks. An attacker
    stone played inside the region that touches the wall joins it and is
    immortal too; ones that do not form ordinary, capturable chains. The
    defender can never touch the wall (adjacent stones of opposite colour are
    not connected), so defender chains live entirely inside the region.
    """

    def __init__(self, problem):
        self.problem = problem
        pts = sorted(problem.region | problem.target)
        inside = set(pts)
        # Vital points first: the point with the most neighbours inside the
        # eye space is the one to try first, and trying it first is what lets
        # the cut-offs fire early instead of after the whole fan-out.
        pts.sort(key=lambda p: (-sum(1 for n in neighbours(*p) if n in inside), p))
        self.pts = pts
        self.index = {p: i for i, p in enumerate(pts)}
        self.n = len(pts)

        board, att = problem.board, problem.attacker
        self.nbrs, self.walled, init = [], [], []
        for (r, c) in pts:
            ns, touches = [], False
            for nb in neighbours(r, c):
                j = self.index.get(nb)
                if j is not None:
                    ns.append(j)
                elif board[nb[0]][nb[1]] == att:
                    touches = True
            self.nbrs.append(tuple(ns))
            self.walled.append(touches)
            v = board[r][c]
            init.append(D if v == problem.defender else (A if v == att else E))
        # States are bytes, not tuples. They are the key of every entry in a
        # transposition table that reaches millions of rows on the larger
        # regions, and a 16-byte bytes object is about a quarter the size of
        # the equivalent tuple of ints — the difference between a couple of
        # gigabytes and something that gets the process killed.
        self.init = bytes(init)


class Illegal(Exception):
    pass


class BudgetExceeded(Exception):
    """Raised on its own so it is never confused with a RecursionError.

    RecursionError subclasses RuntimeError, so catching RuntimeError around
    the search quietly turns "this got too deep" into a verdict.
    """


def chain(reg, state, i):
    """(stones, liberties, immortal) for the chain at index i."""
    colour = state[i]
    seen, libs, stack = {i}, set(), [i]
    immortal = False
    while stack:
        k = stack.pop()
        if colour == A and reg.walled[k]:
            immortal = True
        for j in reg.nbrs[k]:
            v = state[j]
            if v == E:
                libs.add(j)
            elif v == colour and j not in seen:
                seen.add(j)
                stack.append(j)
    return seen, libs, immortal


def place(reg, state, i, colour):
    """Place `colour` at index i; returns (state, captured) or raises Illegal."""
    if state[i] != E:
        raise Illegal("occupied")
    ns = bytearray(state)
    ns[i] = colour
    other = D if colour == A else A
    captured = set()
    for j in reg.nbrs[i]:
        if ns[j] == other:
            stones, libs, immortal = chain(reg, ns, j)
            if not libs and not immortal:
                captured |= stones
    for k in captured:
        ns[k] = E
    if not captured:
        _, libs, immortal = chain(reg, ns, i)
        if not libs and not immortal:
            raise Illegal("suicide")
    return bytes(ns), captured


def eye_count(reg, state):
    """defender-chain-id -> how many enclosed eyes it owns."""
    chain_id, next_id = {}, 0
    for i in range(reg.n):
        if state[i] == D and i not in chain_id:
            stones, _, _ = chain(reg, state, i)
            for k in stones:
                chain_id[k] = next_id
            next_id += 1

    seen, counts = set(), {}
    for i in range(reg.n):
        if state[i] != E or i in seen:
            continue
        comp, stack, ok, owners = {i}, [i], True, set()
        seen.add(i)
        while stack:
            k = stack.pop()
            if reg.walled[k]:
                ok = False
            for j in reg.nbrs[k]:
                if state[j] == E:
                    if j not in seen:
                        seen.add(j)
                        comp.add(j)
                        stack.append(j)
                elif state[j] == D:
                    owners.add(chain_id[j])
                else:
                    ok = False
        if ok and len(owners) == 1:
            who = next(iter(owners))
            counts[who] = counts.get(who, 0) + 1
    return counts


def two_eyes(reg, state):
    return any(v >= 2 for v in eye_count(reg, state).values())


def can_live_alone(reg, state, memo):
    """Could the defender reach two eyes if the attacker never moved again?

    This is what separates life from seki once the attacker gives up. Testing
    for two eyes *right now* is the wrong question — a group behind one big
    eye space has not split it yet and does not need to until attacked, and
    asking that way calls plainly alive groups seki. In a seki the defender
    cannot reach two eyes even playing every move itself.

    Terminates because each defender move adds a defender stone and the
    defender's own stones can never come off (self-capture is illegal).
    """
    hit = memo.get(state)
    if hit is not None:
        return hit
    if two_eyes(reg, state):
        memo[state] = True
        return True
    for i in range(reg.n):
        if state[i] != E:
            continue
        try:
            ns, _ = place(reg, state, i, D)
        except Illegal:
            continue
        if can_live_alone(reg, ns, memo):
            memo[state] = True
            return True
    memo[state] = False
    return False


_ORDER = {DEAD: 0, SEKI: 1, ALIVE: 2}


class Solver:
    """Exact search over the sealed region.

    Verdicts are DEAD, ALIVE or SEKI. A repetition is scored as survival for
    the defender — the same player facing the same position having gained
    nothing has made no progress, and an attacker who throws a stone in and
    lets it be captured is back where they started rather than a move closer
    to a kill. Genuine ko lives in that same bucket, so any verdict a
    repetition contributed to sets `loop_seen`, and callers treat those as
    "ko, look closer" rather than as an answer.
    """

    # The transposition table is what makes a ten- or eleven-point region
    # tractable, and it is also the only thing here that grows without bound.
    # The cap is a backstop against a region big enough to exhaust memory, not
    # a tuning knob: cutting it too low is worse than useless, because every
    # position that falls out of the table gets searched again from scratch,
    # which costs both time and stack depth.
    MEMO_LIMIT = 12_000_000

    def __init__(self, region, node_budget=80_000_000):
        self.reg = region
        self.budget = node_budget
        self.nodes = 0
        self.memo = {}
        self.live_memo = {}
        self.loop_seen = False
        self.exhausted = None

    def solve(self, attacker_to_move):
        self.nodes = 0
        self.memo = {}
        self.loop_seen = False
        self.exhausted = None
        try:
            status, used = self._search(
                self.reg.init, A if attacker_to_move else D, None, set())
            self.loop_seen = used
            return status
        except (BudgetExceeded, RecursionError) as exc:
            self.exhausted = type(exc).__name__
            return UNKNOWN

    def status_after(self, state, turn, ko=None):
        return self._search(state, turn, ko, set())

    def _search(self, state, turn, ko, path):
        self.nodes += 1
        if self.nodes > self.budget:
            raise BudgetExceeded("node budget")
        if D not in state:
            return DEAD, False

        key = (state, turn, ko)
        if key in path:
            return ALIVE, True
        hit = self.memo.get(key)
        if hit is not None:
            return hit, False

        # The path set is mutated in place and unwound on the way out;
        # copying it per node costs O(depth) allocation at every one of
        # millions of nodes.
        path.add(key)
        try:
            return self._expand(state, turn, ko, path, key)
        finally:
            path.discard(key)

    def _expand(self, state, turn, ko, path, key):
        results = []
        for i in range(self.reg.n):
            if state[i] != E or i == ko:
                continue
            try:
                ns, captured = place(self.reg, state, i, turn)
            except Illegal:
                continue
            results.append(self._search(ns, D if turn == A else A,
                                        ko_point(self.reg, ns, i, captured), path))
            if turn == A and results[-1][0] == DEAD:
                break
            if turn == D and results[-1][0] == ALIVE:
                break

        if turn == A:
            # The attacker giving up settles it: they would only ever pass
            # when nothing worked, so it is the floor of their options.
            results.append(
                (ALIVE if can_live_alone(self.reg, state, self.live_memo)
                 else SEKI, False))
        else:
            results.append(self._search(state, A, None, path))

        pick = min if turn == A else max
        best, used = pick(results, key=lambda rl: _ORDER[rl[0]])
        # Cached even when a repetition contributed: a repetition's value is
        # the value of that player making no progress, which is a fact about
        # the position rather than the path that reached it. (The usual
        # graph-history-interaction approximation; the ko cases it blurs are
        # the ones `loop_seen` flags.)
        if len(self.memo) < self.MEMO_LIMIT:
            self.memo[key] = best
        return best, used


def ko_point(reg, state, i, captured):
    """The point a simple ko forbids, if this capture made one."""
    if len(captured) != 1:
        return None
    stones, libs, _ = chain(reg, state, i)
    if len(stones) == 1 and len(libs) == 1:
        return next(iter(captured))
    return None


def run_deep(fn, stack_mb=64, frame_bytes=1600):
    """Run `fn` on a thread with a big stack and a raised recursion limit.

    Capture-and-recapture lines go far deeper than the region has points, so
    the default 1000-frame limit is hit long before the search is finished.

    The limit is derived from the stack actually obtained rather than set to
    some large round number: a recursion limit the stack cannot hold does not
    give a clean RecursionError, it overruns the stack and takes the process
    down without a traceback.
    """
    box = {}
    got_mb = stack_mb

    def work():
        sys.setrecursionlimit(max(2000, got_mb * 1024 * 1024 // frame_bytes))
        try:
            box["value"] = fn()
        except BaseException as exc:
            box["error"] = exc

    old = threading.stack_size()
    for mb in (stack_mb, 32, 16, 8):
        try:
            threading.stack_size(mb * 1024 * 1024)
            got_mb = mb
            break
        except (ValueError, RuntimeError):
            continue
    try:
        t = threading.Thread(target=work)
        t.start()
        t.join()
    finally:
        threading.stack_size(old)
    if "error" in box:
        raise box["error"]
    return box["value"]


# ── growing the drill tree ───────────────────────────────────────────────────

class Node:
    __slots__ = ("idx", "colour", "correct", "children", "wrong")

    def __init__(self, idx, colour):
        self.idx = idx
        self.colour = colour
        self.correct = False
        self.children = []
        self.wrong = False


def has_correct(node):
    return node.correct or any(has_correct(c) for c in node.children)


def count_nodes(node):
    return 1 + sum(count_nodes(c) for c in node.children)


def tree_depth(node):
    return 1 + max([tree_depth(c) for c in node.children], default=0)


# kind -> (solver plays the attacker, who moves first, what the solver wants)
DRILL_SPEC = {
    KILL:   (True,  A, DEAD),
    REFUTE: (True,  D, DEAD),
    SAVE:   (False, A, ALIVE),
    LIVE:   (False, D, ALIVE),
}


class TreeBuilder:
    """Grows a drill tree from the solver's own knowledge.

    At the solver's turn only moves proved to reach the goal are offered; at
    the opponent's turn every try worth drilling against, each of which the
    solver is proved able to answer. Lines end when the question is settled.
    """

    def __init__(self, region, solver, kind,
                 max_nodes=400, max_depth=20, wrong_moves=3, deep_replies=2):
        self.reg = region
        self.s = solver
        solver_is_attacker, self.first, self.goal = DRILL_SPEC[kind]
        self.solver_side = A if solver_is_attacker else D
        self.max_nodes = max_nodes
        self.max_depth = max_depth
        self.wrong_moves = wrong_moves
        self.deep_replies = deep_replies
        self.nodes = 0
        self.truncated = False
        # Whether the solver ever actually had to choose. A shape can be
        # alive (or dead) so comfortably that every legal move reaches the
        # goal, and a drill where anything you play is marked RIGHT teaches
        # nothing — for those the real lesson is about territory, which this
        # format has nowhere to put.
        self.had_choice = False
        # The stones the drill is about. A kill is finished when *these* are
        # captured, not when the last defender stone anywhere in the region is
        # gone: after the corner falls the defender can keep dropping stones
        # into the space it used to occupy, and hoovering those up turns a
        # five-move kill into a twenty-move one.
        self.target = frozenset(i for i in range(region.n)
                                if region.init[i] == D)

    # ── when a line is over ──────────────────────────────────────────────
    def settled(self, state, ko=None):
        """Has the drill's question been answered on the board itself?

        For a kill: the eye space can no longer yield two eyes even if the
        defender were handed every remaining move — the shape a player reads
        as simply dead.

        For a defence: the defender never has to answer again. Waiting for two
        eyes to be literally on the board is too strict, because the attacker
        can keep throwing stones in and the eyes only exist once those are
        captured, so the line never ends. The real question is whether any
        attacker move still needs a reply, and the solver answers it exactly:
        play each try, let the defender tenuki, and see if the group holds.
        """
        if self.goal == DEAD:
            if not any(state[i] == D for i in self.target):
                return True
            return not can_live_alone(self.reg, state, self.s.live_memo)
        if two_eyes(self.reg, state):
            return True
        moves = self.legal(state, A, ko)
        if not moves:
            # The attacker having nothing legal to play *right now* is not
            # safety: with the eye space nearly filled, every attacker move
            # can be self-capture while the defender is still dead, because
            # the defender is the one who will run out of moves and have to
            # fill its own shape. Ask the solver outright instead.
            return self.status(state, A, ko) == ALIVE
        for (i, ns, nko) in moves:
            if self.status(ns, A, nko) != ALIVE:
                return False
        return True

    def refuted(self, state):
        """For a wrong line: has the solver's mistake been punished yet?"""
        if self.goal == DEAD:
            return two_eyes(self.reg, state)
        return (not any(state[i] == D for i in self.target) or
                not can_live_alone(self.reg, state, self.s.live_memo))

    # ── moves ────────────────────────────────────────────────────────────
    def legal(self, state, colour, ko=None):
        out = []
        for i in range(self.reg.n):
            if state[i] != E or i == ko:
                continue
            try:
                ns, captured = place(self.reg, state, i, colour)
            except Illegal:
                continue
            out.append((i, ns, ko_point(self.reg, ns, i, captured)))
        return out

    def status(self, state, turn, ko):
        """The solver's verdict, but only when it does not hinge on a ko.

        A simple-ko ban can make a ko position look like settled life: the
        opponent cannot retake this instant, the search finds no other way
        forward, and nothing ever repeats for `loop_seen` to notice. Ending a
        drill there would mark "you are alive because it is not his turn to
        take the ko yet" as the correct answer.

        So the position is judged twice, once under the ban and once without
        it, and only an answer that survives both counts. Returns None when
        they differ, which every caller reads as "not the goal" — conservative
        in both directions, since the two runs hand the extra option to
        opposite players.
        """
        under_ban = self.s.status_after(state, turn, ko)[0]
        if ko is None:
            return under_ban
        without_ban = self.s.status_after(state, turn, None)[0]
        return under_ban if under_ban == without_ban else None

    def winning(self, state, colour, ko=None):
        other = D if colour == A else A
        legal = self.legal(state, colour, ko)
        good = [m for m in legal
                if self.status(m[1], other, m[2]) == self.goal]
        if colour == self.solver_side and legal and len(good) < len(legal):
            self.had_choice = True
        return good

    def resistance(self, state, colour, ko, limit):
        """Opponent tries worth drilling against, most testing first.

        Every legal move would be exhaustive but unplayable: the whole region
        at every opponent turn multiplies out into hundreds of nodes of moves
        nobody would make. Two filters cut it to the tries a real opponent
        has — drop moves that hand the solver the result on the spot, and drop
        transpositions, which are the same lesson twice. What is left is
        ordered by how few winning replies the solver has, so the moves that
        have to be answered precisely come first.
        """
        seen, scored = set(), []
        for (i, ns, nko) in self.legal(state, colour, ko):
            if ns in seen:
                continue
            seen.add(ns)
            if self.settled(ns, nko):
                continue
            replies = self.winning(ns, self.solver_side, nko)
            if not replies:
                continue        # would make the drill unwinnable
            scored.append((len(replies), i, ns, nko))
        scored.sort()
        return [(i, ns, nko) for (_, i, ns, nko) in scored[:limit]]

    # ── the tree ─────────────────────────────────────────────────────────
    def build(self):
        root = Node(None, None)
        state, turn = self.reg.init, self.first
        if turn == self.solver_side:
            moves = self.winning(state, turn)
            if not moves:
                return None, "the solver has no winning first move"
            for (i, ns, nko) in moves:
                root.children.append(self._grow(i, ns, turn, nko, 1))
            self._add_wrong_first_moves(root, state, turn)
        else:
            for (i, ns, nko) in self.resistance(state, turn, None, 99):
                root.children.append(self._grow(i, ns, turn, nko, 1))
        root.children = [c for c in root.children if has_correct(c) or c.wrong]
        if not any(has_correct(c) for c in root.children):
            return None, "no line reached a correct ending"
        if not self.had_choice:
            return None, ("every legal move reaches the goal — the shape is "
                          "settled, so there is nothing to drill")
        return root, None

    def _grow(self, idx, state, colour, ko, depth):
        node = Node(idx, colour)
        self.nodes += 1
        if colour == self.solver_side and self.settled(state, ko):
            node.correct = True
            return node
        if depth >= self.max_depth or self.nodes > self.max_nodes:
            self.truncated = True
            return node
        nxt = D if colour == A else A
        if nxt == self.solver_side:
            moves = self.winning(state, nxt, ko)[:1]
        else:
            moves = self.resistance(state, nxt, ko, self.deep_replies)
        for (i, ns, nko) in moves:
            child = self._grow(i, ns, nxt, nko, depth + 1)
            if nxt != self.solver_side and not has_correct(child):
                continue        # never offer resistance the solver cannot beat
            node.children.append(child)
        return node

    def _add_wrong_first_moves(self, root, state, turn):
        """A few losing first tries, played out until they demonstrably fail.

        Without these a wrong guess just drops off the authored tree into free
        play; with them it gets a verdict, which is the point of drilling.
        """
        taken = {c.idx for c in root.children}
        other = D if turn == A else A
        losers = [m for m in self.legal(state, turn)
                  if m[0] not in taken
                  and self.status(m[1], other, m[2]) != self.goal]
        for (i, ns, nko) in losers[:self.wrong_moves]:
            node = self._grow_wrong(i, ns, turn, nko, 1)
            if node is not None and node.children:
                node.wrong = True
                root.children.append(node)

    def _grow_wrong(self, idx, state, colour, ko, depth):
        """A line in which the solver has already gone wrong.

        It has to end on an *opponent* move with nothing after it — that is
        what the app reads as a verdict of WRONG. Ending on the solver's own
        move would just leave them to play on.
        """
        node = Node(idx, colour)
        nxt = D if colour == A else A
        if depth >= 8:
            return node if colour != self.solver_side else None
        if nxt == self.solver_side:
            moves = self.legal(state, nxt, ko)
            if not moves:
                return node
            best = moves[0]
            for m in moves:
                if self.status(m[1], D if nxt == A else A,
                               m[2]) == self.goal:
                    best = m
                    break
            node.children.append(self._grow_wrong(best[0], best[1], nxt,
                                                  best[2], depth + 1))
            return node
        other = self.solver_side
        moves = [m for m in self.legal(state, nxt, ko)
                 if self.status(m[1], other, m[2]) != self.goal]
        if not moves:
            return node
        i, ns, nko = moves[0]
        if self.refuted(ns):
            node.children.append(Node(i, nxt))   # dead end -> the app says WRONG
            return node
        node.children.append(self._grow_wrong(i, ns, nxt, nko, depth + 1))
        return node


# ── SGF ──────────────────────────────────────────────────────────────────────

def sgf_escape(s):
    return s.replace("\\", "\\\\").replace("]", "\\]")


VERB = {KILL: "KILL", REFUTE: "KILL", SAVE: "SAVE", LIVE: "LIVE IN"}
TASK = {
    KILL:   "KILL THE CORNER",
    REFUTE: "THE CORNER IS DEAD - REFUTE EVERY ATTEMPT TO LIVE",
    SAVE:   "THE CORNER IS ALIVE - ANSWER EVERY ATTACK",
    LIVE:   "MAKE LIFE IN THE CORNER",
}


def drill_name(shape, kind):
    return "%s %s" % (VERB[kind], shape.name)


def drill_filename(shape, kind):
    stem = drill_name(shape, kind).replace(" ", "_")
    stem = "".join(ch for ch in stem if ch.isalnum() or ch in "_-+()")
    return stem + ".sgf"


def to_sgf(region, root, kind, shape, date=None):
    """Write the tree in the same format the app's own drill editor writes."""
    board, defender = region.problem.board, region.problem.defender
    solver_is_attacker, first, _ = DRILL_SPEC[kind]
    defender_is_black = defender == BLACK
    solver_is_black = (not defender_is_black) if solver_is_attacker \
        else defender_is_black
    first_is_black = (first == D) == defender_is_black

    out = ["(;GM[1]FF[4]CA[UTF-8]SZ[19]"]
    for colour, tag in ((BLACK, "AB"), (WHITE, "AW")):
        pts = [(r, c) for r in range(BOARD) for c in range(BOARD)
               if board[r][c] == colour]
        if pts:
            out.append(tag + "".join("[%s]" % sgf_coord(r, c) for r, c in pts))
    out.append("PL[%s]" % ("B" if first_is_black else "W"))
    out.append("ZS[%s]" % ("B" if solver_is_black else "W"))
    out.append("GN[%s]" % sgf_escape(drill_name(shape, kind)))
    out.append("SO[%s]" % sgf_escape(shape.source))
    out.append("AP[corner_drills]")
    out.append("DT[%s]" % (date or time.strftime("%Y-%m-%d")))
    out.append("C[%s TO PLAY - %s]"
               % ("BLACK" if solver_is_black else "WHITE", TASK[kind]))

    def letter(node_colour):
        return "B" if ((node_colour == D) == defender_is_black) else "W"

    def node_text(child):
        r, c = region.pts[child.idx]
        return ";%s[%s]%s" % (letter(child.colour), sgf_coord(r, c),
                              "C[RIGHT]" if child.correct else "")

    def ser(node):
        # Single children are chained rather than parenthesised, matching the
        # app's own drill writer — same tree, shallower nesting, and the files
        # read like the hand-made ones sitting next to them.
        out = []
        cur = node
        while len(cur.children) == 1:
            cur = cur.children[0]
            out.append(node_text(cur))
        for child in cur.children:
            out.append("(" + node_text(child) + ser(child) + ")")
        return "".join(out)

    out.append(ser(root))
    out.append(")\n")
    return "".join(out)


# ── verification ─────────────────────────────────────────────────────────────

def sgf_lines(text):
    """Root properties plus every line through the tree, as (colour, coord)."""
    i = text.index("(;") + 2
    props = {}
    while i < len(text):
        m = re.match(r"\s*([A-Z]+)((\[[^\]]*(?:\\.[^\]]*)*\])+)", text[i:])
        if not m:
            break
        vals = re.findall(r"\[([^\]]*(?:\\.[^\]]*)*)\]", m.group(2))
        props.setdefault(m.group(1), []).extend(vals)
        i += m.end()

    lines = []

    def walk(pos, prefix):
        cur = list(prefix)
        while pos < len(text):
            ch = text[pos]
            if ch.isspace():
                pos += 1
            elif ch == ")":
                if cur:
                    lines.append(list(cur))
                return pos + 1
            elif ch == "(":
                pos = walk(pos + 1, cur)
            elif ch == ";":
                m = re.match(r";\s*([BW])\[([a-z]{2})\]", text[pos:])
                if not m:
                    raise ValueError("bad node near %r" % text[pos:pos + 20])
                cur.append([m.group(1), m.group(2), False])
                pos += m.end()
                while pos < len(text):
                    m2 = re.match(r"\s*([A-Z]+)((\[[^\]]*(?:\\.[^\]]*)*\])+)",
                                  text[pos:])
                    if not m2:
                        break
                    if m2.group(1) == "C" and "RIGHT" in m2.group(2):
                        cur[-1][2] = True
                    pos += m2.end()
            else:
                raise ValueError("unexpected %r" % ch)
        return pos

    walk(i, [])
    return props, lines


def replay(board, line, first_is_black):
    """Play a line out under full rules; returns (final board, error or None)."""
    b = [row[:] for row in board]
    want = BLACK if first_is_black else WHITE
    for n, (colour, coord, _) in enumerate(line):
        r, c = parse_coord(coord)
        got = BLACK if colour == "B" else WHITE
        if got != want:
            return b, "move %d (%s) breaks alternation" % (n + 1, coord)
        try:
            b, _ = play_stone(b, r, c, got)
        except ValueError as exc:
            return b, "move %d (%s) is illegal: %s" % (n + 1, coord, exc)
        want = WHITE if want == BLACK else BLACK
    return b, None


# How one-sided a KataGo ownership reading has to be before it is allowed to
# contradict the tree. Ownership near zero means the engine is unsure, not that
# the group is half alive, and on these positions the exact solver is a proof
# where KataGo is an opinion — so only a decisive reading gets to raise a
# complaint. The mislabelled move that prompted this check read -0.96.
DECISIVE = 0.75


def verify_file(path, oracle=None, visits=4000):
    """Replay every line, then ask KataGo about each one marked correct."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    props, lines = sgf_lines(text)
    board = [[EMPTY] * BOARD for _ in range(BOARD)]
    for v in props.get("AB", []):
        r, c = parse_coord(v)
        board[r][c] = BLACK
    for v in props.get("AW", []):
        r, c = parse_coord(v)
        board[r][c] = WHITE
    first_is_black = props.get("PL", ["B"])[0].startswith("B")
    solver_is_black = props.get("ZS", props.get("PL", ["B"]))[0].startswith("B")

    name = (props.get("GN", [""])[0] or
            os.path.basename(path).rsplit(".", 1)[0]).upper()
    goal_is_kill = not (name.startswith("LIVE") or name.startswith("SAVE"))
    target_colour = (BLACK if not solver_is_black else WHITE) if goal_is_kill \
        else (BLACK if solver_is_black else WHITE)
    target = {(r, c) for r in range(BOARD) for c in range(BOARD)
              if board[r][c] == target_colour}

    problems, right = [], 0

    # A first move the drill presents as losing, but which actually works, is
    # the worst kind of error a drill can have: grinding it teaches that a
    # good move is bad, and nothing in a replay check notices, because the
    # line is *meant* to end without a verdict. So every first move with no
    # correct ending anywhere under it gets checked on its own.
    if oracle is not None:
        solver_moves_first = (first_is_black == solver_is_black)
        if solver_moves_first:
            works = {}
            for line in lines:
                if line:
                    works.setdefault(line[0][1], False)
                    if line[-1][2]:
                        works[line[0][1]] = True
            for coord, ok in sorted(works.items()):
                if ok:
                    continue
                r, c = parse_coord(coord)
                try:
                    b, _ = play_stone(board, r, c,
                                      BLACK if solver_is_black else WHITE)
                except ValueError:
                    continue
                alive_now = {p for p in target if b[p[0]][p[1]] == target_colour}
                if not alive_now:
                    problems.append("%s is shown as a mistake but captures "
                                    "the group outright" % coord)
                    continue
                res = oracle.ask(b, "B" if not solver_is_black else "W",
                                 alive_now, visits=visits)
                v = res["own"] if target_colour == BLACK else -res["own"]
                if goal_is_kill and v <= -DECISIVE:
                    problems.append("%s is shown as a mistake, but katago says "
                                    "it kills (%+.2f)" % (coord, v))
                # A defence drill asks for life, and a move that leaves the
                # group alive *in seki* is still not the answer — but ownership
                # cannot tell seki from life, since the stones survive either
                # way. So this direction only complains when the group ends up
                # decisively owned, and a near-certain reading here is worth a
                # look rather than proof of an error.
                if not goal_is_kill and v >= DECISIVE:
                    problems.append("%s is shown as a mistake, but katago says "
                                    "it lives (%+.2f) — check it is not just a "
                                    "seki" % (coord, v))

    for line in lines:
        b, err = replay(board, line, first_is_black)
        shown = " ".join(m[1] for m in line)
        if err:
            problems.append("%s: %s" % (shown, err))
            continue
        if not (line and line[-1][2]):
            continue
        right += 1
        if oracle is None:
            continue
        alive_now = {p for p in target if b[p[0]][p[1]] == target_colour}
        if not alive_now:
            if not goal_is_kill:
                problems.append("%s: the group being saved has been captured"
                                % shown)
            continue
        res = oracle.ask(b, "B" if not solver_is_black else "W",
                         alive_now, visits=visits)
        v = res["own"] if target_colour == BLACK else -res["own"]
        if goal_is_kill and v >= DECISIVE:
            problems.append("%s: claimed dead, katago says alive (%+.2f)"
                            % (shown, v))
        if not goal_is_kill and v <= -DECISIVE:
            problems.append("%s: claimed alive, katago says dead (%+.2f)"
                            % (shown, v))
    return {"lines": len(lines), "right": right, "problems": problems}


# ── KataGo, for the second opinion ───────────────────────────────────────────

class Oracle:
    """KataGo, asked only ever about group ownership.

    A corner shape on an otherwise empty board is not a real position — the
    engine reads the group running out into open space and ownership comes
    back as mush — so every query is walled off a few lines out, which is the
    context the shape is stated in. scoreLead is never consulted: on these
    boards it is dominated by whose framework the empty rest of the board is.
    """

    def __init__(self, verbose=False):
        here = os.path.dirname(os.path.abspath(__file__))
        sys.path.insert(0, here)
        import katago_annotate as ka
        self.ka = ka
        cfg = ka.read_config(os.path.dirname(here))
        for k in ("katago_exe", "katago_model", "katago_config"):
            if not cfg.get(k):
                raise RuntimeError("%s missing from apps/ogs_client/config.ini" % k)
        self.kata = ka.Kata(cfg["katago_exe"], cfg["katago_model"],
                            cfg["katago_config"], threads=8, verbose=verbose)
        self.n = 0

    def close(self):
        self.kata.close()

    def _gtp(self, r, c):
        return self.ka.xy_to_gtp(c, BOARD - 1 - r)

    def ask(self, board, to_move, target, box=7, visits=4000):
        attacker = WHITE if board[next(iter(target))[0]][
            next(iter(target))[1]] == BLACK else BLACK
        walled = [row[:] for row in board]
        for i in range(box + 2):
            for (r, c) in ((box, i), (i, box), (box + 1, i), (i, box + 1)):
                if r < BOARD and c < BOARD and walled[r][c] == EMPTY:
                    walled[r][c] = attacker
        black = [self._gtp(r, c) for r in range(BOARD) for c in range(BOARD)
                 if walled[r][c] == BLACK]
        white = [self._gtp(r, c) for r in range(BOARD) for c in range(BOARD)
                 if walled[r][c] == WHITE]
        inside = {self._gtp(r, c) for r in range(box) for c in range(box)}
        q = self.ka.make_query(black, white, [], inside, visits)
        self.n += 1
        q["id"] = "cd%d" % self.n
        q["initialPlayer"] = to_move
        self.kata.send(q)
        resp = self.kata.recv(timeout=900)
        verts = [self._gtp(r, c) for (r, c) in sorted(target)]
        return {"own": self.ka.own_of(resp, verts)}


# ── driver ───────────────────────────────────────────────────────────────────

def build_shape(shape, max_region=12, budget=80_000_000):
    """Solve one shape and check it against what Sensei's says. (info, error)."""
    board = shape.board()
    try:
        prob = Problem(board, shape.defender, shape.key)
    except ValueError as exc:
        return None, str(exc)
    issues = prob.validate(max_region=max_region)
    if issues:
        return None, "; ".join(issues)
    reg = Region(prob)
    solver = Solver(reg, node_budget=budget)
    t0 = time.time()
    att = run_deep(lambda: solver.solve(True))
    att_ko = solver.loop_seen
    dfn = run_deep(lambda: solver.solve(False))
    dfn_ko = solver.loop_seen
    # Each side is checked on its own, because a drill only depends on one of
    # them and there is no reason to throw away a sound defence drill because
    # the attacking side is a ko.
    conflict = {}
    for side, got, claimed, saw_loop in (
            ("attacker", att, shape.attacker_first, att_ko),
            ("defender", dfn, shape.defender_first, dfn_ko)):
        if claimed == KO:
            # The solver cannot return "ko" — it resolves repetition in the
            # defender's favour — so a ko claim is consistent with ALIVE or
            # SEKI. A clean DEAD is not: it says the defender has no ko to
            # fall back on, which contradicts the page and usually means the
            # enclosure took away what the real shape has. That only casts
            # doubt on the attacking side; the defence is unaffected.
            if got == DEAD:
                conflict[side] = ("%s-first: solver says a clean kill, "
                                  "sensei's says only a ko" % side)
        elif claimed == SEKI and got == ALIVE:
            # This one poisons both sides. Sensei's says the best the
            # defender gets is a seki and the solver says outright life, so
            # the two disagree about where the line between life and seki
            # falls in this shape — which is exactly the judgement a defence
            # drill's correct endings rest on.
            msg = ("solver says outright life where sensei's says seki, "
                   "so the two disagree about this shape")
            conflict["attacker"] = conflict["defender"] = msg
        elif got != claimed:
            conflict[side] = ("%s-first: solver says %s, sensei's says %s"
                              % (side, got, claimed))
        elif saw_loop:
            conflict[side] = ("%s-first: the verdict leans on a repetition, "
                              "so the shape is really a ko" % side)
    return {"problem": prob, "region": reg, "solver": solver,
            "attacker_first": att, "defender_first": dfn,
            "secs": time.time() - t0, "conflict": conflict,
            "points": len(prob.region), "libs": len(prob.liberties())}, None


# Which side's status each kind of drill actually rests on.
DEPENDS_ON = {KILL: "attacker", REFUTE: "attacker",
              SAVE: "attacker", LIVE: "defender"}


def report_checks(paths):
    """Replay and KataGo-check a list of drill files. Returns an exit code."""
    oracle = None
    try:
        oracle = Oracle()
    except Exception as exc:
        print("KataGo unavailable (%s) — replay check only" % exc)
    bad = 0
    for path in paths:
        try:
            res = verify_file(path, oracle)
        except Exception as exc:
            print("  %-48s could not be read: %s" % (os.path.basename(path), exc))
            bad += 1
            continue
        print("  %-48s %3d lines, %2d correct  %s"
              % (os.path.basename(path), res["lines"], res["right"],
                 "ok" if not res["problems"] else "PROBLEMS"))
        for p in res["problems"]:
            bad += 1
            print("        %s" % p)
    if oracle:
        oracle.close()
    print("  all clear" if not bad else "  %d problem(s)" % bad)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="", help="directory to write drills into")
    ap.add_argument("--list", action="store_true",
                    help="show the shapes and their positions, write nothing")
    ap.add_argument("--only", default="", help="comma-separated shape keys")
    ap.add_argument("--force", action="store_true",
                    help="overwrite drills that are already there")
    ap.add_argument("--verify", action="store_true",
                    help="replay every line, then cross-check with KataGo")
    ap.add_argument("--check", default="",
                    help="check the drills already in this directory and stop "
                         "(works on hand-made ones too)")
    ap.add_argument("--max-region", type=int, default=12,
                    help="largest eye space to attempt; cost climbs steeply "
                         "with it (a ten-point region takes minutes, and the "
                         "L+2 group is the one shape here that can exhaust "
                         "the node budget and come back UNKNOWN)")
    ap.add_argument("--max-nodes", type=int, default=400,
                    help="soft cap on drill tree size")
    ap.add_argument("--replies", type=int, default=2,
                    help="opponent tries kept per turn below the first move; "
                         "1 gives leaner drills, 3 covers more resistance")
    opt = ap.parse_args()

    shapes = SHAPES
    if opt.only:
        want = {k.strip() for k in opt.only.split(",")}
        shapes = [s for s in SHAPES if s.key in want]
        missing = want - {s.key for s in shapes}
        if missing:
            sys.exit("unknown shape(s): %s" % ", ".join(sorted(missing)))

    if opt.list:
        for s in shapes:
            print("=" * 70)
            print("%s  [%s]" % (s.name, s.key))
            print("  %s" % s.source)
            print("  sensei's: %s with the attacker first, %s with the defender"
                  % (s.attacker_first, s.defender_first))
            if s.note:
                print("  %s" % s.note)
            print("  drills: %s" % (", ".join(s.drills) or "none"))
            print(render(s.board()))
        return 0

    if opt.check:
        paths = [os.path.join(opt.check, f) for f in sorted(os.listdir(opt.check))
                 if f.lower().endswith(".sgf")]
        if not paths:
            sys.exit("no SGFs in " + opt.check)
        return report_checks(paths)

    if not opt.out:
        sys.exit("--out is required (or use --list or --check)")
    os.makedirs(opt.out, exist_ok=True)

    written, skipped = [], []
    for shape in shapes:
        print("=" * 70)
        print("%s  — %s" % (shape.name, shape.note or shape.source))
        info, err = build_shape(shape, opt.max_region)
        if err:
            print("  SKIPPED: %s" % err)
            skipped.append((shape.name, err))
            continue
        print("  eye space %d points, %d liberties | solved in %.0fs: "
              "attacker-first %s, defender-first %s"
              % (info["points"], info["libs"], info["secs"],
                 info["attacker_first"], info["defender_first"]))
        for side, msg in sorted(info["conflict"].items()):
            print("  conflict — %s" % msg)
        # Which drills to write follows the *solved* status, not the claimed
        # one, so a drill always matches the position actually on the board.
        # Where the two agree this changes nothing; where Sensei's says ko and
        # the solver finds outright life, it stops a "make life" drill being
        # written for a group that cannot die whatever you play.
        solved = Shape(shape.key, shape.name, shape.rows,
                       info["attacker_first"], info["defender_first"],
                       shape.source, defender=shape.defender)
        kinds = [k for k in solved.drills
                 if k in shape.drills and DEPENDS_ON[k] not in info["conflict"]]
        for k in shape.drills:
            if k in kinds:
                continue
            why = info["conflict"].get(
                DEPENDS_ON[k],
                "the solved status (%s / %s) does not pose this question"
                % (info["attacker_first"], info["defender_first"]))
            print("  %-6s skipped: %s" % (k, why))
            skipped.append(("%s (%s)" % (shape.name, k), why))
        if not kinds:
            continue
        for kind in kinds:
            tb = TreeBuilder(info["region"], info["solver"], kind,
                             max_nodes=opt.max_nodes,
                             deep_replies=opt.replies)
            root, terr = run_deep(tb.build)
            if terr:
                print("  %-6s FAILED: %s" % (kind, terr))
                skipped.append(("%s (%s)" % (shape.name, kind), terr))
                continue
            path = os.path.join(opt.out, drill_filename(shape, kind))
            if os.path.exists(path) and not opt.force:
                print("  %-6s kept: %s is already there"
                      % (kind, os.path.basename(path)))
                continue
            with open(path, "w", encoding="utf-8") as f:
                f.write(to_sgf(info["region"], root, kind, shape))
            print("  %-6s %-46s %3d nodes, depth %d%s"
                  % (kind, os.path.basename(path), count_nodes(root) - 1,
                     tree_depth(root) - 1,
                     "  (truncated)" if tb.truncated else ""))
            written.append(path)

    print("=" * 70)
    print("%d drill(s) written, %d shape(s) skipped" % (len(written), len(skipped)))
    for name, why in skipped:
        print("   skipped %-38s %s" % (name, why))

    if opt.verify and written:
        print("=" * 70)
        print("verifying (replay under Go rules, then KataGo)...")
        return report_checks(written)
    return 0


if __name__ == "__main__":
    sys.exit(main())
