#include "go_viewer.hpp"
#include "go_rules.hpp"
#include "game_state.hpp"
#include "analysis_state.hpp"
#include "catalog.hpp"
#include "renderer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif


// ---------------------------------------------------------------------------
// SGF parser (self-contained, no global state)

struct SgfGame {
    char  moves[MAX_MOVES][MOVE_TEXT_LEN] = {};
    int   colors[MAX_MOVES]               = {};
    int   move_count                      = 0;
    char  black_name[NAME_LEN]            = "Black";
    char  white_name[NAME_LEN]            = "White";
    char  result[RESULT_LEN]              = {};
    char  date[32]                        = {};
    // Root-level comment C[] (game annotation or saved-position note)
    char  comment[1024]                   = {};
    // Setup position from AB[]/AW[] properties (used by saved-position SGFs)
    char  initial_board[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    int   initial_turn_is_black                 = 1;  // from PL[]
    bool  has_setup                             = false;
};

static bool parse_sgf_move(const char* move_str, int& out_r, int& out_f) {
    if (!move_str || strlen(move_str) != 2) return false;
    char fc = move_str[0], rc = move_str[1];
    if (fc < 'a' || fc > 's' || rc < 'a' || rc > 's') return false;
    out_f = fc - 'a';
    out_r = rc - 'a';
    return true;
}

static bool load_sgf(const std::string& path, SgfGame& g) {
    g.move_count            = 0;
    g.black_name[0]         = '\0';
    g.white_name[0]         = '\0';
    g.result[0]             = '\0';
    g.date[0]               = '\0';
    g.comment[0]            = '\0';
    g.has_setup             = false;
    g.initial_turn_is_black = 1;
    memset(g.initial_board, 0, sizeof(g.initial_board));

    // Read the whole file at once so we can track parenthesis depth properly.
    // This is necessary to skip variation branches (depth >= 2) which would
    // otherwise be mixed into the main move sequence and cause playback freezes
    // when a variation move tries to land on an already-occupied intersection.
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    rewind(fp);
    if (fsz <= 0 || fsz > 8 * 1024 * 1024) { fclose(fp); return false; }
    std::vector<char> buf((size_t)fsz + 1);
    size_t nread = fread(buf.data(), 1, (size_t)fsz, fp);
    fclose(fp);
    buf[nread] = '\0';

    // Helper: read a property value starting at src (pointing just past '['),
    // write into dst (null-terminated, trimmed), return pointer to char after ']'.
    auto read_val = [](const char* src, char* dst, size_t dsz) -> const char* {
        size_t i = 0;
        while (*src && *src != ']') {
            if (*src == '\\') { src++; if (!*src) break; }
            if (i < dsz - 1) dst[i++] = *src;
            src++;
        }
        while (i > 0 && (dst[i-1]==' '||dst[i-1]=='\t'||dst[i-1]=='\r'||dst[i-1]=='\n')) i--;
        dst[i] = '\0';
        return (*src == ']') ? src + 1 : src;
    };

    const char* p = buf.data();
    int  depth     = 0;     // ( / ) nesting; 1 = main game line, 2+ = variation
    bool in_prop   = false; // currently inside [...] property value
    bool prev_alpha= false; // previous non-space char was a letter (detects multi-char prop names)

    while (*p) {
        // --- Inside a property value: advance until ']', respecting '\' escapes ---
        if (in_prop) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == ']')  { in_prop = false; }
            if (*p && !isspace((unsigned char)*p)) prev_alpha = (isalpha((unsigned char)*p) != 0);
            p++;
            continue;
        }

        // --- Structural characters (only valid outside property values) ---
        if (*p == '(') { depth++; prev_alpha = false; p++; continue; }
        if (*p == ')') { depth--; prev_alpha = false; p++; continue; }
        // Unknown/unhandled property: consume value so we don't mistake its content
        if (*p == '[') { in_prop = true; prev_alpha = false; p++; continue; }

        // --- Only parse game content at depth 1 (main game sequence) ---
        if (depth != 1) {
            if (*p && !isspace((unsigned char)*p)) prev_alpha = (isalpha((unsigned char)*p) != 0);
            p++;
            continue;
        }

        // ---- Main game line (depth == 1) ----

        // B[move] — black move.  Guard !prev_alpha avoids matching the 'B' in "PB[", "AB[", etc.
        if (*p == 'B' && *(p+1) == '[' && !prev_alpha) {
            p += 2;
            const char* start = p;
            while (*p && *p != ']') { if (*p == '\\') p++; if (*p) p++; }
            if (g.move_count < MAX_MOVES) {
                size_t len = (size_t)(p - start);
                if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                memcpy(g.moves[g.move_count], start, len);
                g.moves[g.move_count][len] = '\0';
                g.colors[g.move_count] = 1;
                g.move_count++;
            }
            if (*p == ']') p++;
            prev_alpha = false;
            continue;
        }

        // W[move] — white move
        if (*p == 'W' && *(p+1) == '[' && !prev_alpha) {
            p += 2;
            const char* start = p;
            while (*p && *p != ']') { if (*p == '\\') p++; if (*p) p++; }
            if (g.move_count < MAX_MOVES) {
                size_t len = (size_t)(p - start);
                if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                memcpy(g.moves[g.move_count], start, len);
                g.moves[g.move_count][len] = '\0';
                g.colors[g.move_count] = 0;
                g.move_count++;
            }
            if (*p == ']') p++;
            prev_alpha = false;
            continue;
        }

        // AB[xx][xx]... — add black setup stones (multi-value)
        if (*p == 'A' && *(p+1) == 'B') {
            p += 2;
            while (*p == '[') {
                p++;
                if (p[0] >= 'a' && p[0] <= 's' && p[1] >= 'a' && p[1] <= 's') {
                    g.initial_board[p[1]-'a'][p[0]-'a'] = 1;
                    g.has_setup = true;
                }
                while (*p && *p != ']') { if (*p == '\\') p++; if (*p) p++; }
                if (*p == ']') p++;
            }
            prev_alpha = false;
            continue;
        }

        // AW[xx][xx]... — add white setup stones (multi-value)
        if (*p == 'A' && *(p+1) == 'W') {
            p += 2;
            while (*p == '[') {
                p++;
                if (p[0] >= 'a' && p[0] <= 's' && p[1] >= 'a' && p[1] <= 's') {
                    g.initial_board[p[1]-'a'][p[0]-'a'] = 2;
                    g.has_setup = true;
                }
                while (*p && *p != ']') { if (*p == '\\') p++; if (*p) p++; }
                if (*p == ']') p++;
            }
            prev_alpha = false;
            continue;
        }

        // PL[B/W] — initial player to move
        if (*p == 'P' && *(p+1) == 'L' && *(p+2) == '[') {
            p += 3;
            g.initial_turn_is_black = (*p == 'B' || *p == 'b') ? 1 : 0;
            in_prop = true; prev_alpha = false; p++; continue;
        }

        // PB[...] — player black name
        if (*p == 'P' && *(p+1) == 'B' && *(p+2) == '[') {
            p += 3; p = read_val(p, g.black_name, sizeof(g.black_name));
            prev_alpha = false; continue;
        }

        // PW[...] — player white name
        if (*p == 'P' && *(p+1) == 'W' && *(p+2) == '[') {
            p += 3; p = read_val(p, g.white_name, sizeof(g.white_name));
            prev_alpha = false; continue;
        }

        // RE[...] — result
        if (*p == 'R' && *(p+1) == 'E' && *(p+2) == '[') {
            p += 3; p = read_val(p, g.result, sizeof(g.result));
            prev_alpha = false; continue;
        }

