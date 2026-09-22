# go_station

Play, analyze, and study Go with a PlayStation controller.

![Live game in progress — clocks, prisoner count, and turn indicator](screenshot0.png)
![ogs_client analysis mode — move tree, KataGo score graph, and live suggestions](screenshot1.png)
![Same analysis view with board coordinates enabled](screenshot2.png)
![Reviewing a professional game (Shusaku vs Ota Yuzo) in analysis mode](screenshot3.png)
![Game catalog — browse past games with opening/final position thumbnails](screenshot4.png)

## Setup

1. Build `ogs_client` — see [Building](#building) below.
2. Copy `apps/ogs_client/config.ini.example` to `apps/ogs_client/config.ini`.
3. Add your OGS login: fill in `username=` and `password=`. The client logs in
   the same way the website does; nothing else is needed. (You can also leave
   them blank and enter them in the app on first launch.) Accounts that signed
   up via Google/Facebook and never set an OGS password aren't supported.
4. *(Optional)* KataGo analysis — point `katago_exe` / `katago_model` /
   `katago_config` at a local KataGo install; see the comments in
   `config.ini.example` for the exact fields and a model download link.
5. Run `ogs_client.bat` at the repo root (or `apps/ogs_client/ogs_client.exe`
   directly).

`go_viewer` — the pro-game library browser — needs no setup beyond building
it; just run `go_viewer.bat`.

## What's here

This is a small monorepo: a shared Go engine + SDL renderer (`engine/`), and
two apps built on it (`apps/`).

| Path | What it is |
|------|------------|
| `engine/` | Shared Go rules, game state, SGF catalog browser, SDL renderer |
| `apps/go_viewer/` | Standalone viewer for the pro game library in `games/` |
| `apps/ogs_client/` | Online-go.com client — live play, puzzles, local KataGo analysis |
| `games/` | Curated professional game library (SGF) |
| `my_games/` | My own OGS game history — kept for reference, not example data |
| `problem_sets/` | Problem books imported from PDFs, one folder per book |
| `tools/` | Packaging and maintenance scripts |

## Building

`engine/` and `apps/go_viewer/` only need SDL2. `apps/ogs_client/` additionally
needs libcurl and libwebsockets, and a `config.ini` (see [Setup](#setup)
above).

### Linux — direct g++, no CMake (recommended)

```bash
# Arch
sudo pacman -S sdl2 curl libwebsockets
# Debian/Ubuntu
sudo apt install libsdl2-dev libcurl4-openssl-dev libwebsockets-dev
# Fedora
sudo dnf install SDL2-devel libcurl-devel libwebsockets-devel

chmod +x build_linux.sh   # first time only
./build_linux.sh
```
Builds both apps in one pass: `apps/go_viewer/go_viewer` and
`apps/ogs_client/ogs_client`. Plain g++ invocations, same idea as the CMake
sections below but without CMake in the loop. `apps/ogs_client/build.sh`
builds just that one app the same way, if that's all you need.

### go_viewer — Linux, via CMake (Arch / Debian / Fedora)

**Arch Linux**
```bash
sudo pacman -S sdl2 cmake base-devel
cmake -B build && cmake --build build --target go_viewer
./build/apps/go_viewer/go_viewer
```

**Debian / Ubuntu**
```bash
sudo apt install libsdl2-dev cmake build-essential
cmake -B build && cmake --build build --target go_viewer
./build/apps/go_viewer/go_viewer
```

**Fedora**
```bash
sudo dnf install SDL2-devel cmake gcc-c++
cmake -B build && cmake --build build --target go_viewer
./build/apps/go_viewer/go_viewer
```

### go_viewer — macOS (Homebrew)

```bash
brew install sdl2 cmake
cmake -B build && cmake --build build --target go_viewer
./build/apps/go_viewer/go_viewer
```

### go_viewer — Windows (MSYS2 / MinGW64, direct g++)

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 MinGW64** shell.
2. Install dependencies:
```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-pkg-config
```
3. From the repo root, build (direct g++ invocation is more reliable than CMake on MSYS2):
```bash
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
export TEMP="/tmp" TMP="/tmp"
g++ -std=c++17 -O2 $(pkg-config --cflags sdl2) -Iengine \
    apps/go_viewer/main.cpp \
    engine/go_rules.cpp engine/game_state.cpp engine/analysis_state.cpp \
    engine/catalog.cpp engine/game_index.cpp engine/renderer.cpp \
    $(pkg-config --libs sdl2) -o go_viewer.exe
```
4. Copy `SDL2.dll` next to the executable so it can run outside the MSYS2 shell:
```bash
cp /c/msys64/mingw64/bin/SDL2.dll .
```
5. Run: `./go_viewer.exe` (from the repo root — see note below)

### Everything, via CMake (Windows / MinGW)

```powershell
cmake -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=C:/msys64/mingw64
cmake --build build
```
Builds both apps in one pass. `ogs_client` additionally needs
`pacman -S mingw-w64-x86_64-curl mingw-w64-x86_64-libwebsockets`. Executables
land in `build/apps/<name>/` and are copied back next to their source
directories — `apps/go_viewer/go_viewer.exe`, `apps/ogs_client/ogs_client.exe`
— which is what `go_viewer.bat` / `ogs_client.bat` launch.

## go_viewer

### Usage

1. Place `.sgf` files in a `games/` folder at the repo root.
   You can organise them in subdirectories — the catalog browser handles them.
2. Run the executable **with your terminal's working directory set to the repo
   root** — go_viewer looks for `games/` relative to the current directory, not
   the executable's own location. It picks a random game on startup.
3. Press **ESC** at any time to toggle the in-app help overlay.

### Controls

| Key | Action |
|-----|--------|
| **Q** | Quit |
| **N** | Next game |
| **R** | Restart current game |
| **C** | Open catalog browser |
| **ESC** | Toggle help overlay |
| **Up / Down** | Faster / slower auto-playback |
| **Left / Right** | Step back / forward one move |

#### Modes

| Key | Mode |
|-----|------|
| **Space / A** | Analysis mode — place and remove stones freely |
| **G** | Guess mode — predict each move; scored ±1 per guess |
| **P** | Play mode — two players on an empty board |
| **T** | Territory drill — estimate which marked territory is larger |
| **U** | Toggle chain-connection lines |

#### Analysis mode
- **Left-click empty** — place a stone (alternates black/white)
- **Hold B / W** while clicking — force black or white
- **Left-click stone** — show / hide its chain's liberties
- **Right-click stone** — remove it

#### Catalog browser
- **Up / Down** — navigate; **Enter** — open; **ESC** — close

## ogs_client

An online-go.com client with local play and analysis against KataGo —
automatch, live play, stone-removal scoring, an OGS puzzle player, adaptive
practice opponents, batch autoplay through a game folder, and more — built on
the same engine as go_viewer above. See [Setup](#setup) above for connecting
your OGS account. This app in particular has grown around my own day-to-day
usage rather than a fixed feature set, so this README is the full extent of
its onboarding — a PlayStation controller is assumed for the on-screen
control hints, though mouse and keyboard work throughout as well.

### Importing a problem book from a PDF

Go problem books typeset in LaTeX with the `igo` package draw their diagrams
with a *font* — one glyph per intersection — so the position is real text in
the PDF rather than a picture of a board. That makes a book directly
importable: every stone reads back exactly, and the answer key at the back
("(1) B1 D1 D2 H1 C1") gives the solution move for move.

```bash
pip install pymupdf
python tools/import_problem_pdf.py common-life-and-death-problems.pdf \
       --name "Common Life & Death" --rank 5
```

Each problem becomes one SGF in `problem_sets/<slug>/` — the diagram's corner
crop placed on a full 19×19 board, with the answer as a judged solution line.
Every problem is replayed under full Go rules before it is written; anything
that doesn't replay (books do clip the occasional diagram, cutting a group off
mid-shape) is reported and skipped rather than shipped broken. Add `--dry-run`
to check a PDF without writing, `--report` to list what was skipped and why.

The set then appears in the puzzle browser as its own pinned collection,
above the OGS ones and alongside `[MY DRILLS]`, with per-problem solved
tracking and the same progress colouring as an OGS collection. Nothing about
it is specific to one book — point the tool at another `igo`-typeset PDF and
it becomes a second collection.

### Sharpening an imported set with KataGo

A printed book gives one line per problem, so a wrong guess has nowhere to go,
and the book's own moves are not always the best ones. `katago_annotate.py`
fixes both without discarding what the book knows:

```bash
python tools/katago_annotate.py problem_sets/common_life_death --jobs 4
```

It grows refutation branches off every solver turn — a plausible losing move
gets answered and explained instead of dropping you into free play — and
flags book moves that give away a point or more. Roughly 80 minutes for a
370-problem set on a mid-range GPU; SGFs are written as each problem finishes,
and re-running re-derives from the book's main line, so an interrupted batch
is safe to repeat.

What it deliberately does *not* do is re-solve the problems. These diagrams
are corner crops, so the enclosing wall is only watertight relative to the
printed frame; on a real 19×19 the group can run out through a gap the book
never had to draw. Left to itself the engine finds that escape, scores it
well, and "corrects" a perfectly good answer. Three things keep it honest:

- **Search wide, judge narrow.** Analysis runs over the problem dilated by a
  ring (~50 points) so replies aren't banned and lines score correctly, but
  candidates are filtered to the actual fight — the target group's liberties
  and those of the white groups pressing on it — before anything is flagged.
- **Never trust one search.** Point advice is re-measured at `--verify-visits`
  (8000 by default) before it is written. Checked against a 1200-visit pass,
  about a third of flags either shrank to nothing or reversed outright.
- **Ignore winrate.** On an otherwise empty board it is pinned at 0.000. Only
  `scoreLead` differences and group ownership carry any signal.

### Drills for the standard corner shapes

The corner shapes every player is told to memorise — the L group, the L+1 and
L+2 groups, the bulky five, the rectangular six — make good drills, and typing
them in by hand is both tedious and a good way to memorise a wrong answer.

```bash
python tools/corner_drills.py --out my_games/<you>/drills --verify
```

Each shape is sealed into a self-contained corner problem, solved exactly, and
only then turned into a drill. The solver is a full search of the sealed
region under Go rules rather than an engine: for eye spaces of this size that
is cheap enough to be a proof instead of an opinion, so every move offered as
correct provably reaches the goal, and every opponent reply in the tree
provably has an answer. `--list` prints the positions and their sources;
`--verify` replays every line under the rules and then has KataGo re-judge
each one independently. Existing files are never overwritten without
`--force`.

Two things stop it shipping something untrue:

- **The positions are sealed, not cropped.** Sensei's draws the open
  formations with their escape routes open and a couple of distant stones
  standing in for "white is strong outside" — fine for a diagram, useless for
  a drill. Sealing only ever continues the wall the diagram already draws, so
  the group ends up with the same room or more, and a shape whose openness is
  not a wall one stone short of the edge is skipped rather than altered. The
  tripod group is the clear case: its eye space genuinely runs out into the
  open corner, so any enclosure of it is a different problem.
- **The solver's answer is checked against Sensei's.** Every shape carries the
  status the page states. If the solved status disagrees, the affected side is
  reported and no drill is written for it, because a disagreement means either
  the enclosure changed the shape or one of the two is wrong. Shapes whose
  best line is a ko or a seki produce only the side that has a clean answer —
  there is no move that is simply "right" in a ko, and the drill format has
  nowhere to put one.

`--check <dir>` runs the same two checks over drills that are already there,
hand-made ones included. Besides replaying every line it re-judges each first
move the drill presents as a *mistake*, which is the one error a replay can
never catch: a line that is meant to end without a verdict looks fine however
wrong it is, so a good move filed under "wrong" would otherwise sit there
teaching the opposite of the truth.

`--replies` controls how much opponent resistance each turn carries (2 by
default); dropping it to 1 gives shorter drills, raising it covers more.

Cost climbs steeply with the size of the eye space: six or seven points solve
instantly, nine takes seconds, ten or eleven takes minutes. The L+2 group sits
right at the edge and can exhaust the node budget, in which case it is
reported as `UNKNOWN` and skipped rather than guessed at. Running one shape at
a time (`--only <key>`) keeps the work already done if that happens.

## Source files

| File | Purpose |
|------|---------|
| `engine/go_viewer.hpp` | Shared constants and types |
| `engine/go_rules.cpp/hpp` | Go rules (capture, liberty counting, suicide check) |
| `engine/game_state.cpp/hpp` | Game state and snapshot history |
| `engine/analysis_state.cpp/hpp` | Free-placement analysis board |
| `engine/catalog.cpp/hpp` | SGF file browser |
| `engine/renderer.cpp/hpp` | All SDL2 rendering |
| `engine/CMakeLists.txt` | Builds the shared `go_engine` static library |
| `apps/go_viewer/main.cpp` | App loop, input handling, drill logic |
| `apps/go_viewer/CMakeLists.txt` | Builds the `go_viewer` executable |
| `apps/ogs_client/` | OGS networking, KataGo integration, puzzles, sound |
| `tools/import_problem_pdf.py` | Imports a LaTeX/`igo` problem-book PDF into `problem_sets/` |
| `tools/katago_annotate.py` | Adds KataGo refutation branches and point notes to an imported set |
| `tools/corner_drills.py` | Solves the standard corner shapes exactly and writes them as drills |
| `CMakeLists.txt` (repo root) | Top-level build — wires up `engine/` and both `apps/` |
