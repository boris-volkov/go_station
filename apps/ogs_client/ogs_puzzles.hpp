#pragma once
#include <string>
#include <vector>
#include <memory>

// ── OGS puzzle REST API (read-only, public — no auth required) ────────────────
//
// Endpoints (verified live 2026-07):
//   GET /api/v1/puzzles/collections/?page=N&page_size=N   → paginated collections
//   GET /api/v1/puzzles/{id}                              → full puzzle + solution tree
//   GET /api/v1/puzzles/{id}/collection_summary           → ordered ids in same collection
//
// All fetchers are blocking (libcurl) — call from a worker thread, not the UI loop.

// A board annotation the author placed at this node — letter labels ("A", "X",
// "O"…) or shape marks reduced to a display character.
struct PuzzleMark {
    int  x = -1, y = -1;
    char ch = '?';
};

// One node of the authored solution tree. Root is the initial position (x=y=-1);
// each child is a move. correct/wrong flags mark judged endpoints; text carries
// the author's comment for that node (often "why this fails" on wrong branches).
struct PuzzleMoveNode {
    int  x = -1, y = -1;
    bool correct = false;
    bool wrong   = false;
    std::string text;
    std::vector<PuzzleMark>     marks;   // annotations to show while at this node
    std::vector<PuzzleMoveNode> branches;
};

struct OgsPuzzle {
    int         id     = 0;
    std::string name;
    std::string description;
    std::string type;            // "life_and_death", "tsumego", …
    int         width  = 19, height = 19;
    int         rank   = 0;      // author-assigned difficulty (0 = unrated)
    // Concatenated 2-char SGF coordinates ("dadbdc…"), same encoding as AB[]/AW[]
    std::string initial_black;
    std::string initial_white;
    bool        black_to_play = true;
    // Which color the human plays. Usually == black_to_play; a drill can decouple
    // the two (e.g. black moves first per the position, but white is the solver),
    // in which case pz_start() auto-plays black's opening move before handing over.
    bool        solver_black  = true;
    bool        opponent_auto = true;   // opponent replies play automatically from the tree
    int         collection_id = 0;
    std::string collection_name;
    PuzzleMoveNode tree;                // root: x=y=-1, branches = first player moves
};

struct OgsPuzzleCollection {
    int         id = 0;
    std::string name;
    std::string owner;
    int         puzzle_count = 0;
    int         min_rank = 0, max_rank = 0;
    float       rating = 0.f;
    int         rating_count = 0;
    int         starting_puzzle_id = 0;
};

// Fetch one page of public collections. total_out receives the total collection
// count (for "page X of Y"). Returns false on network/parse failure.
bool ogs_fetch_puzzle_collections(int page, int page_size,
                                  std::vector<OgsPuzzleCollection>& out,
                                  int& total_out);

// Fetch a single puzzle with its solution tree.
bool ogs_fetch_puzzle(int puzzle_id, OgsPuzzle& out);

// Ordered {id, name} of every puzzle in the same collection as puzzle_id —
// drives next/previous navigation while solving.
bool ogs_fetch_collection_puzzles(int puzzle_id,
                                  std::vector<std::pair<int, std::string>>& out);

// ── goproblems.com download (local files, no network) ────────────────────────
//
// A folder of <id>.sgf files from goproblems.com, each carrying its rank in
// DI[] and genre in GE[]. tools/goproblems_index.py writes the folder's
// goproblems_index.tsv (easiest first), which the app browses; each problem's
// SGF is only parsed when it's opened.
//
// goproblems ids share the app's puzzle-id space (solved_puzzles.txt, saved
// missed/favorite copies) offset by GP_ID_BASE, clear of real OGS ids and of
// the imported books at 1e9 + slot * 1e6. Every goproblems solve is mapped to
// the one synthetic GP_COLLECTION_ID, so the browser pin shows overall progress.
constexpr int GP_ID_BASE       = 2000000000;
constexpr int GP_COLLECTION_ID = 1999999999;

struct GpIndexEntry {
    int         gp_id = 0;
    int         level = 0;     // same scale as OgsPuzzle::rank: 29 = 1k, 30 = 1d
    std::string genre;
};

// Load the index (already sorted easiest-first by the builder). False if the
// file is missing or holds no rows.
bool gp_load_index(const std::string& path, std::vector<GpIndexEntry>& out);

// Load problem gp_id from its SGF at path (out.id = GP_ID_BASE + gp_id). On
// failure err says why — unreadable, or an SGF the solver can't use.
bool gp_load_problem(const std::string& path, int gp_id, OgsPuzzle& out, std::string& err);

// ── OGS Joseki Explorer ("OJE") — https://online-go.com/oje/ ─────────────────
//
// A public position graph walked one node at a time: fetch a node, get its
// commentary plus every known continuation with a quality category. Sequences
// are stored in canonical orientation (first move in the top-right corner,
// Black first). Node ids are strings ("root" or a numeric id).

struct JosekiNextMove {
    std::string placement;        // pretty coordinate ("Q16"), or "pass"
    std::string category;         // IDEAL / GOOD / MISTAKE / TRICK / QUESTION
    std::string variation_label;  // "1".."9" main lines, "_" unlabelled
    std::string node_id;
};

struct JosekiPosition {
    std::string node_id;
    std::string placement;    // move that created this node ("root" at the top)
    std::string category;
    std::string description;  // markdown commentary
    std::string source_desc;  // joseki_source description ("" if none)
    int         child_count = 0;
    std::vector<JosekiNextMove> next_moves;
};

// Fetch one OJE node. node_id: "root" or a numeric id string.
bool ogs_fetch_joseki(const std::string& node_id, JosekiPosition& out);