        // DT[...] — date
        if (*p == 'D' && *(p+1) == 'T' && *(p+2) == '[') {
            p += 3; p = read_val(p, g.date, sizeof(g.date));
            prev_alpha = false; continue;
        }

        // C[...] — root comment (only before any moves; !prev_alpha avoids CA[, GC[, etc.)
        if (*p == 'C' && *(p+1) == '[' && !prev_alpha && g.move_count == 0) {
            p += 2;
            size_t ci = 0; bool esc = false;
            while (*p && ci < sizeof(g.comment) - 1) {
                if (esc)          { g.comment[ci++] = *p++; esc = false; }
                else if (*p=='\\') { esc = true; p++; }
                else if (*p==']')  { p++; break; }
                else               { g.comment[ci++] = *p++; }
            }
            g.comment[ci] = '\0';
            prev_alpha = false; continue;
        }

        // Default: update prev_alpha and advance
        if (!isspace((unsigned char)*p)) prev_alpha = (isalpha((unsigned char)*p) != 0);
        p++;
    }

    if (g.black_name[0] == '\0') strcpy(g.black_name, "Black");
    if (g.white_name[0] == '\0') strcpy(g.white_name, "White");

    return g.move_count > 0 || g.has_setup;
}

// Play up to max_moves moves from an SGF and write the resulting board.
// max_moves = -1 means play the whole game.
// Pure CPU — no SDL, no GameState overhead.  Used for catalog thumbnails.
static bool sgf_board_at(const std::string& path,
                          char board_out[BOARD_SIZE][BOARD_SIZE],
                          int max_moves = -1) {
    SgfGame g;
    if (!load_sgf(path, g)) return false;
    int limit = (max_moves < 0 || max_moves > g.move_count) ? g.move_count : max_moves;
    char board[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    for (int i = 0; i < limit; i++) {
        int r, f;
        if (!parse_sgf_move(g.moves[i], r, f)) continue;
        if (board[r][f] != 0) continue;
        int is_black = (g.colors[i] == 1);
        if (GoRules::would_be_suicide(board, r, f, is_black)) continue;
        board[r][f] = is_black ? 1 : 2;
        int cap_r[BOARD_SIZE * BOARD_SIZE], cap_f[BOARD_SIZE * BOARD_SIZE], cap_count = 0;
        GoRules::find_captured(board, is_black, r, f, cap_r, cap_f, cap_count);
        for (int j = 0; j < cap_count; j++) board[cap_r[j]][cap_f[j]] = 0;
    }
    // Copy only the 19×19 portion — board_out has BOARD_SIZE stride, board has
    // MAX_BOARD_SIZE stride, so a single memcpy would be wrong on both counts.
    // SGF moves never exceed column/row 18 so the rest is always empty.
    for (int r = 0; r < BOARD_SIZE; r++)
        memcpy(board_out[r], board[r], BOARD_SIZE);
    return true;
}

static constexpr int THUMB_OPENING_MOVES = 16;

// ---------------------------------------------------------------------------
// Territory estimation drill

struct TerritoryProblem {
    char board[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    int  black_score = 0;
    int  white_score = 0;
    bool answered    = false;
    bool correct     = false;
};

// shape: 0=compact, 1=horizontal strip, 2=vertical strip
static void grow_blob(int r0, int c0, int r1, int c1, int target,
                      bool out[BOARD_SIZE][BOARD_SIZE], int shape = 0) {
    memset(out, 0, BOARD_SIZE * BOARD_SIZE * sizeof(bool));
    int sr = r0 + rand() % (r1 - r0 + 1);
    int sc = c0 + rand() % (c1 - c0 + 1);
    std::vector<std::pair<int,int>> cells;
    out[sr][sc] = true;
    cells.push_back({sr, sc});
    // dirs: up, down, left, right — weights vary by shape
    const int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    const int v_weight = (shape == 2) ? 6 : 1;  // up/down
    const int h_weight = (shape == 1) ? 6 : 1;  // left/right
    const int total_w  = 2*v_weight + 2*h_weight;
    for (int attempts = 0; (int)cells.size() < target && attempts < target * 100; attempts++) {
        auto& cell = cells[rand() % cells.size()];
        int rw = rand() % total_w;
        int d = (rw < v_weight) ? 0 : (rw < 2*v_weight) ? 1 : (rw < 2*v_weight + h_weight) ? 2 : 3;
        int nr = cell.first + dirs[d][0], nc = cell.second + dirs[d][1];
        if (nr >= r0 && nr <= r1 && nc >= c0 && nc <= c1 && !out[nr][nc]) {
            out[nr][nc] = true;
            cells.push_back({nr, nc});
        }
    }
}

static TerritoryProblem generate_territory_problem() {
    const int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    TerritoryProblem prob;
    for (;;) {
        memset(prob.board, 0, sizeof(prob.board));
        prob.black_score = prob.white_score = 0;

        // Sizes: randomise independently but keep them broadly comparable
        int base   = 15 + rand() % 26;          // 15-40
        int bsize  = base - rand() % 8;
        int wsize  = base - rand() % 8;
        if (bsize < 8) bsize = 8;
        if (wsize < 8) wsize = 8;

        // Pick two different shapes (0=compact, 1=horizontal, 2=vertical)
        int shape1 = rand() % 3;
        int shape2 = (shape1 + 1 + rand() % 2) % 3;

        // Black blob in upper strip, white in lower strip (row 9 gap prevents overlap)
        // Ranges reach the board edges so corner/edge territories are generated naturally
        bool bblob[BOARD_SIZE][BOARD_SIZE];
        bool wblob[BOARD_SIZE][BOARD_SIZE];
        grow_blob(0, 0, 7, 18, bsize, bblob, shape1);
        grow_blob(11, 0, 18, 18, wsize, wblob, shape2);

        // Pass 1: orthogonal border (always placed — these fully enclose the blob)
        for (int r = 0; r < BOARD_SIZE; r++) {
            for (int c = 0; c < BOARD_SIZE; c++) {
                if (bblob[r][c]) {
                    for (auto& d : dirs) {
                        int nr=r+d[0], nc=c+d[1];
                        if (nr<0||nr>=BOARD_SIZE||nc<0||nc>=BOARD_SIZE) continue;
                        if (!bblob[nr][nc] && prob.board[nr][nc] == 0)
                            prob.board[nr][nc] = 1;
                    }
                }
                if (wblob[r][c]) {
                    for (auto& d : dirs) {
                        int nr=r+d[0], nc=c+d[1];
                        if (nr<0||nr>=BOARD_SIZE||nc<0||nc>=BOARD_SIZE) continue;
                        if (!wblob[nr][nc] && prob.board[nr][nc] == 0)
                            prob.board[nr][nc] = 2;
                    }
                }
            }
        }

        // Pass 2: diagonal corner fills — placed randomly, but no two adjacent
        // omissions (which would create a false eye).
        const int diags[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};

        auto add_corner_fills = [&](bool blob[BOARD_SIZE][BOARD_SIZE], char val) {
            // Collect candidates: diagonal-only blob neighbours not yet filled
            std::vector<std::pair<int,int>> cands;
            for (int r = 0; r < BOARD_SIZE; r++) {
                for (int c = 0; c < BOARD_SIZE; c++) {
                    if (blob[r][c] || prob.board[r][c] != 0) continue;
                    bool diag_blob = false;
                    for (auto& d : diags) {
                        int nr=r+d[0], nc=c+d[1];
                        if (nr>=0&&nr<BOARD_SIZE&&nc>=0&&nc<BOARD_SIZE&&blob[nr][nc])
                            { diag_blob=true; break; }
                    }
                    if (diag_blob) cands.push_back({r, c});
                }
            }
            // Shuffle so exclusions are spread randomly
            for (int i=(int)cands.size()-1; i>0; i--) {
                int j = rand()%(i+1); std::swap(cands[i], cands[j]);
            }
            bool omitted[BOARD_SIZE][BOARD_SIZE] = {};
            for (auto& [r, c] : cands) {
                // Try to omit with ~35% probability
                if (rand() % 10 < 4) {
                    // Only omit if no orthogonally adjacent corner is also omitted
                    bool adj_omit = false;
                    for (auto& d : dirs) {
                        int nr=r+d[0], nc=c+d[1];
                        if (nr>=0&&nr<BOARD_SIZE&&nc>=0&&nc<BOARD_SIZE&&omitted[nr][nc])
                            { adj_omit=true; break; }
                    }
                    if (!adj_omit) { omitted[r][c]=true; continue; }
                }
                prob.board[r][c] = val;
            }
        };

        add_corner_fills(bblob, 1);
        add_corner_fills(wblob, 2);

        // Scatter interior stones (same colour) to reduce apparent territory
        for (int r = 0; r < BOARD_SIZE; r++) {
            for (int c = 0; c < BOARD_SIZE; c++) {
                if (bblob[r][c] && prob.board[r][c] == 0 && rand() % 5 == 0)
                    prob.board[r][c] = 1;
                if (wblob[r][c] && prob.board[r][c] == 0 && rand() % 5 == 0)
                    prob.board[r][c] = 2;
            }
        }

        // Count empty interior points
        for (int r = 0; r < BOARD_SIZE; r++) {
            for (int c = 0; c < BOARD_SIZE; c++) {
                if (bblob[r][c] && prob.board[r][c] == 0) prob.black_score++;
                if (wblob[r][c] && prob.board[r][c] == 0) prob.white_score++;
            }
        }

        if (prob.black_score != prob.white_score) break;  // reject ties
    }
    return prob;
}

// ---------------------------------------------------------------------------
// Application

enum NavRequest { NAV_NONE, NAV_NEXT, NAV_PREV, NAV_RESTART, NAV_SELECT };

class App {
public:
    int run(int argc, char* argv[]);

private:
    // SDL
    SDL_Window*   window   = nullptr;
    SDL_Renderer* sdl_rend = nullptr;

    // Core state
    GameState                      game;
    std::unique_ptr<AnalysisState> analysis; // non-null only while in analysis mode

    // File browser
    Catalog catalog;

    // Rendering
    Renderer* renderer = nullptr;

    // Current game data
    SgfGame sgf;
    int     game_index = 0; // current move index within sgf.moves[]

    // Playback controls
    int    move_delay_ms       = MOVE_DELAY_MS;
    bool   game_mode           = false;
    bool   guess_mode          = false;
    int    guess_score         = 0;
    int    guess_count         = 0;   // guesses made this session
    int    guess_last_points   = -1;  // score of the most recent guess, -1 = none yet
    int    guess_last_dist     = -1;  // its ring distance, -1 = none yet

    void reset_guess_stats() {
        guess_score = 0; guess_count = 0;
        guess_last_points = -1; guess_last_dist = -1;
    }
    bool   territory_drill_active = false;
    std::unique_ptr<TerritoryProblem> territory_problem;
    bool   chain_mode          = true;
    bool   free_mode           = false;
    int    free_board_size     = BOARD_SIZE;  // board size used in free mode (2..MAX_BOARD_SIZE)
    bool   show_help           = false;
    bool   cursor_visible      = true;
    Uint32 last_mouse_activity = 0;
    Uint32 speed_message_until  = 0;
    std::string flash_message;
    Uint32 flash_message_until  = 0;
    bool   suppress_present     = false;

    // Save-position text input state
    int  save_input_step = 0;   // 0=off, 1=entering name, 2=entering note
    std::string save_input_buf;
    std::string save_pending_name;
    char save_pending_board[BOARD_SIZE][BOARD_SIZE] = {};
    int  save_pending_turn = 1;
    std::string result_message;
    std::string game_date;
    std::string game_comment;
    std::string black_name, white_name;
    std::string games_dir;
    std::string forced_path;  // set by catalog selection
    bool quit_confirm      = false; // waiting for second Q to confirm quit
    bool show_move_numbers = false; // overlay move numbers on stones (toggle with 1)
    int  analysis_num_grid[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    int  analysis_col_grid[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    int  analysis_move_num = 0;


    // Box selection: shift+drag to select rectangles, additive across drags
    bool box_sel_pts[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    int  box_sel_count  = 0;
    bool box_drag_active = false;   // drag in progress
    int  box_drag_r1 = 0, box_drag_f1 = 0;  // drag start (board coords)

    void clear_box_sel() {
        memset(box_sel_pts, 0, sizeof(box_sel_pts));
        box_sel_count   = 0;
        box_drag_active = false;
    }

    void commit_box(int r1, int f1, int r2, int f2) {
        int rmin = std::min(r1,r2), rmax = std::max(r1,r2);
        int fmin = std::min(f1,f2), fmax = std::max(f1,f2);
        const auto& board = analysis ? analysis->board : game.board;
        for (int r = rmin; r <= rmax; r++)
            for (int f = fmin; f <= fmax; f++)
                if (!box_sel_pts[r][f] && board[r][f] == 0)
                    { box_sel_pts[r][f] = true; box_sel_count++; }
    }

    // Clamp a screen position to the nearest board intersection.
    void screen_to_board_clamped(const BoardView& view, int mx, int my, int& r, int& f) {
        r = (my - view.offset_y) / view.square;
        f = (mx - view.offset_x) / view.square;
        r = std::max(0, std::min(view.active_size - 1, r));
        f = std::max(0, std::min(view.active_size - 1, f));
    }

    // Catalog thumbnails: opening (first N moves) and final position.
    std::string thumb_path;
    char        thumb_open[BOARD_SIZE][BOARD_SIZE];   // after THUMB_OPENING_MOVES moves
    char        thumb_final[BOARD_SIZE][BOARD_SIZE];  // after all moves
    bool        thumb_valid = false;

    // Navigation
    NavRequest nav_request     = NAV_NONE;
    Uint32     last_move_tick  = 0;  // time of the last move advance (auto or manual)

    // Helpers
    bool in_analysis() const { return analysis != nullptr; }
    int  active_size()  const {
        return (free_mode && analysis) ? analysis->board_size : BOARD_SIZE;
    }

    void enter_analysis() {
        // Take a clean snapshot of game state and give it to AnalysisState.
        // GameState is never touched again until exit_analysis().
        analysis = std::make_unique<AnalysisState>(game.take_snapshot());
        // turn_is_black is inherited from the snapshot (i.e. whoever's turn it is in the game).
        memset(analysis_num_grid, 0, sizeof(analysis_num_grid));
        memset(analysis_col_grid, 0, sizeof(analysis_col_grid));
        analysis_move_num = 0;
        game.liberty_count = 0;
        game.selected_group_count = 0;
    }

    void exit_analysis() {
        analysis.reset();
        game.liberty_count = 0;
        game.selected_group_count = 0;
        free_mode = false;
    }

    void enter_game_mode() {
        if (in_analysis()) exit_analysis();
        guess_mode = false;
        free_mode  = false;
        GameSnapshot empty{};          // blank board, black goes first
        analysis = std::make_unique<AnalysisState>(empty);
        analysis->turn_is_black = 1;
        game.liberty_count = 0;
        game.selected_group_count = 0;
        game_mode   = true;
        black_name  = "Black";
        white_name  = "White";
    }

    void enter_free_mode() {
        if (territory_drill_active) exit_territory_drill();
        if (game_mode) exit_game_mode();
        else if (in_analysis()) exit_analysis();
        guess_mode = false; reset_guess_stats();
        GameSnapshot empty{};
        analysis = std::make_unique<AnalysisState>(empty, free_board_size);
        memset(analysis_num_grid, 0, sizeof(analysis_num_grid));
        memset(analysis_col_grid, 0, sizeof(analysis_col_grid));
        analysis_move_num = 0;
        game.liberty_count = 0;
        game.selected_group_count = 0;
        free_mode = true;
    }

    void exit_free_mode() {
        analysis.reset();
        game.liberty_count = 0;
        game.selected_group_count = 0;
        free_mode      = false;
        black_name     = sgf.black_name;
        white_name     = sgf.white_name;
    }

    void enter_territory_drill() {
        if (game_mode)      exit_game_mode();
        if (in_analysis())  exit_analysis();
        guess_mode = false;
        territory_drill_active = true;
        territory_problem = std::make_unique<TerritoryProblem>(generate_territory_problem());
    }

    void exit_territory_drill() {
        territory_drill_active = false;
        territory_problem.reset();
    }

    void exit_game_mode() {
        analysis.reset();
        game.liberty_count = 0;
        game.selected_group_count = 0;
        game_mode  = false;
        free_mode  = false;
        black_name = sgf.black_name;
        white_name = sgf.white_name;
    }

    bool init();
    void cleanup();

    // Returns true if the game loop should quit the application entirely.
    bool play_current_game();

    // Input handling
    void handle_key(SDL_Keycode key, const Uint8* kb_state, bool& quit);
    void handle_analysis_lclick(const BoardView& view, int mx, int my, const Uint8* kb);
    void handle_analysis_rclick(const BoardView& view, int mx, int my);
    void handle_playback_lclick(const BoardView& view, int mx, int my);
    void handle_guess_lclick(const BoardView& view, int mx, int my,
                             bool& guess_pending, int& guess_r, int& guess_f);

    bool animate_move(int r, int f, int is_black);
    void save_position();
    void do_save(const std::string& name, const std::string& note);

    int  adjust_move_delay(int delta_ms, Uint32 now);
    void set_cursor_visible(bool v);
    void note_mouse_activity(Uint32 now);
    void note_mouse_activity_event(const SDL_Event& e);
    void update_cursor_auto_hide(Uint32 now);

    Renderer::DrawState make_draw_state() {
        const TerritoryProblem* tp = territory_problem.get();

        // Software cursor type: determined from current mode
        int cursor_type = 0;
        if (cursor_visible) {
            if (in_analysis() || game_mode || guess_mode) {
                const Uint8* kb = SDL_GetKeyboardState(nullptr);
                int is_black;
                if (in_analysis() && kb[SDL_SCANCODE_B])      is_black = 1;
                else if (in_analysis() && kb[SDL_SCANCODE_W]) is_black = 0;
                else if (in_analysis() || game_mode)          is_black = analysis ? analysis->turn_is_black : 1;
                else                                          is_black = game.turn_is_black;
                cursor_type = is_black ? 3 : 2;  // 3=black stone, 2=white stone
            } else {
                cursor_type = 1;  // crosshair
            }
        }
        int cx = -1, cy = -1;
        SDL_GetMouseState(&cx, &cy);

        // Catalog thumbnails: parse opening + final position when selection changes
        if (catalog.active) {
            std::string sel = catalog.selected_entry_path();
            if (sel != thumb_path) {
                thumb_path  = sel;
                thumb_valid = !sel.empty()
                    && sgf_board_at(sel, thumb_open,  THUMB_OPENING_MOVES)
                    && sgf_board_at(sel, thumb_final);
            }
        }

        // Stone visibility filter: only active during plain playback
        int stone_filter = 0;
        if (!in_analysis() && !game_mode && !guess_mode && !territory_drill_active) {
            const Uint8* kb2 = SDL_GetKeyboardState(nullptr);
            if      (kb2[SDL_SCANCODE_B]) stone_filter = 1;  // black only
            else if (kb2[SDL_SCANCODE_W]) stone_filter = 2;  // white only
        }

        // Bullseye aim point: the intersection under the mouse. Nearest-cell, not
        // screen_to_board's — that one rejects positions near a cell edge, which
        // would make the heatmap blink out between cells. handle_guess_lclick snaps
        // identically, so the highlighted cell is always the one a click guesses.
        int heat_r = -1, heat_f = -1;
        if (guess_mode && cursor_visible) {
            BoardView hv; renderer->get_board_view(hv, active_size());
            if (hv.square > 0 && cx >= hv.offset_x && cy >= hv.offset_y) {
                int hf = (cx - hv.offset_x) / hv.square;
                int hr = (cy - hv.offset_y) / hv.square;
                if (hr < hv.active_size && hf < hv.active_size) { heat_r = hr; heat_f = hf; }
            }
        }

        // Compute drag end from current mouse position
        int box_r2 = box_drag_r1, box_f2 = box_drag_f1;
        if (box_drag_active) {
            BoardView tmpview; renderer->get_board_view(tmpview);
            screen_to_board_clamped(tmpview, cx, cy, box_r2, box_f2);
        }

        return Renderer::DrawState{
            .game                   = game,
            .analysis               = analysis.get(),
            .analysis_mode          = in_analysis(),
            .game_mode              = game_mode,
            .guess_mode             = guess_mode,
            .guess_score            = guess_score,
            .guess_count            = guess_count,
            .guess_last_points      = guess_last_points,
            .guess_last_dist        = guess_last_dist,
            .guess_heat_r           = heat_r,
            .guess_heat_f           = heat_f,
            .cursor_snap_heat       = true,
            .chain_mode             = chain_mode,
            .free_mode              = free_mode,
            .active_board_size      = active_size(),
            .show_help              = show_help,
            .catalog                = catalog,
            .black_name             = black_name,
            .white_name             = white_name,
            .result_message         = result_message,
            .game_date              = game_date,
            .game_comment           = game_comment,
            .move_delay_ms          = move_delay_ms,
            .speed_message_until    = speed_message_until,
            .suppress_present       = suppress_present,
            .territory_drill        = territory_drill_active,
            .territory_board        = tp ? tp->board : nullptr,
            .territory_b_score      = tp ? tp->black_score : 0,
            .territory_w_score      = tp ? tp->white_score : 0,
            .territory_answered     = tp ? tp->answered    : false,
            .territory_correct      = tp ? tp->correct     : false,
            .stone_filter           = stone_filter,
            .cursor_x               = cx,
            .cursor_y               = cy,
            .cursor_type            = cursor_type,
            .show_move_numbers      = show_move_numbers,
            .sgf_moves              = sgf.moves,
            .sgf_colors             = sgf.colors,
            .sgf_game_index         = game_index,
            .analysis_num_grid      = analysis_num_grid,
            .analysis_col_grid      = analysis_col_grid,
            .quit_confirm           = quit_confirm,
            .box_sel_pts            = box_sel_pts,
            .box_sel_count          = box_sel_count,
            .box_drag_active        = box_drag_active,
            .box_drag_r1            = box_drag_r1,
            .box_drag_f1            = box_drag_f1,
            .box_drag_r2            = box_r2,
            .box_drag_f2            = box_f2,
            .catalog_thumb_valid    = thumb_valid,
            .catalog_thumb_open     = thumb_valid ? thumb_open  : nullptr,
            .catalog_thumb_final    = thumb_valid ? thumb_final : nullptr,
            .catalog_thumb_board_size = BOARD_SIZE,
            .flash_message          = flash_message,
            .flash_message_until    = flash_message_until,
            .save_input_step        = save_input_step,
            .save_input_buf         = save_input_buf,
        };
    }

    void draw_board() {
        if (catalog.active) {
            // Lazy-load display names for visible entries not in the game index
            if (!catalog.search_mode && !catalog.virtual_year_mode && !catalog.virtual_player_mode)
                catalog.ensure_names_loaded(catalog.scroll, 80);
            // Refresh virtual year list once the background index finishes
            catalog.tick();
        }
        auto ds = make_draw_state();
        renderer->draw_board(ds);
    }

    // Pick a file to load, respecting sequential / random / forced selection
    std::string pick_next_file(NavRequest req);
};

void App::set_cursor_visible(bool v) {
    cursor_visible = v;
}

void App::note_mouse_activity(Uint32 now) {
    last_mouse_activity = now;
    if (!cursor_visible) set_cursor_visible(true);
}

void App::note_mouse_activity_event(const SDL_Event& e) {
    if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN ||
        e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEWHEEL)
        note_mouse_activity(SDL_GetTicks());
}

void App::update_cursor_auto_hide(Uint32 /*now*/) {
    if (!cursor_visible) set_cursor_visible(true);
}

int App::adjust_move_delay(int delta_ms, Uint32 now) {
    int nd = move_delay_ms + delta_ms;
    if (nd < MOVE_DELAY_MIN_MS) nd = MOVE_DELAY_MIN_MS;
    if (nd > MOVE_DELAY_MAX_MS) nd = MOVE_DELAY_MAX_MS;
    if (nd == move_delay_ms) return 0;
    move_delay_ms      = nd;
    speed_message_until = now + SPEED_MESSAGE_MS;
    return 1;
}

// ---------------------------------------------------------------------------
// init / cleanup

bool App::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    window = SDL_CreateWindow("Go Viewer",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              SCREEN_SIZE, SCREEN_SIZE,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    sdl_rend = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window || !sdl_rend) return false;
    renderer = new Renderer(sdl_rend);
    SDL_ShowCursor(SDL_DISABLE);  // software cursor drawn by renderer
    cursor_visible = true;
    note_mouse_activity(SDL_GetTicks());
    srand(std::random_device{}());
    return true;
}

void App::cleanup() {
    delete renderer; renderer = nullptr;
    if (sdl_rend) { SDL_DestroyRenderer(sdl_rend); sdl_rend = nullptr; }
    if (window)   { SDL_DestroyWindow(window);      window   = nullptr; }
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Game-level logic

bool App::animate_move(int r, int f, int is_black) {
    if (!game.place_stone(r, f, is_black)) return false;
    game.save_snapshot();
    draw_board();
    return true;
}

// ---------------------------------------------------------------------------
// Input handlers

void App::handle_key(SDL_Keycode key, const Uint8* /*kb*/, bool& quit) {
    // Save dialog intercepts all events in the event loop; this guard covers
    // any path that might call handle_key directly while the dialog is open.
    if (save_input_step != 0) return;

    Uint32 now = SDL_GetTicks();

    if (key == SDLK_q) {
        if (quit_confirm) { nav_request = NAV_NONE; quit = true; return; }
        quit_confirm = true;
        draw_board();
        return;
    }
    // Any other key cancels quit confirm.  Escape is handled here immediately so that
    // mode-specific blocks (territory drill, etc.) that return early can't swallow it.
    if (quit_confirm) {
        quit_confirm = false;
        draw_board();
        if (key == SDLK_ESCAPE) return;   // Escape: cancel only, don't fall through
    }

    // Territory drill intercepts most keys
    if (territory_drill_active) {
        if (key == SDLK_t) {
            exit_territory_drill(); draw_board();
        } else if (!territory_problem->answered) {
            if (key == SDLK_b || key == SDLK_w) {
                bool user_says_black = (key == SDLK_b);
                bool black_bigger    = territory_problem->black_score > territory_problem->white_score;
                territory_problem->answered = true;
                territory_problem->correct  = (user_says_black == black_bigger);
                draw_board();
            }
        } else if (key == SDLK_SPACE || key == SDLK_RETURN) {
            territory_problem = std::make_unique<TerritoryProblem>(generate_territory_problem());
            draw_board();
        }
        return;
    }

    if (key == SDLK_t) {
        enter_territory_drill(); draw_board(); return;
    }

    if (key == SDLK_n) { nav_request = NAV_NEXT; quit = true; return; }
    if (key == SDLK_r) {
        if (in_analysis()) {
            // Reset analysis board back to the position it was opened on
            analysis->reset_to_base();
            game.liberty_count = 0;
            game.selected_group_count = 0;
            memset(analysis_num_grid, 0, sizeof(analysis_num_grid));
            memset(analysis_col_grid, 0, sizeof(analysis_col_grid));
            analysis_move_num = 0;
            clear_box_sel();
            draw_board();
            return;
        }
        nav_request = NAV_RESTART; quit = true; return;
    }

    if (key == SDLK_c) {
        catalog.open(games_dir);
        draw_board();
        return;
    }
    if (key == SDLK_ESCAPE) {
        if (quit_confirm) { quit_confirm = false; draw_board(); return; }
        if (box_sel_count > 0 || box_drag_active) { clear_box_sel(); draw_board(); return; }
        show_help = !show_help;
        draw_board();
        return;
    }

    if (key == SDLK_UP || key == SDLK_DOWN) {
        int delta = (key == SDLK_UP) ? MOVE_DELAY_STEP_MS : -MOVE_DELAY_STEP_MS;
        if (adjust_move_delay(delta, now))
            draw_board();
        return;
    }

    // Space and A both toggle analysis mode
    if (key == SDLK_SPACE || key == SDLK_a) {
        if (game_mode) {
            // Space/A exits game mode back to playback
            exit_game_mode();
        } else if (in_analysis()) {
            exit_analysis();
        } else {
            if (guess_mode) { guess_mode = false; }
            enter_analysis();
        }
        set_cursor_visible(true);
        draw_board();
        return;
    }

    if (key == SDLK_g) {
        if (guess_mode) {
            guess_mode = false;
            game.liberty_count = 0;
        } else {
            if (in_analysis()) exit_analysis();
            if (game_mode) exit_game_mode();
            guess_mode = true;
            reset_guess_stats();
        }
        draw_board();
        return;
    }

    if (key == SDLK_p) {
        if (game_mode)
            exit_game_mode();
        else
            enter_game_mode();
        draw_board();
        return;
    }

    if (key == SDLK_f) {
        if (free_mode) exit_free_mode();
        else           enter_free_mode();
        set_cursor_visible(true);
        draw_board();
        return;
    }

    if (free_mode && (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS)) {
        // + = zoom in = fewer intersections
        int ns = free_board_size - 1;
        if (ns >= 2) {
            free_board_size = ns;
            analysis->board_size = ns;
        }
        draw_board();
        return;
    }
    if (free_mode && (key == SDLK_MINUS || key == SDLK_KP_MINUS)) {
        // - = zoom out = more intersections
        int ns = free_board_size + 1;
        if (ns <= MAX_BOARD_SIZE) {
            free_board_size = ns;
            analysis->board_size = ns;
        }
        draw_board();
        return;
    }

    if (key == SDLK_s) {
        save_position();
        return;
    }

    if (key == SDLK_u) {
        chain_mode = !chain_mode;
        draw_board();
        return;
    }

    if (key == SDLK_1) {
        show_move_numbers = !show_move_numbers;
        draw_board();
        return;
    }

    if (key == SDLK_x && in_analysis()) {
        memset(analysis->board, 0, sizeof(analysis->board));
        analysis->black_prisoners = 0;
        analysis->white_prisoners = 0;
        game.liberty_count = 0;
        game.selected_group_count = 0;
        draw_board();
        return;
    }

    if (!guess_mode && !in_analysis()) {
        if (key == SDLK_LEFT && game_index > 0) {
            game_index--;
            if (game_index == 0)
                game.reset();
            else
                game.restore_snapshot(game_index - 1);
            last_move_tick = SDL_GetTicks();
            draw_board();
            return;
        }
        if (key == SDLK_RIGHT && game_index < sgf.move_count) {
            int r, f;
            if (parse_sgf_move(sgf.moves[game_index], r, f)) {
                bool is_black = (sgf.colors[game_index] == 1);
                if (animate_move(r, f, is_black)) {
                    game_index++;
                    if (game_index < sgf.move_count)
                        game.turn_is_black = sgf.colors[game_index];
                }
            } else {
                game_index++;
            }
            last_move_tick = SDL_GetTicks();  // reset timer so auto-advance doesn't fire immediately
            draw_board();
            return;
        }
    }
}

void App::handle_analysis_lclick(const BoardView& view, int mx, int my, const Uint8* kb) {
    int r = -1, f = -1;
    if (!renderer->screen_to_board(view, mx, my, r, f)) return;
    if (!analysis) return;

    if (analysis->board[r][f] != 0) {
        // Toggle liberty display for the clicked stone's chain
        if (analysis->is_stone_in_selected_group(r, f)) {
            analysis->selected_group_count = 0;
            analysis->liberty_count        = 0;
        } else {
            analysis->store_selected_group(r, f);
            analysis->calculate_chain_liberties(r, f);
        }
        draw_board();
        return;
    }

    // Place a new stone
    int forced_color = -1;
    if (kb[SDL_SCANCODE_B]) forced_color = 1;
    if (kb[SDL_SCANCODE_W]) forced_color = 0;
    int stone_color = (forced_color != -1) ? forced_color : analysis->turn_is_black;
    if (analysis->place_stone(r, f, stone_color)) {
        analysis_num_grid[r][f] = ++analysis_move_num;
        analysis_col_grid[r][f] = stone_color;
        if (forced_color == -1)
            analysis->turn_is_black = !analysis->turn_is_black;
        analysis->liberty_count        = 0;
        analysis->liberty_display_r    = -1;
        analysis->liberty_display_f    = -1;
        analysis->selected_group_count = 0;
    }
    draw_board();
}

void App::handle_analysis_rclick(const BoardView& view, int mx, int my) {
    int r = -1, f = -1;
    if (!renderer->screen_to_board(view, mx, my, r, f)) return;
    if (!analysis) return;
    if (analysis->remove_stone_at(r, f)) {
        analysis->liberty_count        = 0;
        analysis->liberty_display_r    = -1;
        analysis->liberty_display_f    = -1;
        analysis->selected_group_count = 0;
        draw_board();
    }
}

void App::handle_playback_lclick(const BoardView& view, int mx, int my) {
    int r = -1, f = -1;
    if (!renderer->screen_to_board(view, mx, my, r, f)) return;
    if (game.board[r][f] == 0) return;
    if (game.is_stone_in_selected_group(r, f)) {
        game.selected_group_count = 0;
        game.liberty_count        = 0;
    } else {
        game.store_selected_group(r, f);
        game.calculate_chain_liberties(r, f);
    }
    draw_board();
}

void App::handle_guess_lclick(const BoardView& view, int mx, int my,
                              bool& guess_pending, int& gr, int& gf) {
    // Nearest intersection, not screen_to_board's — that one rejects clicks near a
    // cell's edge, which would silently drop guesses on points the heatmap cursor is
    // visibly highlighting. The heatmap snaps the same way, so what you see is what
    // you guess.
    if (view.square <= 0) return;
    if (mx < view.offset_x || my < view.offset_y) return;   // before the divide
    int f = (mx - view.offset_x) / view.square;
    int r = (my - view.offset_y) / view.square;
    if (r >= view.active_size || f >= view.active_size) return;
    if (game.board[r][f] == 0) {
        gr = r; gf = f;
        guess_pending = true;
    }
}

// ---------------------------------------------------------------------------
// File selection

std::string App::pick_next_file(NavRequest req) {
    // Forced selection from catalog
    if (!forced_path.empty() && req == NAV_SELECT) {
        std::string p = forced_path;
        forced_path.clear();
        return p;
    }

    std::vector<std::string> files;
    std::string dir;

    if (!catalog.sequential_dir.empty()) {
        dir = catalog.sequential_dir;
        Catalog::list_sgf_files(dir, files);
        if (files.empty()) {
            catalog.sequential_dir   = "";
            catalog.sequential_index = 0;
        }
    }

    if (files.empty()) {
        dir = games_dir;
        Catalog::list_sgf_files(dir, files);
    }
    if (files.empty()) return {};

    int idx = 0;
    if (!catalog.sequential_dir.empty()) {
        idx = catalog.sequential_index % (int)files.size();
        catalog.sequential_index++;
    } else {
        if (req == NAV_PREV) {
            // not tracked in random mode — just pick random
        }
        idx = rand() % (int)files.size();
    }

    // Build full path (files[] may already be relative to dir)
    std::string p = files[idx];
    if (p.size() < 2 || (p[0] != '/' && p[1] != ':' && p[0] != '\\'))
        p = dir + PATH_SEP_STR + p;
    return p;
}

// ---------------------------------------------------------------------------
// Save position to SGF

// Escape SGF text values: ] and \ must be backslash-escaped.
static std::string sgf_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == ']') out += '\\';
        out += c;
    }
    return out;
}

// Sanitise a user-supplied name into a safe filename component.
static std::string sanitise_filename(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c == ' ' || c == '\t') out += '_';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            out += c;
        // other characters silently dropped
    }
    if (out.size() > 40) out.resize(40);
    return out;
}

// Capture current board and enter the name-entry prompt.
void App::save_position() {
    const char (*b)[MAX_BOARD_SIZE] = analysis ? analysis->board : game.board;
    memcpy(save_pending_board, b, sizeof(save_pending_board));
    save_pending_turn = analysis ? analysis->turn_is_black : game.turn_is_black;
    save_pending_name.clear();
    save_input_buf.clear();
    save_input_step = 1;
    draw_board();
}

// Write the SGF once name + note are collected.
void App::do_save(const std::string& name, const std::string& note) {
    // Create the saved/ subdirectory if needed
    std::string save_dir = Catalog::join_path(games_dir, "saved");
#ifdef _WIN32
    CreateDirectoryA(save_dir.c_str(), nullptr);
#else
    mkdir(save_dir.c_str(), 0755);
#endif

    // Build filename: [name_]YYYYMMDD_HHMMSS.sgf
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm_info);
    std::string safe = sanitise_filename(name);
    std::string filename = (safe.empty() ? "" : safe + "_") + ts + ".sgf";
    std::string path     = Catalog::join_path(save_dir, filename);

    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) {
        flash_message       = "Save failed!";
        flash_message_until = SDL_GetTicks() + 2500;
        draw_board();
        return;
    }

    fprintf(fp, "(;GM[1]FF[4]SZ[19]\n");
    if (!name.empty())         fprintf(fp, "GN[%s]\n", sgf_escape(name).c_str());
    if (black_name != "Black") fprintf(fp, "PB[%s]\n", black_name.c_str());
    if (white_name != "White") fprintf(fp, "PW[%s]\n", white_name.c_str());
    if (!game_date.empty())    fprintf(fp, "DT[%s]\n", game_date.c_str());
    if (!note.empty())         fprintf(fp, "C[%s]\n",  sgf_escape(note).c_str());
    fprintf(fp, "PL[%c]\n", save_pending_turn ? 'B' : 'W');

    // Black setup stones
    bool any = false;
    for (int r = 0; r < BOARD_SIZE; r++)
        for (int f = 0; f < BOARD_SIZE; f++)
            if (save_pending_board[r][f] == 1) {
                if (!any) { fprintf(fp, "AB"); any = true; }
                fprintf(fp, "[%c%c]", 'a' + f, 'a' + r);
            }
    if (any) fprintf(fp, "\n");

    // White setup stones
    any = false;
    for (int r = 0; r < BOARD_SIZE; r++)
        for (int f = 0; f < BOARD_SIZE; f++)
            if (save_pending_board[r][f] == 2) {
                if (!any) { fprintf(fp, "AW"); any = true; }
                fprintf(fp, "[%c%c]", 'a' + f, 'a' + r);
            }
    if (any) fprintf(fp, "\n");

    fprintf(fp, ")\n");
    fclose(fp);

    // Insert into the in-memory index immediately so the catalog reflects this
    // file on the next open without needing a full rebuild.
    {
        std::string rel = Catalog::join_path("saved", filename);
        GameIndexEntry ge;
        ge.rel_path = rel;
        ge.black    = (black_name != "Black") ? black_name : "";
        ge.white    = (white_name != "White") ? white_name : "";
        ge.date     = game_date;
        catalog.game_index.insert_entry(ge);

        // If the catalog is open in a list view, flag it for an immediate refresh.
        if (catalog.active) {
            if (catalog.virtual_player_mode && catalog.virtual_player.empty())
                catalog.player_needs_refresh = true;
            if (catalog.virtual_year_mode && catalog.virtual_year.empty())
                catalog.year_needs_refresh = true;
        }
    }

    std::string lbl = name.empty() ? filename : "\"" + name + "\"";
    flash_message       = "Saved: " + lbl;
    flash_message_until = SDL_GetTicks() + 2500;
    draw_board();
}

// ---------------------------------------------------------------------------
// Main game loop

bool App::play_current_game() {
    result_message = sgf.result;
    game_date      = sgf.date;
    game_comment   = sgf.comment;
    black_name     = sgf.black_name;
    white_name     = sgf.white_name;

    game.reset();
    // Apply setup position from AB[]/AW[] if present (saved-position SGFs)
    if (sgf.has_setup) {
        memcpy(game.board, sgf.initial_board, sizeof(game.board));
        game.turn_is_black = sgf.initial_turn_is_black;
    }
    game_index = 0;
    nav_request = NAV_NONE;
    clear_box_sel();
    if (territory_drill_active) exit_territory_drill();
    if (game_mode) exit_game_mode();
    else if (in_analysis()) exit_analysis();
    guess_mode  = false;
    reset_guess_stats();
    free_mode   = false;
    // Pump pending events so the window is fully mapped before the first draw.
    // On Linux/Wayland the compositor may not have set the real window size yet;
    // this gives it a chance to fire SDL_WINDOWEVENT_SIZE_CHANGED first.
    SDL_PumpEvents();
    draw_board();

    bool quit          = false;
    bool guess_pending = false;
    int  guess_r = -1, guess_f = -1;
    last_move_tick = SDL_GetTicks();

    while (!quit) {
        Uint32 now = SDL_GetTicks();
        update_cursor_auto_hide(now);

        // Set current turn from SGF
        if (game_index < sgf.move_count)
            game.turn_is_black = sgf.colors[game_index];

        // ---------------------------------------------------------------------------
        // Compute how long we can sleep before something needs to happen on a timer.
        // Any input event (mouse move, key, etc.) wakes us immediately regardless,
        // so cursor tracking is always smooth with near-zero CPU when idle.
        int wait_ms = 1000;

        // Wake when the speed-change label or flash message should disappear
        if (speed_message_until > 0 && speed_message_until > now)
            wait_ms = std::min(wait_ms, (int)(speed_message_until - now));
        if (flash_message_until > 0 && flash_message_until > now)
            wait_ms = std::min(wait_ms, (int)(flash_message_until - now));

        // Wake periodically while the game index is loading so the UI refreshes
        // when the background thread finishes (shows names / enables search).
        if (catalog.active && catalog.game_index.is_loading())
            wait_ms = std::min(wait_ms, 250);

        // In playback: wake when the next move is due or the game-over pause ends
        if (!in_analysis() && !guess_mode && !territory_drill_active && !catalog.active) {
            if (game_index < sgf.move_count) {
                int remaining = (int)move_delay_ms - (int)(now - last_move_tick);
                wait_ms = std::min(wait_ms, std::max(remaining, 0));
            } else if (game.game_finished && game.game_finished_timer > 0) {
                int remaining = (int)GAME_OVER_PAUSE_MS - (int)(now - game.game_finished_timer);
                wait_ms = std::min(wait_ms, std::max(remaining, 0));
            }
        }

        // ---------------------------------------------------------------------------
        // Block until an event arrives or the timer fires
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, wait_ms)) {
            // Process this event then drain any others that queued up
            do {
                note_mouse_activity_event(e);

                // Save-input mode: intercept ALL events.
                // Character collection is done via KEYDOWN (not SDL_TEXTINPUT) so
                // that (a) the 's' keypress that opened the dialog is already
                // consumed before save_input_step becomes non-zero, and (b) every
                // game-key KEYDOWN is swallowed here and never reaches handle_key.
                if (save_input_step != 0) {
                    if (e.type == SDL_KEYDOWN) {
                        SDL_Keycode k = e.key.keysym.sym;
                        if (k == SDLK_BACKSPACE) {
                            if (!save_input_buf.empty()) {
                                save_input_buf.pop_back();
                                draw_board();
                            }
                        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                            if (save_input_step == 1) {
                                save_pending_name = save_input_buf;
                                save_input_buf.clear();
                                save_input_step = 2;
                                draw_board();
                            } else {
                                std::string note = save_input_buf;
                                save_input_step  = 0;
                                save_input_buf.clear();
                                do_save(save_pending_name, note);
                            }
                        } else if (k == SDLK_ESCAPE) {
                            save_input_step = 0;
                            save_input_buf.clear();
                            draw_board();
                        } else {
                            // Map keycode to a printable character (US layout)
                            bool shift = (SDL_GetModState() & (KMOD_LSHIFT | KMOD_RSHIFT)) != 0;
                            char c = 0;
                            if (k >= SDLK_a && k <= SDLK_z)
                                c = shift ? (char)('A' + (k - SDLK_a)) : (char)('a' + (k - SDLK_a));
                            else if (k >= SDLK_0 && k <= SDLK_9)
                                c = (char)('0' + (k - SDLK_0));
                            else if (k == SDLK_SPACE)      c = ' ';
                            else if (k == SDLK_MINUS)      c = shift ? '_' : '-';
                            else if (k == SDLK_PERIOD)     c = '.';
                            else if (k == SDLK_COMMA)      c = ',';
                            else if (k == SDLK_QUOTE)      c = shift ? '"' : '\'';
                            else if (k == SDLK_SLASH)      c = shift ? '?' : '/';
                            else if (k == SDLK_SEMICOLON)  c = shift ? ':' : ';';
                            if (c) { save_input_buf += c; draw_board(); }
                        }
                    }
                    // Always consume — never let events through to game handlers
                    continue;
                }

                // Catalog intercepts all input
                if (catalog.active) {
                    if (e.type == SDL_KEYDOWN) {
                        SDL_Keycode key = e.key.keysym.sym;
                        int total = (int)catalog.entries.size();
                        if (key == SDLK_ESCAPE) {
                            if (catalog.search_mode) {
                                catalog.search_clear();
                            } else {
                                catalog.close();
                                last_move_tick = SDL_GetTicks();
                            }
                        } else if (key == SDLK_UP && catalog.index > 0) {
                            catalog.index--;
                        } else if (key == SDLK_DOWN && catalog.index < total - 1) {
                            catalog.index++;
                        } else if (key == SDLK_PAGEUP) {
                            catalog.index = (catalog.index > 6) ? catalog.index - 6 : 0;
                        } else if (key == SDLK_PAGEDOWN) {
                            catalog.index = (catalog.index + 6 < total) ? catalog.index + 6 : total - 1;
                        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                            catalog.select();
                            draw_board();  // refresh immediately (dir nav or file select)
                            if (catalog.selection_made) {
                                forced_path  = catalog.selected_path;
                                catalog.selection_made = false;
                                nav_request  = NAV_SELECT;
                                quit         = true;
                            }
                        } else if (key == SDLK_BACKSPACE) {
                            catalog.search_backspace();
                        } else if (key >= SDLK_a && key <= SDLK_z) {
                            // Letters: append lowercase (shift state ignored for search)
                            catalog.search_append((char)(key - SDLK_a + 'a'));
                        } else if (key >= SDLK_0 && key <= SDLK_9) {
                            catalog.search_append((char)(key - SDLK_0 + '0'));
                        } else if (key == SDLK_SPACE && catalog.search_mode) {
                            catalog.search_append(' ');
                        }
                    }
                    continue;  // skip game-event handling; drain next event
                }

                if (e.type == SDL_WINDOWEVENT) {
                    // On Linux/Wayland the compositor sets the real window size
                    // asynchronously; redraw whenever the window is shown, resized,
                    // or needs repainting so the board is never stretched.
                    switch (e.window.event) {
                        case SDL_WINDOWEVENT_SHOWN:
                        case SDL_WINDOWEVENT_EXPOSED:
                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                            draw_board();
                            break;
                        default: break;
                    }
                } else if (e.type == SDL_QUIT) {
                    nav_request = NAV_NONE; quit = true;
                } else if (e.type == SDL_KEYDOWN) {
                    const Uint8* kb = SDL_GetKeyboardState(nullptr);
                    handle_key(e.key.keysym.sym, kb, quit);
                } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    const Uint8* kb = SDL_GetKeyboardState(nullptr);
                    bool shift = (kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT]);
                    if (shift) {
                        // Begin box-selection drag
                        BoardView view; renderer->get_board_view(view, active_size());
                        screen_to_board_clamped(view, e.button.x, e.button.y,
                                                box_drag_r1, box_drag_f1);
                        box_drag_active = true;
                        draw_board();
                    } else {
                        // Clear any existing box selection on non-shift click
                        if (box_sel_count > 0 || box_drag_active) {
                            clear_box_sel();
                            draw_board();
                        }
                        BoardView view; renderer->get_board_view(view, active_size());
                        if (in_analysis()) {
                            handle_analysis_lclick(view, e.button.x, e.button.y, kb);
                        } else if (guess_mode) {
                            handle_guess_lclick(view, e.button.x, e.button.y, guess_pending, guess_r, guess_f);
                        } else {
                            handle_playback_lclick(view, e.button.x, e.button.y);
                        }
                    }
                } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                    if (box_drag_active) {
                        // Commit the dragged rectangle
                        BoardView view; renderer->get_board_view(view, active_size());
                        int r2, f2;
                        screen_to_board_clamped(view, e.button.x, e.button.y, r2, f2);
                        commit_box(box_drag_r1, box_drag_f1, r2, f2);
                        box_drag_active = false;
                        draw_board();
                    }
                } else if (e.type == SDL_MOUSEMOTION && box_drag_active) {
                    // Only redraw for the last motion event in the current burst.
                    // SDL can queue hundreds of MOUSEMOTION events between frames; calling
                    // SDL_RenderPresent() for each one freezes the program on slow/software
                    // renderers (each present is a full X11/Wayland buffer swap).
                    SDL_Event peek;
                    if (SDL_PeepEvents(&peek, 1, SDL_PEEKEVENT, SDL_MOUSEMOTION, SDL_MOUSEMOTION) == 0)
                        draw_board();
                } else if (in_analysis() && e.type == SDL_MOUSEBUTTONDOWN) {
                    BoardView view; renderer->get_board_view(view, active_size());
                    if (e.button.button == SDL_BUTTON_RIGHT)
                        handle_analysis_rclick(view, e.button.x, e.button.y);
                }
            } while (!quit && SDL_PollEvent(&e));
        }
        if (quit) break;

        // Guess mode: resolve pending guess and advance
        if (guess_pending && game_index < sgf.move_count) {
            int er, ef;
            if (parse_sgf_move(sgf.moves[game_index], er, ef)) {
                // Bullseye scoring: credit for landing near the played move, not just
                // on it. No penalty for a miss — the ceiling (10/guess, shown in the
                // HUD) is what makes a score meaningful, not a negative floor.
                int dist = guess_ring_dist(guess_r, guess_f, er, ef);
                guess_last_dist   = dist;
                guess_last_points = guess_points(dist);
                guess_score      += guess_last_points;
                guess_count++;
                bool is_black = (sgf.colors[game_index] == 1);
                animate_move(er, ef, is_black);
                game_index++;
                if (game_index < sgf.move_count) game.turn_is_black = sgf.colors[game_index];
                last_move_tick = SDL_GetTicks();
            }
            guess_pending = false;
        }

        // Auto-advance playback (analysis/guess/catalog modes freeze this)
        if (!in_analysis() && !guess_mode && !territory_drill_active && !catalog.active) {
            now = SDL_GetTicks();
            if (game_index < sgf.move_count) {
                if (now - last_move_tick >= (Uint32)move_delay_ms) {
                    int r, f;
                    if (parse_sgf_move(sgf.moves[game_index], r, f)) {
                        bool is_black = (sgf.colors[game_index] == 1);
                        animate_move(r, f, is_black);
                    }
                    game_index++;
                    if (game_index < sgf.move_count) game.turn_is_black = sgf.colors[game_index];
                    last_move_tick = now;
                }
            } else {
                if (!game.game_finished) {
                    game.game_finished      = 1;
                    game.game_finished_timer = now;
                }
                if (now - game.game_finished_timer >= GAME_OVER_PAUSE_MS) {
                    nav_request = NAV_NEXT;
                    quit        = true;
                }
            }
        }

        draw_board();
    }

    return quit && nav_request == NAV_NONE;
}

// ---------------------------------------------------------------------------
// Top-level run

int App::run(int /*argc*/, char* /*argv*/[]) {
    games_dir = DEFAULT_GAMES_DIR;

    // If test.sgf exists, load it first
    if (FILE* fp = fopen("test.sgf", "r")) {
        fclose(fp);
        forced_path = "test.sgf";
        nav_request = NAV_SELECT;
    }

    if (!init()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Go Viewer",
            "Failed to initialise SDL.", nullptr);
        return 1;
    }

    bool quit = false;
    while (!quit) {
        std::string path;

        if (!forced_path.empty()) {
            path = forced_path;
            forced_path.clear();
        } else {
            path = pick_next_file(nav_request);
        }
        nav_request = NAV_NONE;

        if (path.empty()) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "No SGF files found.\n\nLooked in: %s\n\n"
                     "Place .sgf files in a 'games/' folder next to the executable,\n"
                     "or put a test.sgf next to the executable.",
                     games_dir.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Go Viewer", msg, window);
            break;
        }

        if (!load_sgf(path, sgf)) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Failed to load:\n%s", path.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Go Viewer", msg, window);
            SDL_Delay(500);
            continue;
        }

        bool should_quit = play_current_game();
        if (should_quit) { quit = true; break; }

        if (nav_request == NAV_SELECT && !forced_path.empty()) {
            // Another catalog selection happened inside play_current_game — loop back
            continue;
        }
        if (nav_request == NAV_RESTART) {
            // Reload the same file — path is already gone, so re-set forced_path
            forced_path = path;
        }
    }

    cleanup();
    return 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    App* app = new App();
    int r = app->run(argc, argv);
    delete app;
    return r;
}
