#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cfloat>
#include <cmath>
#include "go_viewer.hpp"
#include "go_rules.hpp"
#include "game_state.hpp"
#include "analysis_state.hpp"
#include "catalog.hpp"
#include "renderer.hpp"
#include "palette.hpp"      // CHALK / CHALK_DARK for the drawing-mode cursor tint
#include "ogs_client.hpp"
#include "ogs_net.hpp"
#include "katago.hpp"
#include "sound.hpp"
#include "ogs_puzzles.hpp"
#include <atomic>

#include "json.hpp"
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <algorithm>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#endif

// Registered once in main(); OgsNet reads this to push SDL events.
Uint32 g_net_event_type = 0;

// ── Credentials ──────────────────────────────────────────────────────────────

static std::string exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0) {
        std::string path(buf, n);
        auto slash = path.find_last_of("\\/");
        if (slash != std::string::npos) return path.substr(0, slash + 1);
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        std::string path(buf, n);
        auto slash = path.find_last_of('/');
        if (slash != std::string::npos) return path.substr(0, slash + 1);
    }
#endif
    return "";
}

// Cross-platform replacements for the small set of Win32 file-system calls this
// app needs (directory existence check, directory creation). Paths are UTF-8
// throughout; on Windows that means routing through the wide APIs the same way
// Catalog::fopen_utf8 does, since plain GetFileAttributesA/CreateDirectoryA go
// through the ANSI codepage and mangle non-Latin paths.
static bool is_dir(const std::string& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(Catalog::utf8_to_wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}
static void make_dir(const std::string& p) {
#ifdef _WIN32
    CreateDirectoryW(Catalog::utf8_to_wide(p).c_str(), nullptr);
#else
    mkdir(p.c_str(), 0755);
#endif
}
static bool path_exists(const std::string& p) {
#ifdef _WIN32
    return GetFileAttributesW(Catalog::utf8_to_wide(p).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}
static bool remove_path(const std::string& p) {
#ifdef _WIN32
    return _wremove(Catalog::utf8_to_wide(p).c_str()) == 0;
#else
    return remove(p.c_str()) == 0;
#endif
}
static bool rename_path(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return _wrename(Catalog::utf8_to_wide(from).c_str(), Catalog::utf8_to_wide(to).c_str()) == 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}
static int str_icmp(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return _stricmp(a.c_str(), b.c_str());
#else
    return strcasecmp(a.c_str(), b.c_str());
#endif
}

// True for "C:\..." or a leading "\"/"/" (including UNC "\\server\...") — anything
// else is a relative path, meant to be resolved against the exe's own directory so
// a katago/ folder dropped next to ogs_client.exe works regardless of launch method
// (double-click, shortcut, terminal — not just the common case where cwd happens to
// already be the exe's folder).
static bool is_absolute_path(const std::string& p) {
    if (p.empty()) return false;
    if (p.size() >= 2 && p[1] == ':') return true;
    if (p[0] == '\\' || p[0] == '/') return true;
    return false;
}
static std::string resolve_path(const std::string& p) {
    if (p.empty() || is_absolute_path(p)) return p;
    return exe_dir() + p;
}

static bool load_config(std::string& username, std::string& password,
                        std::string& kata_exe, std::string& kata_model, std::string& kata_cfg,
                        std::string& kata_model_9x9, std::string& kata_human_model) {
    std::string path = exe_dir() + "config.ini";
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return false;
    char line[4096];  // generous — KataGo paths can be long
    while (fgets(line, sizeof(line), fp)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* val = eq + 1;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r'))
            val[--vlen] = '\0';
        if      (strcmp(line, "username")           == 0) username          = val;
        else if (strcmp(line, "password")           == 0) password          = val;
        else if (strcmp(line, "katago_exe")         == 0) kata_exe          = val;
        else if (strcmp(line, "katago_model")       == 0) kata_model        = val;
        else if (strcmp(line, "katago_config")      == 0) kata_cfg          = val;
        else if (strcmp(line, "katago_model_9x9")   == 0) kata_model_9x9   = val;
        else if (strcmp(line, "katago_human_model") == 0) kata_human_model  = val;
    }
    fclose(fp);
    kata_exe         = resolve_path(kata_exe);
    kata_model       = resolve_path(kata_model);
    kata_cfg         = resolve_path(kata_cfg);
    kata_model_9x9   = resolve_path(kata_model_9x9);
    kata_human_model = resolve_path(kata_human_model);
    // A username means "try to log in"; a bad/empty password just fails auth and
    // drops to the credential prompt. No username means go straight to the prompt.
    return !username.empty();
}

// Persist a prompt-entered login into config.ini so the next launch auto-connects.
// Updates the username=/password= lines in place (appends them if absent), leaving
// everything else — comments, KataGo paths — untouched. Plaintext, the same as any
// config.ini login; the file is the user's own and is gitignored.
static void save_credentials(const std::string& username, const std::string& password) {
    std::string path = exe_dir() + "config.ini";
    std::vector<std::string> lines;
    bool have_user = false, have_pass = false;
    if (FILE* fp = fopen(path.c_str(), "r")) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            if      (line.rfind("username=", 0) == 0) { line = "username=" + username; have_user = true; }
            else if (line.rfind("password=", 0) == 0) { line = "password=" + password; have_pass = true; }
            lines.push_back(std::move(line));
        }
        fclose(fp);
    }
    if (!have_user) lines.push_back("username=" + username);
    if (!have_pass) lines.push_back("password=" + password);
    if (FILE* out = fopen(path.c_str(), "w")) {
        for (const auto& l : lines) { fputs(l.c_str(), out); fputc('\n', out); }
        fclose(out);
    }
}

// ── App state machine ─────────────────────────────────────────────────────────

enum class AppState {
    CREDENTIAL_PROMPT, // entering username / password
    CONNECTING,        // auth in progress
    LOBBY,             // authenticated, idle
    MATCH_MENU,        // match settings menu open
    SEARCHING,         // automatch in queue
    CORR_LIST,         // browsing my ongoing correspondence games ("MY GAMES")
    CHALLENGES,        // browsing incoming challenges (accept / decline)
    CHALLENGE_CREATE,  // composing a challenge to send (opponent + game settings)
    WHO_WON,           // quiz: guess the winner of a finished professional position
    PLAYING,           // live game active
    STONE_REMOVAL,     // game ended, awaiting stone removal acceptance
    GAME_OVER,         // game finished, showing result
    PUZZLE_BROWSE,     // browsing OGS puzzle collections / puzzle lists
    PUZZLE_PLAY,       // solving an OGS puzzle against its authored solution tree
    JOSEKI,            // walking the OGS Joseki Explorer position graph
};

// Live game state
struct LiveGame {
    // History of board states after each stone placement (index 0 = start of game)
    std::vector<GameState> history;
    int history_pos = -1;  // -1 = live view, >=0 = reviewing history[history_pos]
    int         game_id     = 0;
    int         board_size  = 19;
    int         my_color    = 1;   // 1=black, 0=white
    int         my_player_id = 0;
    std::string black_name, white_name;
    std::string black_rank, white_rank;
    int         black_secs        = -1;
    int         white_secs        = -1;
    int         black_periods     = -1;  // -1 = not byo-yomi
    int         white_periods     = -1;
    int         black_period_secs = -1;
    int         white_period_secs = -1;
    bool        black_in_byo      = false;  // main time gone — living on byo-yomi periods
    bool        white_in_byo      = false;
    Uint32      clock_tick  = 0;   // SDL_GetTicks() when clock last updated
    bool        my_turn     = false;
    int         pending_col = -2;  // -2=none, -1=pass pending echo, >=0=move pending echo
    int         pending_row = -2;
    int         handicap    = 0;
    bool        free_handicap = false;
    // Live-play desync guards (see the OPPONENT_MOVE handler). Colour and turn are
    // derived from the server's authoritative move_number, not the running toggle,
    // so a single missed/echoed move can't invert every stone thereafter.
    int         initial_player   = 1;   // who plays move 1: 1=black, 0=white — parity anchor
    int         last_move_number = 0;   // server move_number of the last move applied to the board
    Uint32      resync_req_at    = 0;   // SDL ticks of the last gamedata re-request (debounce)
    // clock.current_player — the server's own answer to "whose move is it", carried on
    // every clock event. Cross-checked against my_turn as a last line of defence: if
    // they disagree for long enough, a live move went missing and the board is dead.
    int         server_turn_player = 0;
    Uint32      turn_mismatch_at   = 0; // ticks when they first disagreed (0 = in agreement)
    int         cursor_r    = 9;
    int         cursor_f    = 9;
    GameState   board;
    std::string result;
    // Stone removal phase overlay
    bool dead_stones[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    int  ownership[MAX_BOARD_SIZE][MAX_BOARD_SIZE]   = {};
};

// ── Lobby screensaver: minimal SGF parser ────────────────────────────────────

struct SgfGame {
    char  moves[MAX_MOVES][MOVE_TEXT_LEN] = {};
    int   colors[MAX_MOVES]               = {};
    int   move_count                      = 0;
    int   setup_count                     = 0;  // leading entries from AB[]/AW[] setup stones, not real moves
    int   start_black                     = 1;  // from PL[] property: who's to move in the starting position
    bool  has_pl                          = false; // true if PL[] was present (start_black is authoritative)
    int   board_size                      = BOARD_SIZE;
    char  black_name[NAME_LEN]            = "Black";
    char  white_name[NAME_LEN]            = "White";
    char  result[32]                      = {};
    char  date[32]                        = {};    // from DT[] property
    float komi                            = 7.5f;  // from KM[] property
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
    g.move_count = 0;
    g.board_size = BOARD_SIZE;
    g.black_name[0] = g.white_name[0] = '\0';

    FILE* fp = Catalog::fopen_utf8(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    rewind(fp);
    if (fsz <= 0 || fsz > 8 * 1024 * 1024) { fclose(fp); return false; }
    std::vector<char> buf((size_t)fsz + 1);
    size_t nread = fread(buf.data(), 1, (size_t)fsz, fp);
    fclose(fp);
    buf[nread] = '\0';

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
    int  depth = 0;
    bool in_prop = false;
    bool prev_alpha = false;
    // For each parent depth d, true once we've followed the first '(' child.
    // A second '(' at the same parent depth is a variation branch and gets skipped.
    // This handles both standard SGF (variations at depth 2) and the OGS auto-save
    // format where every move after the second is wrapped in its own '(' pair,
    // pushing moves to progressively deeper levels — so depth can grow roughly as
    // large as the move count. Sized past MAX_MOVES with margin (not just 256, which
    // any real OGS game past ~255 moves would silently overflow — this function runs
    // on every catalog cursor move for the thumbnail preview, so that was a reliable
    // stack-corruption crash on scrolling to any sufficiently long game).
    static constexpr int MAX_SGF_DEPTH = MAX_MOVES + 16;
    bool branch_taken[MAX_SGF_DEPTH] = {};
    int  skip_depth = 0;   // >0 = we're inside a sibling variation; skip until depth drops below this

    while (*p) {
        if (in_prop) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == ']')  { in_prop = false; }
            if (*p && !isspace((unsigned char)*p)) prev_alpha = (isalpha((unsigned char)*p) != 0);
            p++;
            continue;
        }
        if (*p == '(') {
            depth++;
            if (depth >= MAX_SGF_DEPTH) {
                // Pathologically deep nesting (or a malformed/hostile file) — stop
                // parsing rather than overflow branch_taken[]. Whatever was already
                // parsed is kept; the remainder of the file is dropped.
                break;
            }
            if (skip_depth == 0) {
                if (branch_taken[depth - 1]) {
                    skip_depth = depth;          // sibling branch → skip
                } else {
                    branch_taken[depth - 1] = true;  // first child → follow main line
                }
            }
            prev_alpha = false; p++; continue;
        }
        if (*p == ')') {
            if (skip_depth > 0 && depth == skip_depth) skip_depth = 0;
            branch_taken[depth] = false;
            depth--;
            prev_alpha = false; p++; continue;
        }
        if (*p == '[') { in_prop = true; prev_alpha = false; p++; continue; }

        if (skip_depth > 0) {
            if (*p && !isspace((unsigned char)*p)) prev_alpha = (isalpha((unsigned char)*p) != 0);
            p++;
            continue;
        }

        // AB[...][...] / AW[...] — setup stones (handicap). Multi-value: AB[pd][dp]...
        if (*p == 'A' && (*(p+1) == 'B' || *(p+1) == 'W') && *(p+2) == '[' && !prev_alpha) {
            int ab_color = (*(p+1) == 'B') ? 1 : 0;
            p += 2;  // skip 'A', 'B'/'W' — p now points to first '['
            while (*p == '[') {
                p++;  // skip '['
                const char* start = p;
                while (*p && *p != ']') { if (*p == '\\') p++; if (*p) p++; }
                if (g.move_count < MAX_MOVES && p > start) {
                    size_t len = (size_t)(p - start);
                    if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                    memcpy(g.moves[g.move_count], start, len);
                    g.moves[g.move_count][len] = '\0';
                    g.colors[g.move_count] = ab_color;
                    g.move_count++;
                    g.setup_count++;
                }
                if (*p == ']') p++;
                while (*p && isspace((unsigned char)*p)) p++;  // skip whitespace between values
            }
            prev_alpha = false; continue;
        }
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
            prev_alpha = false; continue;
        }
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
            prev_alpha = false; continue;
        }
        if (depth == 1) {
            if (*p == 'S' && *(p+1) == 'Z' && *(p+2) == '[') {
                p += 3;
                int sz = (int)atoi(p);
                if (sz >= 2 && sz <= MAX_BOARD_SIZE) g.board_size = sz;
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                prev_alpha = false; continue;
            }
            if (*p == 'K' && *(p+1) == 'M' && *(p+2) == '[') {
                p += 3;
                float km = (float)atof(p);
                // Fox Go Server encodes Chinese-rules komi without a decimal point
                // (e.g. "375" meaning 3.75) — KataGo doesn't understand that raw
                // value, so translate that specific case to the equivalent komi
                // it expects.
                if (km == 375.f) km = 3.5f;
                g.komi = km;
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                prev_alpha = false; continue;
            }
            if (*p == 'P' && *(p+1) == 'L' && *(p+2) == '[') {
                p += 3;
                g.start_black = (*p == 'B') ? 1 : 0;
                g.has_pl      = true;
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                prev_alpha = false; continue;
            }
            if (*p == 'P' && *(p+1) == 'B' && *(p+2) == '[') {
                p += 3; p = read_val(p, g.black_name, sizeof(g.black_name));
                prev_alpha = false; continue;
            }
            if (*p == 'P' && *(p+1) == 'W' && *(p+2) == '[') {
                p += 3; p = read_val(p, g.white_name, sizeof(g.white_name));
                prev_alpha = false; continue;
            }
            if (*p == 'R' && *(p+1) == 'E' && *(p+2) == '[') {
                p += 3; p = read_val(p, g.result, sizeof(g.result));
                prev_alpha = false; continue;
            }
            if (*p == 'D' && *(p+1) == 'T' && *(p+2) == '[') {
                p += 3; p = read_val(p, g.date, sizeof(g.date));
                prev_alpha = false; continue;
            }
        }

        if (!isspace((unsigned char)*p)) prev_alpha = (isalpha((unsigned char)*p) != 0);
        p++;
    }

    if (g.black_name[0] == '\0') strcpy(g.black_name, "Black");
    if (g.white_name[0] == '\0') strcpy(g.white_name, "White");
    return g.move_count > 0;
}

// ── Catalog thumbnails ────────────────────────────────────────────────────────

static constexpr int THUMB_OPENING_MOVES = 16;

// Replay up to max_moves from an SGF and write the resulting board positions.
// Used for catalog preview thumbnails — avoids the full GameState overhead.
static bool sgf_board_at(const std::string& path,
                          char board_out[BOARD_SIZE][BOARD_SIZE],
                          int* board_size_out,
                          int max_moves = -1) {
    SgfGame g;
    if (!load_sgf(path, g)) return false;
    int sz = g.board_size;
    if (board_size_out) *board_size_out = sz;
    int limit = (max_moves < 0 || max_moves > g.move_count) ? g.move_count : max_moves;
    if (limit < g.setup_count) limit = g.setup_count;  // never truncate mid-setup
    char board[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    for (int i = 0; i < limit; i++) {
        int r, f;
        if (!parse_sgf_move(g.moves[i], r, f)) continue;
        if (r < 0 || r >= sz || f < 0 || f >= sz) continue;
        if (board[r][f] != 0) continue;
        int is_black = (g.colors[i] == 1);
        if (GoRules::would_be_suicide(board, r, f, is_black, sz)) continue;
        board[r][f] = is_black ? 1 : 2;
        int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_count = 0;
        GoRules::find_captured(board, is_black, r, f, cap_r, cap_f, cap_count, sz);
        for (int j = 0; j < cap_count; j++) board[cap_r[j]][cap_f[j]] = 0;
    }
    // Copy row-by-row: internal buffer has MAX_BOARD_SIZE stride, output has BOARD_SIZE stride
    memset(board_out, 0, BOARD_SIZE * BOARD_SIZE);
    int copy_sz = sz < BOARD_SIZE ? sz : BOARD_SIZE;
    for (int r = 0; r < copy_sz; r++)
        memcpy(board_out[r], board[r], copy_sz);
    return true;
}

// ── Analysis tree ─────────────────────────────────────────────────────────────

struct AnalysisNode {
    GameState board;
    int   move_col   = -1;   // stone played to reach this node (-1 = root or pass)
    int   move_row   = -1;
    int   move_color = -1;   // 1=black, 0=white, -1=root
    int   depth      = 0;    // distance from root
    int   active_child = 0;  // which child RT navigates into
    float score_lead = FLT_MAX;  // KataGo score lead from Black's perspective; FLT_MAX = unknown
    // True for nodes that are part of the actual played game (children[0] chain from
    // root, set once by build_analysis_tree() and never altered). False for nodes
    // created by branching off into a hypothetical line. Distinct from active_child,
    // which just tracks navigation and gets repointed at whichever branch you last
    // explored — is_main_line is the stable, structural "is this the real game" fact.
    bool  is_main_line = true;
    // Marks this position as a correct ending of a life-and-death drill line
    // (rendered as a green halo in the tree panel; serialized as C[RIGHT]).
    bool  drill_correct = false;
    // Letter annotations ("A", "B", …) placed with the circle button — owned by the
    // position they were placed on, so navigating away and back restores them.
    std::vector<BoardLabel> labels;
    std::vector<std::unique_ptr<AnalysisNode>> children;
    AnalysisNode* parent = nullptr;
};

// ── Application ───────────────────────────────────────────────────────────────

class App {
public:
    int run();

private:
    SDL_Window*         window_   = nullptr;
    SDL_Renderer*       sdl_rend_ = nullptr;
    SDL_GameController* pad_      = nullptr;
    Renderer*           renderer_ = nullptr;

    AppState    state_    = AppState::CONNECTING;
    OgsNet      net_;
    LiveGame    game_;
    Sound       sound_;

    // Game catalog for reviewing saved OGS games
    Catalog      catalog_;
    bool         catalog_delete_confirm_ = false;  // Y pressed once, awaiting confirm
    // True while browsing the pro game library (open_pro_catalog()) — blocks the
    // Y-button delete path so a stray double-press can never remove a curated
    // professional-game SGF. False for the user's own games/<username>/ catalog.
    bool         catalog_readonly_ = false;
    std::string  my_username_;   // from config.ini; used to locate games/<name>/ dir

    // Catalog thumbnails (BOARD_SIZE stride matches DrawState::catalog_thumb_open type)
    std::string  thumb_path_;
    char         thumb_open_ [BOARD_SIZE][BOARD_SIZE] = {};
    char         thumb_final_[BOARD_SIZE][BOARD_SIZE] = {};
    bool         thumb_valid_      = false;
    bool         thumb_single_     = false;  // opening == final → show one board, not two
    int          thumb_board_size_ = BOARD_SIZE;

    void update_catalog_thumb() {
        std::string sel = catalog_.selected_entry_path();
        if (sel == thumb_path_) return;
        thumb_path_  = sel;
        thumb_valid_ = !sel.empty()
            && sgf_board_at(sel, thumb_open_,  &thumb_board_size_, THUMB_OPENING_MOVES)
            && sgf_board_at(sel, thumb_final_,  nullptr);
        // Single-position files (marked positions, study puzzles — all setup stones,
        // no moves) produce identical opening/final snapshots; drawing both is just
        // the same picture twice. Board equality is the cheapest reliable test.
        thumb_single_ = thumb_valid_ &&
                        memcmp(thumb_open_, thumb_final_, sizeof(thumb_open_)) == 0;
    }

    // Rows in each settings-menu column, and how many columns exist, for the current
    // mode. The single authority on which controls exist — pad navigation and mouse
    // hit-testing both validate against this, so neither can invent a row.
    void match_menu_cols(int out_sizes[3], int& col_count) const {
        const int DISPLAY_ROWS = 12; // SHOW COORDINATES, ENGINE ANALYSIS, CHAIN LINKS,
                                     // SQUARE STONES, OCTAGON STONES, SQUARE GRID,
                                     // SHOW TERRITORY, SHOW NEXT MOVE, TETRIS CAPTURES,
                                     // BOARD HUE, BOARD SAT, BOARD VAL
        out_sizes[0] = out_sizes[1] = out_sizes[2] = 0;
        if (match_menu_.ingame) {
            col_count    = 1;
            out_sizes[0] = DISPLAY_ROWS;
        } else if (match_menu_.katago_mode) {
            // KATAGO SETTINGS: board size + strength only, no DISPLAY column.
            col_count    = 2;
            out_sizes[0] = 3;
            out_sizes[1] = 9;   // 8 fixed ranks + ADAPTIVE
        } else {
            col_count    = 3;
            out_sizes[0] = 3;
            out_sizes[1] = 3;   // speeds
            out_sizes[2] = DISPLAY_ROWS;
        }
    }

    // Settings-menu control under a screen point. Geometry comes from the renderer;
    // validity against match_menu_cols. Returns false if the point isn't on a control.
    bool match_menu_hit(int mx, int my, int& col, int& row) const {
        int c, r;
        if (!renderer_->match_menu_cell_at(match_menu_, mx, my, c, r)) return false;
        int sizes[3], col_count;
        match_menu_cols(sizes, col_count);
        if (c >= col_count || r >= sizes[c]) return false;
        col = c; row = r;
        return true;
    }

    // Shared by L1/R1 (and their keyboard mirrors), the [-]/[+] buttons, and the
    // plain-click-raises fallback — the one place that knows which DISPLAY row
    // maps to which Palette setter, so those three input paths can't drift apart.
    void adjust_board_hsv(int row, int delta) {
        if      (row ==  9) Palette::set_board_hue(Palette::BOARD_HUE + delta);
        else if (row == 10) Palette::set_board_sat(Palette::BOARD_SAT + delta);
        else if (row == 11) Palette::set_board_val(Palette::BOARD_VAL + delta);
    }

    // Move the catalog selection, keep it scrolled into view, and refresh the
    // preview thumbnail. Shared by pad navigation and mouse clicks so this scroll
    // bookkeeping exists in exactly one place.
    void set_catalog_index(int i) {
        const int CAT_VIS = 15;
        int n = (int)catalog_.entries.size();
        if (n == 0) return;
        catalog_.index  = std::max(0, std::min(n - 1, i));
        catalog_.scroll = std::min(catalog_.scroll, catalog_.index);
        if (catalog_.index >= catalog_.scroll + CAT_VIS)
            catalog_.scroll = catalog_.index - CAT_VIS + 1;
        update_catalog_thumb();
    }

    // Catalog row under a screen point, or -1. Tests against the layout the last
    // draw published, so it stays cheap no matter how many entries are listed.
    int catalog_hit(int mx, int my) const {
        if (!catalog_.active) return -1;
        return renderer_->catalog_entry_at(mx, my);
    }

    // DrawState anchors (refs must point to persistent objects)
    std::string  empty_str_;
    // Formatted player labels including rank, e.g. "Alice [5d]"
    std::string  black_label_, white_label_;
    AnalysisState* dummy_analysis_ = nullptr;

    // Credential prompt text input
    int         cred_step_ = 0;  // 0=off, 1=username, 2=password
    std::string cred_username_, cred_password_, cred_buf_;
    bool        cred_from_prompt_ = false;  // set on submit; save creds once AUTH_OK confirms them
    // Skipped the login (ESC at the prompt): no network thread is running, so the
    // lobby hides everything that needs an OGS account and offers LOG IN instead.
    // Review, analysis, puzzles and joseki only need public data or local files.
    bool        offline_ = false;
    void continue_offline();
    void return_to_login();

    // Status line (changes with state)
    std::string status_;
    std::string hist_status_;    // scratch: "MOVE N/M" while reviewing history
    std::string themed_status_;  // scratch: status with pad labels themed for render

    // Double-press confirms (pass on circle, mark on triangle, find-match on cross)
    bool pass_confirm_       = false;
    bool mark_confirm_       = false;
    bool find_match_confirm_ = false;

    // Flash message
    std::string flash_;
    Uint32      flash_until_    = 0;
    // Set when a list screen painted flash_ into its footer. Those screens repaint
    // only on input, so without a nudge an expired flash would sit there stale.
    bool        flash_on_list_  = false;
    Uint32      ko_flash_until_   = 0;
    Uint32      kata_query_after_      = 0;    // deferred KataGo query — fires after user settles
    bool        kata_analysis_enabled_ = true; // toggled in the settings menu (BACK)
    float       review_komi_          = 7.5f; // komi used for GAME_OVER analysis queries;
                                               // set from a loaded SGF's KM[], else default

    // Match settings
    MatchPrefs            match_prefs_;
    Renderer::MatchMenu   match_menu_;
    AppState              pre_menu_state_ = AppState::LOBBY;  // state to return to when closing an in-game menu

    // Persisted across sessions to settings.txt: match_prefs_ (board size/speed/
    // KataGo mode/strength) plus the DISPLAY column toggles (show_coords_,
    // kata_analysis_enabled_, chain_mode_, square_stones_, octagon_stones_,
    // square_grid_) and the
    // board HUE/SAT/VAL (Palette::BOARD_HUE/SAT/VAL — global engine state, not an
    // App member, so it needs no mirror field here). Loaded once at startup; saved
    // whenever the settings menu closes, since that's already the single commit
    // point match_prefs_ itself uses (save_prefs()) — by then any DISPLAY toggles
    // pressed during this visit are live too.
    void load_settings();
    void save_settings();

    // Lobby screensaver: auto-plays a game from the games/ directory.
    // Moves are applied one per second into a single GameState — no bulk copying.
    struct DemoState {
        std::vector<int> rows, cols, colors;  // move list loaded from SGF
        int    pos        = 0;
        int    board_size = BOARD_SIZE;
        float  komi       = 7.5f;   // from KM[]; handicap games aren't 7.5
        std::string black_name, white_name;
        Uint32 next_tick  = 0;
        GameState board;  // single board, updated move-by-move

        // End-of-game territory fill. A catalog game almost always ends in a
        // resignation, so there's no scoring phase to borrow — instead the last
        // move is followed by a KataGo ownership query on the final position, and
        // the answer paints every point (and every stone the map contradicts) in
        // its owner's colour, the same fully-covered Chinese-rules board the
        // stone-removal phase draws for the user's own games. Then it holds there
        // long enough to actually read before the next game starts.
        bool   end_phase  = false;  // last move played; querying or holding
        bool   fill_ready = false;  // ownership[] valid — board is drawn filled
        int    ownership[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};  // 1=black, -1=white, 0=neutral
        bool   dead[MAX_BOARD_SIZE][MAX_BOARD_SIZE]      = {};  // stones the map says belong to the other side
        // Chinese count of the filled board: stones of each colour once the fill is
        // applied. Raw counts, no komi — the two numbers the finished board shows.
        int    area_black = -1;
        int    area_white = -1;
    } demo_;
    bool demo_active_ = false;
    // Hold after the fill appears, and the cap on waiting for KataGo to answer
    // (it may not be running at all, in which case the final position just sits
    // there for the hold and the screensaver moves on).
    static constexpr Uint32 DEMO_FILL_HOLD_MS = 8000;
    static constexpr Uint32 DEMO_FILL_WAIT_MS = 10000;
    void demo_begin_end_fill();
    void demo_clear_end_fill();
    void demo_next_game();

    // Scoped screensaver playlist — set by start_catalog_autoplay() (X in the
    // catalog, on a game or a directory/player/year entry) to play a specific
    // ordered list of games in the lobby instead of the default random pick.
    // -1 = no scoped playlist (default, untouched random screensaver).
    std::vector<std::string> demo_playlist_;
    int                       demo_playlist_pos_ = -1;
    void start_catalog_autoplay(int start_at_catalog_index);

    // Visual links between chained stones — toggled in the settings menu's DISPLAY column
    bool chain_mode_       = true;
    // Stones drawn as beveled square tiles instead of circles (DISPLAY toggle, off by default)
    bool square_stones_    = false;
    // Stones drawn as beveled octagonal tiles filling the whole cell, so orthogonal
    // neighbours meet edge to edge (DISPLAY toggle, off by default). Mutually
    // exclusive with square_stones_ — a stone has one shape.
    bool octagon_stones_   = false;
    // Points sit at the centre of a checkerboard cell instead of a line crossing
    // (DISPLAY toggle, off by default) — see renderer.cpp's render_board_content.
    bool square_grid_      = false;
    // Captured stones blink a few times and vanish, early-Tetris line-clear style
    // (DISPLAY toggle, off by default) — see renderer.cpp's update_capture_anim.
    bool tetris_captures_  = false;
    // Yellow ring on the move actually played next, during GAME_OVER review
    // (DISPLAY toggle, off by default) — working out the next move yourself is
    // most of the point of the review screen, and the ring hands you the answer
    // before you've had a chance to look. Off, it blanks exactly as guess mode
    // already blanks it.
    bool show_next_move_   = false;

    // Live territory overlay during play (DISPLAY toggle, off by default) — a
    // KataGo ownership guess redrawn after every move, for demo/teaching games
    // where showing the audience "who's ahead where" is more useful than
    // waiting for stone removal. Shares kata_/kata_9_'s single query slot with
    // the background score-graph sweep and GAME_OVER analysis, so it's debounced
    // like kata_query_after_ and yields to any query already in flight.
    bool   show_live_ownership_  = false;
    Uint32 live_own_query_after_ = 0;   // 0 = none pending
    bool   live_own_pending_     = false;
    Uint32 live_own_started_at_  = 0;   // for the same lost-response timeout bg_analysis_busy_ uses
    void   request_live_ownership();

    // Freehand chalk annotation (SPACE toggles). Screen-space and position-agnostic
    // — like drawing straight on the monitor — so nothing here is keyed to a move or
    // saved with the game; the pixels live in the renderer's own layer.
    bool draw_mode_  = false;
    bool chalk_dark_ = false;  // SHIFT toggles: near-black stroke instead of white
    bool pen_down_  = false;   // left button held while in draw mode
    int  pen_x_     = 0;       // last stamped point, for segment continuity
    int  pen_y_     = 0;
    // Current pointer position, tracked from motion events (not SDL_GetMouseState)
    // so it's in the same coordinate space as pen_x_/pen_y_ and the hit-tests.
    // Drives the chalk crosshair cursor in drawing mode.
    int  mouse_x_   = -1;
    int  mouse_y_   = -1;
    void toggle_draw_mode();

    // Help overlay / quit confirm
    bool show_help_      = false;
    bool quit_confirm_   = false;
    bool quit_requested_ = false;  // set by the menu's QUIT item; the event loop exits

    bool cursor_shown_ = false;  // OS cursor visible? shown on mouse use, hidden for pad/keyboard

    // Board-edge coordinate labels toggle (RT during live play)
    bool show_coords_  = false;

    // START popup action menu: a context-sensitive command list for the current
    // state (resign, return to lobby, catalog, …) navigated with the dpad and
    // cross. Destructive items set `confirm` — the first press rearms the row as
    // "REALLY …?", the second executes. Closed by circle/OPTIONS, and by any
    // network event that changes the app state out from under it.
    struct PopupItem {
        std::string           label;
        bool                  confirm = false;
        std::function<void()> action;
        // Section grouping. A heading is a label with no action — it is drawn in
        // `color` and skipped by every form of selection. Command rows carry the
        // muted tint of the section they belong to.
        bool                  heading = false;
        SDL_Color             color   = Palette::TEXT_WHITE;
    };
    std::vector<PopupItem>   popup_items_;
    std::vector<std::string> popup_labels_;  // display labels (armed row shows REALLY …?)
    std::vector<PopupStyle>  popup_styles_;  // per-row colour/heading, or empty when flat
    std::string popup_title_;
    bool popup_active_ = false;
    int  popup_index_  = 0;
    int  popup_armed_  = -1;   // item index awaiting its confirm press; -1 = none
    void open_popup_menu();
    void close_popup_menu();
    void popup_sync_labels();
    void handle_popup_button(Uint8 btn);
    // Nearest selectable row at or after `from`, stepping by `dir` and wrapping.
    // Section headings are not selectable, so every move of the highlight —
    // opening the menu included — goes through here rather than ±1.
    int  popup_step(int from, int dir) const;
    // Draw the popup over whatever is already on the backbuffer. The list
    // screens all layer it the same way, and it has to carry the same styles
    // the hit-test uses, so there is one copy of the call.
    void draw_popup_overlay();
    // Mouse hit-test against the open popup, in the same terms make_ds draws it.
    // Returns the item index under the point, or -1; `inside_panel` distinguishes
    // "missed a row" from "clicked off the panel entirely" (which dismisses).
    int  popup_hit(int mx, int my, bool* inside_panel = nullptr) const;
    // Actions shared by the popup and direct button bindings
    void do_resign();
    void return_to_lobby();
    void accept_stone_removal();

    // Last-played stone position (for row/col crosshair highlight)
    int last_move_r_ = -1;
    int last_move_f_ = -1;

    // Undo-request state
    bool undo_pending_     = false;
    int  undo_move_number_ = 0;

    // Stone removal: true once OGS has sent territory data; prevents KataGo from overwriting it
    bool stone_removal_has_ogs_territory_ = false;
    // True after user presses A but before server confirms our acceptance
    bool        my_accept_sent_           = false;
    Uint32      accept_resend_at_         = 0;   // next time to proactively resend cmd_accept_stones
    std::string stone_removal_all_removed_;  // all_removed string from last removed_stones event
    // Toggled with Triangle/Y during STONE_REMOVAL: replaces the colored territory
    // squares with the board fully filled in with real stones — dead-stone cells and
    // empty territory both get the owning color's stone, like Chinese-rules scoring.
    bool        stone_removal_fill_view_  = false;

    // Post-game analysis tree (valid in GAME_OVER state)
    std::unique_ptr<AnalysisNode>           analysis_root_;
    AnalysisNode*                           analysis_cur_ = nullptr;
    std::vector<AnalysisTreeRenderNode>     analysis_tree_render_;

    // KataGo subprocesses: kata_ = general model, kata_9_ = 9x9-specialist model
    KatagoProc     kata_;
    KatagoProc     kata_9_;
    MoveSuggestion kata_suggestions_[MAX_SUGGESTIONS] = {};
    int            kata_suggestion_count_             = 0;
    float          kata_score_lead_               = FLT_MAX;  // FLT_MAX = no data

    // Score graph: KataGo score_lead per main-line depth, built during play and review.
    std::vector<float> move_scores_;           // indexed by depth; FLT_MAX = unknown
    std::vector<bool>  move_marked_;           // indexed by depth; true = flagged for review
    // Path of the standalone SGF written for a marked depth this session (empty = none/unknown —
    // e.g. marks restored from a companion file on load have no recorded path to clean up).
    std::vector<std::string> marked_paths_;
    int                bg_analysis_next_  = 0; // next main-line depth to submit
    int                bg_analysis_depth_ = -1;// depth of the currently-pending bg query
    bool               bg_analysis_busy_  = false;
    Uint32             bg_analysis_started_at_ = 0;  // for the stuck-query timeout below
    bool               fg_kata_pending_   = false; // foreground KataGo query in-flight
    std::string        companion_path_;            // path of .katago companion file

    // Auto-detected study puzzles: positions where exactly one move kept the game
    // and it wasn't played. Top-2 of each background-scoring response is kept here
    // (the sweep already queries every position; we previously discarded all of it
    // except the root score), then evaluated once the game is over — never during
    // play, so no engine information can leak into a live game.
    struct PuzzleEval {
        float best_sl   = FLT_MAX;   // best suggestion's scoreLead (Black's perspective)
        float second_sl = FLT_MAX;   // second-best; FLT_MAX = KataGo offered no alternative
        int   best_r = -1, best_f = -1;
    };
    std::map<int, PuzzleEval> puzzle_eval_;   // keyed by depth (position before the move)
    std::set<int>             puzzle_saved_;  // depths already written to puzzles/
    void check_puzzle(int d);
    void save_puzzle_position(int depth);

    // Path of the SGF currently open for review (catalog-loaded only; empty after a
    // live game). Enables L3/R3 cycling through sibling files without the catalog.
    std::string review_path_;
    void review_cycle(int dir);
    // Peek at the next/prev sibling .sgf in review_path_'s directory (alphabetical,
    // wraps) without loading it — shared by review_cycle() and next_review_sibling's
    // callers. Empty string = fewer than 2 sibling files. out_index/out_total
    // (1-based / count), if non-null, receive the target's position in the listing.
    std::string next_review_sibling(int dir, int* out_index = nullptr, int* out_total = nullptr) const;
    // Advance analysis_cur_ one ply along the active-child main line — shared by
    // the L2/R2 step-forward button handler.
    void analysis_step_forward();

    // ── Guess the move (GAME_OVER review of a pro game) ───────────────────────
    // Aim with the board cursor, press cross to commit; the guess scores by ring
    // distance from the move actually played (see guess_points() in renderer.hpp)
    // and the review then steps forward onto that move. Only meaningful on a
    // loaded file — a live game's own review has no moves left to guess.
    bool guess_mode_        = false;
    int  guess_score_       = 0;
    int  guess_count_       = 0;
    int  guess_last_points_ = -1;   // -1 = nothing guessed yet
    int  guess_last_dist_   = -1;
    bool guess_available() const {
        return state_ == AppState::GAME_OVER && !review_path_.empty()
               && analysis_cur_ && !analysis_cur_->children.empty();
    }
    void toggle_guess_mode();
    void guess_commit();
    // Scoreboard text for the status line (the live client's gutter is the status
    // line's, so the guess HUD reports through it rather than drawing its own).
    std::string guess_status() const;

    // ── OGS puzzle browsing / solving ─────────────────────────────────────────
    // Fetches run on detached worker threads (blocking libcurl); results land in a
    // shared buffer and wake the event loop with the net event. A superseded fetch's
    // buffer just gets dropped when its shared_ptr no longer matches pz_fetch_.
    struct PuzzleFetch {
        std::atomic<bool> ready{false};
        bool ok   = false;
        int  kind = 0;  // 1=collections page, 2=puzzle, 3=collection siblings
        std::vector<OgsPuzzleCollection> collections;
        int  total = 0;
        OgsPuzzle puzzle;
        std::vector<std::pair<int, std::string>> siblings;
        std::string err;   // goproblems: why the problem couldn't be used
    };
    std::shared_ptr<PuzzleFetch> pz_fetch_;
    bool pz_loading_ = false;

    enum class PzView { COLLECTIONS, PUZZLES };
    PzView pz_view_ = PzView::COLLECTIONS;
    static constexpr int PZ_PAGE_SIZE = 15;
    std::vector<OgsPuzzleCollection> pz_collections_;   // current page
    int  pz_col_total_ = 0;
    int  pz_col_page_  = 1;                             // 1-based
    std::vector<std::pair<int, std::string>> pz_list_;  // puzzles in open collection
    std::string pz_list_title_;
    int  pz_index_  = 0;                                // browser cursor (per view)
    int  pz_hover_  = -1;                               // mouse-hovered row (-1 = none)
    int  pz_lines_drawn_ = 0;   // rows the last draw handed the list screen — the
                                // hit-test must use the same count to agree with it
    int  pz_item_count() const;                 // real entries in the current view
    int  pz_hit(int mx, int my) const;          // list row under a screen point, or -1
    int  pz_list_pos_ = -1;                             // playing puzzle's index in pz_list_

    // Puzzles solved across sessions (ids; persisted to solved_puzzles.txt) —
    // shown as [X] checkmarks in the puzzle list.
    std::set<int> pz_solved_ids_;
    // Puzzle id → collection id, driving the collections-list coloring (yellow =
    // started, green = every puzzle solved). 0 = unknown: solves recorded before
    // this mapping existed; backfilled whenever their collection is next opened.
    // Persisted as a second column in solved_puzzles.txt.
    std::map<int, int> pz_solved_col_;
    int pz_open_col_id_ = 0;   // collection id of the pending/most recent list fetch
    void load_solved_puzzles();
    void save_solved_puzzles();
    // Collection id → how many of its puzzles are solved (from pz_solved_col_)
    std::map<int, int> pz_solved_per_collection() const {
        std::map<int, int> counts;
        for (const auto& kv : pz_solved_col_)
            if (kv.second > 0) counts[kv.second]++;
        return counts;
    }
    // Index in pz_list_ of the furthest-down solved puzzle (0 if none) — where a
    // freshly opened collection parks the cursor so each visit resumes there.
    int pz_last_solved_index() const {
        for (int i = (int)pz_list_.size() - 1; i >= 0; i--)
            if (pz_solved_ids_.count(pz_list_[i].first)) return i;
        return 0;
    }
    // Every collection ever opened, persisted to puzzle_collections.txt — the
    // metadata source for the pinned MY SETS section, which must be able to show
    // (and reopen) in-progress collections that live on OGS pages not currently
    // fetched. Recorded/refreshed each time a collection is opened.
    std::map<int, OgsPuzzleCollection> pz_known_cols_;
    void load_known_collections();
    void save_known_collections();
    // What the COLLECTIONS view actually shows and indexes: pinned in-progress
    // sets (name order) first, then the current server page minus duplicates.
    std::vector<OgsPuzzleCollection> pz_display_cols_;
    void pz_rebuild_display();

    // goproblems.com download (problem_sets/<folder> holding goproblems_index.tsv),
    // browsed easiest-first from that index. The list is an ordinary PUZZLES view
    // — pz_list_ ids are GP_ID_BASE-offset, which pz_launch_fetch routes to the
    // local SGF — parallel to gp_index_ so LEFT/RIGHT can jump between rank
    // levels. Its pin row carries sentinel id -4.
    std::string gp_dir_;            // found by scan_local_books(), "" = none
    std::vector<GpIndexEntry> gp_index_;
    std::string gp_index_dir_;      // folder gp_index_ was loaded from
    bool gp_list_open() const {
        return pz_view_ == PzView::PUZZLES && local_pz_kind_ == LocalPzKind::NONE &&
               pz_open_col_id_ == GP_COLLECTION_ID;
    }
    void open_gp_list();
    // R3/L3 through goproblems can land on a problem the solver can't use (no
    // RIGHT line, unplayable moves): the failed fetch then steps on past it, in
    // the same direction, a few times before giving up.
    int pz_step_dir_    = 0;    // direction of the in-flight step, 0 = not a step
    int pz_step_target_ = -1;   // pz_list_ index that step is fetching
    int pz_step_skips_  = 0;

    OgsPuzzle             pz_;                 // puzzle being solved
    const PuzzleMoveNode* pz_node_  = nullptr; // current position in the solution tree
    bool        pz_done_   = false;            // solved or failed — judging over
    bool        pz_solved_ = false;
    bool        pz_explore_ = false;           // off the authored tree — free sandbox, no judging
    int         pz_explore_anchor_ = 0;        // history length to restore to on "back to solving"
    // Opponent-branch visit counts for the loaded puzzle: retries walk the LEAST
    // visited resistance line, so repeated attempts sweep every authored variation
    // in order instead of sampling randomly. Cleared when a new puzzle loads.
    std::map<const PuzzleMoveNode*, int> pz_visits_;
    bool pz_subtree_unexplored(const PuzzleMoveNode* n) const;  // any never-traversed node below?
    bool pz_more_lines_ = false;               // somewhere in the tree, a line hasn't been seen yet
    const PuzzleMoveNode* pz_pending_reply_ = nullptr;  // opponent reply chosen, awaiting the delay
    Uint32 pz_reply_at_ = 0;                            // SDL_GetTicks() deadline to apply it
    // Solution tree rendered in the left panel via the analysis-tree renderer
    std::vector<AnalysisTreeRenderNode> pz_tree_render_;
    int  pz_cur_depth_ = 0;
    void pz_build_tree_render();
    std::vector<BoardLabel> pz_marks_;         // current node's author marks (letter renderer)
    void pz_refresh_marks() {
        pz_marks_.clear();
        if (!pz_node_) return;
        for (const auto& m : pz_node_->marks)
            if (m.y >= 0 && m.y < game_.board_size && m.x >= 0 && m.x < game_.board_size)
                pz_marks_.push_back({m.y, m.x, m.ch});
    }
    std::string pz_banner_;                    // "SOLVED!" / "WRONG" big banner
    std::string pz_comment_;                   // author's comment at the current node

    // ── Life-and-death drill trees (local, user-authored) ─────────────────────
    // Locally-stored puzzle sets, all browsed/played the same way: drills
    // (authored in analysis mode via drill_correct marks + SAVE AS DRILL),
    // missed puzzles (auto-saved on a wrong OGS-puzzle attempt), and
    // favorites (manually toggled). Each lives in its own my_games/<user>/
    // subfolder as standard variation SGFs, drilled through the puzzle
    // player via a synthesized local OgsPuzzle.
    //
    // BOOK is the one that isn't personal: a problem set imported from a
    // printed collection by tools/import_problem_pdf.py, living under
    // problem_sets/<slug>/ with one folder per book. Unlike the other three
    // there can be many of them at once, so each gets its own pin — hence
    // local_book_, which says which one is open.
    enum class LocalPzKind { NONE, DRILLS, MISSED, FAVORITES, BOOK };
    LocalPzKind local_pz_kind_ = LocalPzKind::NONE;   // puzzle browser PUZZLES view lists this local set

    // One imported problem set. collection_id is the synthetic OGS-style id the
    // importer stamped into every SGF (ZC[]), which is what makes the app's
    // normal per-puzzle solved tracking work for book problems too. pin_id is
    // the negative sentinel its collection-list row carries, purely for routing.
    struct LocalBook {
        int         pin_id = 0;
        int         collection_id = 0;
        std::string dir, name;
    };
    std::vector<LocalBook> local_books_;    // rebuilt by scan_local_books()
    LocalBook              local_book_;     // the one currently open (BOOK kind)
    std::vector<std::string> drill_paths_;     // parallel to pz_list_ while local_pz_kind_ != NONE
    std::string drill_play_path_;              // file behind pz_ ("" = network OGS puzzle)
    std::string drill_edit_path_;              // overwrite target for SAVE ("" = save as new)
    // Setup-stone placement in analysis: -1 = off (normal alternating moves),
    // 1/0 = cross places/removes raw black/white stones on the CURRENT node's
    // board without creating tree nodes — for laying out a drill's initial
    // shape before recording any lines.
    int         analysis_setup_color_ = -1;
    // Which color the human will play when this drill is solved, independent of
    // which side's move-count parity moves first (see "SWITCH SIDE TO MOVE"):
    // -1 = unset (solver follows whoever moves first, today's behavior), 1/0 =
    // explicitly pinned to black/white via the "SOLVE AS ..." menu toggle. Reset
    // to -1 whenever a fresh analysis tree starts; restored from ZS[] when
    // re-editing a saved drill.
    int         drill_solver_black_ = -1;
    // Local-list preview: the selected file's setup position, parsed on
    // cursor move and cached (same BOARD_SIZE stride as the catalog thumbs).
    char        drill_thumb_[BOARD_SIZE][BOARD_SIZE] = {};
    int         drill_thumb_idx_ = -1;   // drill_paths_ index the cache holds (-1 = none)
    int         drill_thumb_bs_  = 0;    // 0 = nothing valid to draw
    // Renaming a file (keyboard text entry in a local list), same
    // plain-keycode capture idiom as the credential prompt
    bool        drill_rename_active_ = false;
    std::string drill_rename_buf_;
    void        drill_commit_rename();
    void        drill_delete_selected();
    void        delete_analysis_branch();      // remove analysis_cur_'s subtree (hypothetical only)
    std::string drills_dir(bool create);
    std::string missed_puzzles_dir(bool create);
    std::string favorite_puzzles_dir(bool create);
    std::string current_local_pz_dir(bool create);   // resolves by local_pz_kind_
    void        scan_local_books();                  // refresh local_books_ from problem_sets/
    bool save_drill(const std::string& path);
    void save_drill_from_current();
    void open_local_pz_list(LocalPzKind kind);
    void local_pz_load_and_start(int idx);
    void drill_edit_current();
    bool save_missed_puzzle();   // returns true if newly saved (false = already had it)
    void toggle_favorite_puzzle();

    void open_puzzle_browser();
    void pz_launch_fetch(int kind, int arg);   // spawn worker for one of the 3 fetchers
    void poll_puzzle_fetch();                  // consume a finished fetch (event loop)
    void pz_start();                           // (re)set the board to pz_'s initial position
    void pz_place(int r, int f);               // player move → tree matching + feedback
    void pz_advance(const PuzzleMoveNode* node, bool opponent_follows);
    const PuzzleMoveNode* pz_pick_reply(const PuzzleMoveNode* node);  // least-visited-first branch choice
    void pz_fire_pending_reply();               // apply the delayed opponent reply, then keep judging
    void pz_enter_explore();                   // triangle: free sandbox from the current position
    void pz_return_to_solving();               // circle, mid-exploration: back to the judged anchor
    void pz_step(int dir);                     // prev/next puzzle within the collection
    void handle_puzzle_button(Uint8 btn);      // PUZZLE_BROWSE + PUZZLE_PLAY input
    void draw_puzzle_browser();

    // ── Joseki explorer (OGS "OJE" position graph) ────────────────────────────
    // Walked node by node from "root"; every visited node is kept on a path
    // stack with a parallel board snapshot, so stepping back is instant and
    // re-advancing hits the in-memory cache instead of the network.
    struct JosekiFetch {
        std::atomic<bool> ready{false};
        bool ok = false;
        std::string    node_id;
        JosekiPosition pos;
    };
    std::shared_ptr<JosekiFetch> jk_fetch_;
    bool jk_loading_ = false;
    std::vector<JosekiPosition> jk_path_;    // root .. current node
    std::vector<GameState>      jk_boards_;  // board after each path node
    std::map<std::string, JosekiPosition> jk_cache_;   // node_id → fetched node
    std::string              jk_comment_;    // scrubbed description of current node
    std::vector<PointMarker> jk_markers_;    // continuation dots for current node
    void open_joseki_explorer();
    void jk_fetch_node(const std::string& node_id);
    void poll_joseki_fetch();
    void jk_arrive(const JosekiPosition& pos);  // push node + apply its move
    void jk_show_current();                     // markers/comment/status refresh
    void jk_step_back();
    void handle_joseki_button(Uint8 btn);

    // Returns the best available KataGo process for the given board size.
    KatagoProc& kata_for(int bs) {
        return (bs == 9 && kata_9_.running()) ? kata_9_ : kata_;
    }

    // KataGo GTP subprocess for local games vs the human SL model
    KataGoGtp   kata_gtp_;
    std::string kata_exe_;          // path from config (shared with analysis)
    std::string kata_model_;        // path from config (shared with analysis)
    std::string kata_human_model_;  // path to humanv0.bin.gz (enables VS KATAGO)

    // Adaptive KataGo strength: fractional rank index (0=20k … 19=1k, 20=1d … 28=9d),
    // nudged up after each win and down after each loss (scaled by the score margin),
    // persisted to adaptive_level.txt so it tracks Boris's level across sessions.
    float adaptive_rank_ = 10.0f;   // starting point: 10 KYU
    bool  adaptive_game_ = false;   // current local game uses the adaptive profile
    void  load_adaptive();
    void  save_adaptive();
    void  update_adaptive(const std::string& result);

    // Correspondence game state
    std::vector<CorrGameSummary> corr_games_;   // rows in the MY GAMES list
    int  corr_index_       = 0;                  // list cursor
    int  corr_hover_       = -1;                 // mouse-hovered row (-1 = none)
    int  corr_lines_drawn_ = 0;                  // rows the last draw handed the list screen
    bool corr_loading_     = false;              // an overview fetch is in flight
    bool is_corr_game_     = false;              // the open game came from the corr list
    void open_corr_list();      // enter CORR_LIST and (re)fetch the overview
    void refresh_corr_list();   // kick off an overview fetch without changing state
    void draw_corr_list();
    void handle_corr_button(Uint8 btn);
    void leave_corr_game();     // disconnect the open corr game, return to MY GAMES
    int  corr_hit(int mx, int my) const;  // list row under a screen point, or -1

    // Incoming challenges (accept / decline)
    std::vector<ChallengeSummary> challenges_;
    int  chal_index_          = 0;
    int  chal_hover_          = -1;
    int  chal_lines_drawn_    = 0;
    bool chal_loading_        = false;
    bool chal_accept_confirm_ = false;   // A armed once, awaiting the confirm press
    bool chal_decline_confirm_= false;   // Y armed once, awaiting the confirm press
    void open_challenges();     // enter CHALLENGES and fetch the list
    void refresh_challenges();  // (re)fetch me/challenges on a worker thread
    void draw_challenges();
    void handle_chal_button(Uint8 btn);
    int  chal_hit(int mx, int my) const;

    // Composing a challenge to send. Rows 0-4 are settings (left/right cycles the
    // value), row 5 is SEND. Opponent index 0 = open challenge, 1.. = friends_.
    std::vector<FriendSummary> friends_;
    bool friends_loading_  = false;
    int  cc_row_           = 0;
    int  cc_opp_           = 0;      // 0 = open challenge, else 1 + index into friends_
    int  cc_size_          = 2;      // 0=9x9, 1=13x13, 2=19x19
    bool cc_ranked_        = true;
    int  cc_pace_          = 1;      // 0=1 day, 1=3 days, 2=7 days per move
    int  cc_color_         = 0;      // 0=automatic, 1=black, 2=white
    bool cc_send_confirm_  = false;
    int  cc_hover_         = -1;
    int  cc_lines_drawn_   = 0;
    static const int CC_ROWS = 6;    // 5 settings + SEND
    void open_challenge_create();
    void draw_challenge_create();
    void handle_cc_button(Uint8 btn);
    void cc_cycle(int delta);        // change the focused row's value
    void cc_send();                  // build the request and fire it
    int  cc_hit(int mx, int my) const;

    // "Who won?" quiz — a finished pro position, guess black or white.
    // GameState is ~12MB, but App itself is heap-allocated (new App()), so this
    // member is fine where a local would blow the stack.
    GameState   wq_board_;
    int         wq_bs_       = BOARD_SIZE;
    std::string wq_black_, wq_white_, wq_date_, wq_result_;
    std::string wq_path_;              // the SGF this position came from
    int         wq_winner_   = 1;      // 1 = black won, 0 = white won
    bool        wq_answered_ = false;
    bool        wq_correct_  = false;
    int         wq_right_    = 0;      // running tally
    int         wq_total_    = 0;
    std::vector<std::string> wq_pool_; // candidate SGF paths, listed once per session
    void open_who_won();
    void wq_load_next();               // pick a random eligible game and show its final position
    void handle_wq_button(Uint8 btn);
    void wq_answer(int guess_black);   // 1 = guessed black, 0 = guessed white

    // Local game state
    bool        is_local_game_       = false;
    bool        local_prev_was_pass_ = false;  // true if the last move (by either side) was a pass
    std::string local_game_score_;             // score string shown during stone removal, empty until ownership arrives
    // Komi the current local game is actually played at: 7.5 for fresh games, the
    // review komi for practice-from-position games. Written into the saved SGF and
    // restored into review_komi_ when the game ends, so post-game analysis queries
    // use the komi the game was scored with.
    float       local_game_komi_     = 7.5f;

    void start_local_game();
    // "Practice vs KataGo" from the review menu: fork the current analysis position
    // into a live local game. The player takes the side to move at that position.
    void start_practice_from_position();
    // Free analysis: an empty 19x19 board in the normal GAME_OVER analysis mode
    // (tree panel, branching, engine toggle) — entered from LOBBY with triangle.
    void start_free_analysis();
    // Board size is single-select (radio) vs KataGo but multi-select for OGS search;
    // collapses match_menu_.size_sel[] down to exactly one entry — the first one
    // already checked, or 19x19 if none were — when entering KataGo mode.
    // Write an SGF from game_.history for a local game (no OGS copy exists to fetch).
    void save_local_sgf(const std::string& path);
    void handle_katago_gtp_move(int row, int col);
    // forced_result: non-empty means the result is already known (e.g. resignation).
    // In that case skip the GTP final_status query and go straight to territory display.
    void begin_local_stone_removal(const std::string& forced_result = "");
    void end_local_game(const std::string& result);

    // Left-stick joystick cursor state
    Sint16 js_left_x_  = 0;
    Sint16 js_left_y_  = 0;

    // RT held state — while held during live play, reveals the last-played stone
    bool rt_down_ = false;
    // Right mouse button held on a board — the mouse mirror of RT above. Kept
    // separate so releasing one doesn't hide the marker the other is holding up.
    bool rclick_down_ = false;

    bool init();
    void cleanup();
    void event_loop();
    void handle_controller_button(Uint8 button);
    void handle_net_msg(const NetMsg& msg);
    // forced_color: -1 = use current turn (normal), 0/1 = force that color and don't flip turn
    int  apply_move(int col, int row, int forced_color = -1);  // returns stones captured
    void play_move_sound(bool mine, int captured);
    void apply_pass();  // flips turn and records a history snapshot, same bookkeeping as apply_move
    void request_resync();  // debounced game/connect — GAME_CONNECTED rebuilds the position
    void reset_byo_countdowns(bool running_player_too);  // stored secs -> full period (byo players)
    void undo_local_move();  // pop back to your last turn in a local game vs KataGo
    void step_history(int delta);  // delta=-1 back, +1 forward; sets history_pos
    void load_demo_game();
    void load_demo_from_path(const std::string& path);
    void save_live_game();
    void save_companion();          // write .katago file alongside the SGF
    void load_companion();          // read .katago file if present
    std::string marked_position_path(int depth) const;  // deterministic path in games/<user>/marked/
    void save_marked_position(int depth);   // write flattened static-position SGF for the marked move
    void delete_marked_position(int depth); // remove that SGF when unmarked
    void load_sgf_for_review(const std::string& path);
    void open_game_catalog();
    void open_pro_catalog();   // curated professional-game library, read-only
    // Delete an SGF (and its .katago companion) from disk, then refresh the
    // catalog listing in place. Called via double-press Y confirm in the catalog.
    void delete_catalog_game(const std::string& sgf_path);
    void open_settings_menu();
    void open_katago_settings();   // local-play settings, as its own popup entry
    void draw();
    void build_analysis_tree();
    void build_analysis_tree_render();
    void apply_analysis_move(int col, int row);
    bool is_legal_analysis_move(int col, int row) const;
    float cached_analysis_score(const AnalysisNode* node) const;

    void set_status(const std::string& s) { status_ = s; }

    // Its own screen — make_ds() suppresses the live status line entirely in
    // CREDENTIAL_PROMPT (live=false → live_status=nullptr), so the prompt has to draw
    // itself or nothing appears at all (which is exactly what it did before).
    void draw_credential_prompt();

    Renderer::DrawState make_ds();
};

// ── Init / cleanup ────────────────────────────────────────────────────────────

// Map a connected controller's SDL type to the on-screen hint glyph style. Only a
// positively identified Xbox or Switch Pro pad switches away from the PlayStation
// default (also used for PS pads and anything SDL can't identify), so an
// unrecognized controller never shows confidently-wrong labels.
static PadStyle detect_pad_style(SDL_GameController* pad) {
    if (!pad) return PadStyle::PlayStation;
    switch (SDL_GameControllerGetType(pad)) {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE:             return PadStyle::Xbox;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return PadStyle::Nintendo;
    default:                                      return PadStyle::PlayStation;
    }
}

bool App::init() {
    // Report controller buttons by physical position (bottom face button =
    // SDL_CONTROLLER_BUTTON_A) rather than by printed label, so the Nintendo hint
    // glyphs (bottom=B, right=A...) line up with the button that actually fires.
    // Must be set before the GameController subsystem initializes.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) return false;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    sound_.init();  // best-effort — app runs silent if no audio device

    window_ = SDL_CreateWindow("OGS Live",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_SIZE, SCREEN_SIZE,
        SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    sdl_rend_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window_ || !sdl_rend_) return false;

    renderer_ = new Renderer(sdl_rend_);
    SDL_ShowCursor(SDL_DISABLE);
    // Enable SDL text input so the credential prompt receives real composed
    // characters (SDL_TEXTINPUT) — raw keycodes are always lowercase/unshifted,
    // which mangles capitals and shifted symbols in passwords. Harmless to leave on
    // for the whole session; game controls still come through as SDL_KEYDOWN.
    SDL_StartTextInput();

    // Optional: load an external community controller-mapping DB dropped next to
    // the exe (gamecontrollerdb.txt), so pads SDL's built-in table doesn't know
    // still map to the standard layout. Best-effort — the built-in mappings already
    // cover common Xbox / PlayStation / Switch pads, so a missing file is fine.
    if (SDL_GameControllerAddMappingsFromFile((exe_dir() + "gamecontrollerdb.txt").c_str()) < 0)
        SDL_GameControllerAddMappingsFromFile((exe_dir() + "../../gamecontrollerdb.txt").c_str());

    // Open first available controller
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            pad_ = SDL_GameControllerOpen(i);
            break;
        }
    }
    Renderer::set_pad_style(detect_pad_style(pad_));

    return true;
}

void App::cleanup() {
    save_companion();  // persist any in-progress review scores/marks before exiting
    sound_.shutdown();
    kata_.stop();
    kata_9_.stop();
    kata_gtp_.stop();
    net_.stop();
    if (pad_)      { SDL_GameControllerClose(pad_); pad_ = nullptr; }
    delete renderer_; renderer_ = nullptr;
    if (sdl_rend_) { SDL_DestroyRenderer(sdl_rend_); sdl_rend_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);      window_   = nullptr; }
    SDL_Quit();
}

// ── Board management ──────────────────────────────────────────────────────────

int App::apply_move(int col, int row, int forced_color) {
    if (col < 0 || row < 0) return 0;  // pass — board unchanged
    last_move_r_ = row;
    last_move_f_ = col;
    int is_black = (forced_color >= 0) ? forced_color : (int)game_.board.turn_is_black;
    const int prisoners_before = game_.board.black_prisoners + game_.board.white_prisoners;
    game_.board.place_stone(row, col, is_black);
    const int captured = game_.board.black_prisoners + game_.board.white_prisoners - prisoners_before;
    game_.board.save_snapshot();
    if (forced_color >= 0) {
        game_.history.push_back(game_.board);
        if (move_scores_.size() < game_.history.size()) {
            move_scores_.push_back(FLT_MAX); move_marked_.push_back(false); marked_paths_.push_back("");
        }
        return 0;  // pre-placed handicap stone: caller sets turn after
    }
    // Free handicap: first `handicap` stones are all black; after last one, white plays
    if (game_.free_handicap && game_.board.stone_count <= game_.handicap) {
        game_.board.turn_is_black = (game_.board.stone_count < game_.handicap) ? 1 : 0;
    } else {
        game_.board.turn_is_black = !is_black;
    }
    game_.history.push_back(game_.board);
    if (move_scores_.size() < game_.history.size()) {
        move_scores_.push_back(FLT_MAX); move_marked_.push_back(false); marked_paths_.push_back("");
    }
    return captured;
}

// The placing clack, then — if the move took anything — the captured stones
// clattering into the bowl (queued, so it follows the clack).
void App::play_move_sound(bool mine, int captured) {
    sound_.play_stone(mine);
    if (captured > 0) sound_.play_capture(captured);
}

// A pass has no stone to place, but still needs its own history snapshot with the same
// bookkeeping as a real move — skipping it desyncs history depth from the true move
// count (ko-check's "2 states ago" comparison, move_scores_/marked_paths_ indexing, and
// build_analysis_tree()'s per-node depth all silently drift once a pass is missing).
void App::apply_pass() {
    game_.board.turn_is_black = !game_.board.turn_is_black;
    game_.board.save_snapshot();
    game_.history.push_back(game_.board);
    if (move_scores_.size() < game_.history.size()) {
        move_scores_.push_back(FLT_MAX); move_marked_.push_back(false); marked_paths_.push_back("");
    }
}

// Ask OGS to re-send gamedata: GAME_CONNECTED then rebuilds the whole position
// authoritatively, which is the only reliable cure once the local board and the
// server disagree. Debounced so a flurry of bad events can't spam game/connect,
// and self-retrying so a dropped rebuild can't leave us frozen.
void App::request_resync() {
    if (game_.game_id == 0 || is_local_game_) return;
    Uint32 now = SDL_GetTicks();
    if (now - game_.resync_req_at <= 3000) return;
    game_.resync_req_at = now;
    // A resync is a round trip: it can only be answered while the socket is up.
    // Sending it into a dead connection and putting "RESYNCING..." on screen
    // described a recovery that was not happening — that message sat there for the
    // nine seconds before the 2026-07-31 drop was even reported. The network thread
    // reconnects on its own and re-sends game/connect when it does, so there is
    // nothing to do here but say so. (Still behind the debounce above, so this
    // can't redraw on every pass of the main loop.)
    if (!net_.connected()) {
        net_.log_line("request_resync: socket down — waiting on the reconnect");
        set_status("RECONNECTING...");
        draw();
        return;
    }
    net_.log_line("request_resync: re-sending game/connect for " + std::to_string(game_.game_id));
    net_.cmd_reconnect_game(game_.game_id);
    set_status("RESYNCING...");
    draw();
}

// Byo-yomi: a period resets the instant a move is played — the player who just
// moved parks with a full period banked, and the player now to move starts a
// fresh one. But server move events carry period_time_left as a snapshot of the
// mover's just-ended turn (the leftover, e.g. 8s of a 30s period), and that
// stale remainder was being stored verbatim — so when the turn flipped back,
// the countdown started from 8s, hit zero, and began eating banked periods on
// screen while the server-side clock was actually fine. Normalize stored secs
// to the full period for byo players: always for the parked player (their
// stored value is never meaningful); for the running player too when a move
// was just applied (running_player_too=true — at a turn flip they start a
// fresh period by rule). Mid-turn clock updates keep running_player_too=false
// so a genuine in-progress countdown (e.g. reconnect) is preserved.
void App::reset_byo_countdowns(bool running_player_too) {
    bool btm   = (game_.board.turn_is_black == 1);
    bool b_byo = game_.black_in_byo || (game_.black_secs <= 0 && game_.black_periods > 0);
    bool w_byo = game_.white_in_byo || (game_.white_secs <= 0 && game_.white_periods > 0);
    if (b_byo && game_.black_period_secs > 0 && (running_player_too || !btm))
        game_.black_secs = game_.black_period_secs;
    if (w_byo && game_.white_period_secs > 0 && (running_player_too || btm))
        game_.white_secs = game_.white_period_secs;
}

// Pops history back to your own last turn in a local game vs KataGo — your last move
// plus whatever KataGo played in response (generically walks back ply-by-ply checking
// whose turn each resulting position is, rather than assuming a hardcoded pair, so it
// still does the right thing across a free-handicap run of same-color moves). Only
// safe to call when it's genuinely your turn: if KataGo is still "thinking" (a genmove
// request is in flight), pulling the position out from under it would leave its reply
// arriving for a position that no longer exists.
void App::undo_local_move() {
    if (!is_local_game_ || state_ != AppState::PLAYING) return;
    if (!game_.my_turn) {
        flash_       = "CAN'T UNDO — KATAGO IS THINKING";
        flash_until_ = SDL_GetTicks() + 1500;
        draw();
        return;
    }
    if (game_.history.size() <= 1) {
        flash_       = "NOTHING TO UNDO";
        flash_until_ = SDL_GetTicks() + 1500;
        draw();
        return;
    }

    while (game_.history.size() > 1) {
        game_.history.pop_back();
        // Keep the score arrays in strict lockstep with history — only pop entries
        // that actually correspond to the popped plies. (Guarding on size, not just
        // non-empty, means a desynced-longer array can't be drained past the moves
        // being undone.)
        while (move_scores_.size() > game_.history.size()) {
            move_scores_.pop_back();
            move_marked_.pop_back();
            marked_paths_.pop_back();
        }
        kata_gtp_.send_undo();
        if (game_.history.back().turn_is_black == game_.my_color) break;
    }
    // Rewind the background-scoring sweep: its pointer only ever moves forward, so
    // without this, replayed moves after an undo would never get scored (the sweep
    // would still think it was past them) — the "dead space" gap in the score graph.
    // The skip-loop fast-forwards over already-scored depths, so restarting from 0
    // costs nothing.
    bg_analysis_next_ = 0;
    // Puzzle evals for the popped depths describe positions that no longer exist;
    // the replayed line gets fresh ones when the sweep re-scores it. (Evals for
    // surviving depths — including the position we landed on — stay valid.)
    puzzle_eval_.erase(puzzle_eval_.lower_bound((int)game_.history.size()),
                       puzzle_eval_.end());

    game_.board    = game_.history.back();
    game_.my_turn  = true;
    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;

    // Recompute "was the last move a pass" and the last-played-stone highlight from
    // the position we landed on, rather than leaving them stale from before the undo.
    local_prev_was_pass_ = false;
    last_move_r_ = last_move_f_ = -1;
    if (game_.history.size() >= 2) {
        const auto& cur  = game_.history.back();
        const auto& prev = game_.history[game_.history.size() - 2];
        local_prev_was_pass_ = (memcmp(cur.board, prev.board, sizeof(cur.board)) == 0);
        for (int r = 0; r < game_.board_size && last_move_r_ < 0; r++)
            for (int f = 0; f < game_.board_size; f++)
                if (cur.board[r][f] != 0 && prev.board[r][f] == 0) {
                    last_move_r_ = r; last_move_f_ = f; break;
                }
    }

    flash_       = "UNDID YOUR MOVE";
    flash_until_ = SDL_GetTicks() + 1500;
    set_status(std::string("YOUR TURN  (") + (game_.my_color == 1 ? "BLACK" : "WHITE") + ")");
    request_live_ownership();
    draw();
}

void App::step_history(int delta) {
    if (game_.history.empty()) return;
    int max_idx = (int)game_.history.size() - 1;
    if (delta < 0) {
        if (game_.history_pos < 0)      game_.history_pos = std::max(0, max_idx - 1);
        else if (game_.history_pos > 0) game_.history_pos--;
    } else if (delta > 0) {
        if (game_.history_pos < 0)                    return;  // already live
        if (game_.history_pos >= max_idx - 1) game_.history_pos = -1;  // skip last (=live board)
        else                                  game_.history_pos++;
    }
}

void App::build_analysis_tree() {
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    // Every non-drill analysis entry (live game end, catalog review, free
    // analysis) funnels through here — a fresh tree is never an edit of a
    // previously saved drill, so SAVE must create a new file, not overwrite.
    // (drill_edit_current() also passes through here for its root, then
    // re-sets drill_edit_path_ afterwards.)
    drill_edit_path_.clear();
    analysis_setup_color_ = -1;   // setup-stone mode never carries across sessions
    drill_solver_black_   = -1;   // solver defaults to whoever moves first, until pinned

    analysis_root_ = std::make_unique<AnalysisNode>();
    analysis_root_->board = game_.history.empty() ? game_.board : game_.history[0];
    analysis_root_->depth = 0;

    int sz = game_.board_size;
    AnalysisNode* cur = analysis_root_.get();
    for (int i = 1; i < (int)game_.history.size(); i++) {
        auto child = std::make_unique<AnalysisNode>();
        child->board  = game_.history[i];
        child->depth  = i;
        child->parent = cur;

        // Identify which cell was placed so apply_analysis_move can dedup against it
        int mv_r = -1, mv_f = -1;
        for (int r = 0; r < sz && mv_r < 0; r++)
            for (int f = 0; f < sz && mv_r < 0; f++)
                if (game_.history[i].board[r][f] != 0 && cur->board.board[r][f] == 0)
                    { mv_r = r; mv_f = f; }
        child->move_row = mv_r;
        child->move_col = mv_f;
        // Read actual stone color from the board — immune to turn_is_black carry-over
        // from a previous game or handicap sequences where turn doesn't alternate normally.
        // Fall back to turn_is_black only for pass moves (no stone placed).
        child->move_color = (mv_r >= 0 && mv_f >= 0)
            ? ((game_.history[i].board[mv_r][mv_f] == 1) ? 1 : 0)
            : (int)cur->board.turn_is_black;

        cur->children.push_back(std::move(child));
        cur->active_child = 0;
        cur = cur->children[0].get();
    }
    analysis_cur_ = analysis_root_.get();  // open at start of game

    // Seed nodes on the main line with any scores already collected during play.
    {
        AnalysisNode* n = analysis_root_.get();
        while (n) {
            int d = n->depth;
            if (d < (int)move_scores_.size() && move_scores_[d] != FLT_MAX)
                n->score_lead = move_scores_[d];
            if (n->children.empty()) break;
            n = n->children[0].get();
        }
    }

    build_analysis_tree_render();
}

void App::build_analysis_tree_render() {
    analysis_tree_render_.clear();
    if (!analysis_root_) return;

    int max_col = 0;
    std::function<void(AnalysisNode*, int, int, int)> dfs =
        [&](AnalysisNode* node, int col, int parent_col, int parent_depth) {
            AnalysisTreeRenderNode rn;
            rn.depth        = node->depth;
            rn.col          = col;
            rn.current      = (node == analysis_cur_);
            rn.parent_depth = parent_depth;
            rn.parent_col   = parent_col;
            rn.move_color   = node->move_color;
            rn.marked       = (node->depth < (int)move_marked_.size()) && move_marked_[node->depth];
            rn.goal         = node->drill_correct;   // green halo, same as puzzle solution endpoints
            analysis_tree_render_.push_back(rn);

            for (int i = 0; i < (int)node->children.size(); i++) {
                int child_col = (i == 0) ? col : ++max_col;
                dfs(node->children[i].get(), child_col, col, node->depth);
            }
        };

    dfs(analysis_root_.get(), 0, 0, -1);
}

bool App::is_legal_analysis_move(int col, int row) const {
    if (!analysis_cur_) return false;
    const GameState& gs = analysis_cur_->board;
    int bs = game_.board_size;
    if (row < 0 || row >= bs || col < 0 || col >= bs) return false;
    if (gs.board[row][col] != 0) return false;
    int is_black = (gs.turn_is_black == 1) ? 1 : 0;
    if (GoRules::would_be_suicide(gs.board, row, col, is_black, bs)) return false;
    // Ko: simulated result must not recreate the position from one move ago (parent).
    // Mirrors the live game check: history[size()-2] = board before the last move.
    const AnalysisNode* prev = analysis_cur_->parent;
    if (prev) {
        char test[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
        memcpy(test, gs.board, sizeof(test));
        test[row][col] = is_black ? 1 : 2;
        int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_count = 0;
        GoRules::find_captured(test, is_black, row, col, cap_r, cap_f, cap_count, bs);
        for (int j = 0; j < cap_count; j++) test[cap_r[j]][cap_f[j]] = 0;
        if (memcmp(test, prev->board.board, sizeof(test)) == 0) return false;
    }
    return true;
}

// Returns an already-known score for this node without querying KataGo — the node's
// own score_lead if a foreground query has landed on it before, else move_scores_[depth]
// (populated by background scoring; exact for main-line nodes, a same-depth proxy off
// it). FLT_MAX if nothing is cached yet. Used to show "PROJECTED: ..." instantly on
// navigation instead of blanking out until the (now debounced) fresh query completes.
float App::cached_analysis_score(const AnalysisNode* node) const {
    if (!node) return FLT_MAX;
    if (node->score_lead != FLT_MAX) return node->score_lead;
    // move_scores_[] is main-line-only (see AnalysisNode::is_main_line) — for a branch
    // node it would just be some unrelated main-line position at the same depth, not a
    // real preview of this position, so don't fall back to it off the main line.
    if (node->is_main_line && node->depth < (int)move_scores_.size())
        return move_scores_[node->depth];
    return FLT_MAX;
}

void App::apply_analysis_move(int col, int row) {
    if (!analysis_cur_) return;

    // Navigate to existing child if this move was already played (always legal)
    for (int i = 0; i < (int)analysis_cur_->children.size(); i++) {
        if (analysis_cur_->children[i]->move_col == col &&
            analysis_cur_->children[i]->move_row == row) {
            analysis_cur_->active_child = i;
            analysis_cur_ = analysis_cur_->children[i].get();
            build_analysis_tree_render();
            return;
        }
    }

    // Ko / suicide check for new moves
    if (!is_legal_analysis_move(col, row)) {
        ko_flash_until_ = SDL_GetTicks() + 400;
        return;
    }

    // Create new child node — always a hypothetical branch. build_analysis_tree() is
    // the only place that creates real is_main_line=true nodes, from the actual played
    // game; any move placed interactively here, even one continuing past the end of
    // the recorded game, is exploration, not the real game.
    auto child = std::make_unique<AnalysisNode>();
    child->board        = analysis_cur_->board;
    child->move_col     = col;
    child->move_row     = row;
    child->depth        = analysis_cur_->depth + 1;
    child->parent       = analysis_cur_;
    child->is_main_line = false;

    // Apply the move using GameState::place_stone (handles captures & prisoners)
    int is_black = (child->board.turn_is_black == 1) ? 1 : 0;
    if (!child->board.place_stone(row, col, is_black)) return;  // illegal (occupied or suicide)
    child->move_color = is_black;
    child->board.turn_is_black = !is_black;

    int new_idx = (int)analysis_cur_->children.size();
    analysis_cur_->active_child = new_idx;
    analysis_cur_->children.push_back(std::move(child));
    analysis_cur_ = analysis_cur_->children[new_idx].get();
    build_analysis_tree_render();
}

// Remove the current node and its entire subtree from the analysis tree,
// landing on the parent. Hypothetical branches only — the popup item is gated
// on !is_main_line, and this re-checks for safety (deleting real game moves
// would silently rewrite the record being reviewed).
void App::delete_analysis_branch() {
    AnalysisNode* cur = analysis_cur_;
    if (!cur || !cur->parent || cur->is_main_line) return;
    AnalysisNode* parent = cur->parent;
    auto& sibs = parent->children;
    for (size_t i = 0; i < sibs.size(); i++) {
        if (sibs[i].get() == cur) {
            sibs.erase(sibs.begin() + i);   // unique_ptr — frees the whole subtree
            break;
        }
    }
    if (parent->active_child >= (int)sibs.size())
        parent->active_child = 0;
    analysis_cur_ = parent;
    build_analysis_tree_render();
    kata_suggestion_count_ = 0;
    kata_score_lead_  = cached_analysis_score(analysis_cur_);
    kata_query_after_ = SDL_GetTicks() + 1000;
    flash_       = "BRANCH DELETED";
    flash_until_ = SDL_GetTicks() + 1200;
}

// Load a specific SGF into the lobby screensaver (board + names, no analysis
// UI) — shared by load_demo_game()'s random pick and start_catalog_autoplay()'s
// scoped playlist.
void App::load_demo_from_path(const std::string& path) {
    demo_active_ = false;
    demo_.rows.clear();
    demo_.cols.clear();
    demo_.colors.clear();
    demo_.pos = 0;
    demo_clear_end_fill();

    SgfGame g;
    if (!load_sgf(path, g) || g.move_count == 0) return;

    demo_.board_size = g.board_size;
    demo_.komi       = g.komi;
    demo_.black_name = g.black_name;
    demo_.white_name = g.white_name;

    // Store just the move list — board is updated one move per second at runtime
    for (int i = 0; i < g.move_count; i++) {
        int r, f;
        if (!parse_sgf_move(g.moves[i], r, f)) continue;
        demo_.rows.push_back(r);
        demo_.cols.push_back(f);
        demo_.colors.push_back(g.colors[i]);
    }

    // Reset the board for this game
    demo_.board.reset();
    demo_.board.board_size   = g.board_size;
    demo_.board.turn_is_black = 1;

    demo_.next_tick = SDL_GetTicks() + 1000;
    demo_active_ = !demo_.rows.empty();
}

// Back out of the end-of-game fill — a new game is loading, or the viewer
// stepped back into the game with L2 and the final position no longer applies.
void App::demo_clear_end_fill() {
    demo_.end_phase  = false;
    demo_.fill_ready = false;
    demo_.area_black = -1;
    demo_.area_white = -1;
    memset(demo_.ownership, 0, sizeof(demo_.ownership));
    memset(demo_.dead,      0, sizeof(demo_.dead));
}

// Last move of a screensaver game has been played: ask KataGo who owns what in
// the final position. The answer arrives on the reader thread and is picked up
// by the poll loop, which paints the fill and restarts the hold from there —
// so a slow answer still gets its full time on screen. next_tick is the
// backstop: if the query is never answered (or never sent, KataGo not running),
// the screensaver moves on by itself.
void App::demo_begin_end_fill() {
    demo_.end_phase  = true;
    demo_.fill_ready = false;

    KatagoProc& kp = kata_for(demo_.board_size);
    if (kp.running() && !demo_.rows.empty()) {
        bool no_dead[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
        kp.query_ownership(demo_.board.board, demo_.board_size, no_dead, demo_.komi, 100);
        demo_.next_tick = SDL_GetTicks() + DEMO_FILL_WAIT_MS;
    } else {
        demo_.next_tick = SDL_GetTicks() + DEMO_FILL_HOLD_MS;
    }
}

// The hold is over (or the fill never came): on to the next game, either the
// next entry of a catalog playlist or a fresh random pick.
void App::demo_next_game() {
    if (demo_playlist_pos_ >= 0) {
        // Scoped autoplay (started from the catalog): advance through the
        // remembered list in order; after one full lap, fall back to the
        // default random screensaver rather than looping forever.
        demo_playlist_pos_++;
        if (demo_playlist_pos_ < (int)demo_playlist_.size()) {
            load_demo_from_path(demo_playlist_[demo_playlist_pos_]);
            return;
        }
        demo_playlist_.clear();
        demo_playlist_pos_ = -1;
    }
    load_demo_game();
}

void App::load_demo_game() {
    // Merge the pro library (games/) and the user's own games (my_games/) into one
    // pool so the ambient screensaver keeps the same mixed breadth it always had,
    // back when both lived under the same games/ tree.
    std::vector<std::string> all_paths;
    auto collect = [&](const std::string& dirname) {
        // Try next to exe first, then one level up (dev layout: exe is in ogs_client/)
        std::string dir = exe_dir() + dirname;
        std::vector<std::string> rel;
        if (!Catalog::list_sgf_files(dir, rel) || rel.empty()) {
            dir = exe_dir() + "../../" + dirname;
            rel.clear();
            if (!Catalog::list_sgf_files(dir, rel) || rel.empty()) return;
        }
        for (const auto& r : rel) all_paths.push_back(Catalog::join_path(dir, r));
    };
    collect("games");
    collect("my_games");
    if (all_paths.empty()) return;

    // Pick a random file, skipping the one we just played if possible.
    static std::string last_path;
    std::srand((unsigned)std::time(nullptr) ^ (unsigned)SDL_GetTicks());
    std::string path;
    if (all_paths.size() == 1) {
        path = all_paths[0];
    } else {
        do { path = all_paths[(size_t)std::rand() % all_paths.size()]; }
        while (path == last_path);
    }
    last_path = path;
    load_demo_from_path(path);
}

// X in the catalog, on a game or a directory/player/year entry: play every
// file-type row currently shown, in order, as the lobby screensaver — e.g. an
// entire pro player's folder back to back for a screencap — starting at the
// row that was pressed. Works uniformly across flat-directory, virtual
// player/year, and search-result views, since all of them populate
// catalog_.entries with file rows (type 0) the same way.
void App::start_catalog_autoplay(int start_at_catalog_index) {
    std::vector<std::string> paths;
    int start_pos = 0;
    for (int i = 0; i < (int)catalog_.entries.size(); i++) {
        if (catalog_.entries[i].type != 0) continue;
        if (i == start_at_catalog_index) start_pos = (int)paths.size();
        std::string p = catalog_.entry_path(i);
        if (!p.empty()) paths.push_back(p);
    }
    if (paths.empty()) return;

    demo_playlist_     = std::move(paths);
    demo_playlist_pos_ = std::min(start_pos, (int)demo_playlist_.size() - 1);
    load_demo_from_path(demo_playlist_[demo_playlist_pos_]);
    catalog_.close();
    state_ = AppState::LOBBY;
    set_status("");
}

// ── Controller ────────────────────────────────────────────────────────────────

// Defined with the joseki explorer below; used by the LT/RT handler here.
static bool jk_parse_placement(const std::string& p, int bs, int& r, int& f);

void App::handle_controller_button(Uint8 btn) {
    // Credential prompt: typing needs a keyboard, but a pad can still skip the login
    if (state_ == AppState::CREDENTIAL_PROMPT) {
        if (btn == SDL_CONTROLLER_BUTTON_START || btn == SDL_CONTROLLER_BUTTON_B)
            continue_offline();
        return;
    }

    // Catalog overlay: intercept all input while open
    if (catalog_.active) {
        // Delete confirm is armed by Y and cancelled by any other button
        if (catalog_delete_confirm_ && btn != SDL_CONTROLLER_BUTTON_Y)
            catalog_delete_confirm_ = false;

        auto set_cat_index = [&](int i) { set_catalog_index(i); };
        // First letter of a row's sortable name (case-insensitive) — used by the
        // L2/R2 letter-scrub below. Meta-entries like "[BY YEAR]" compare on their
        // own bracketed label, which is fine since scrubbing just needs "looks
        // different from here", not a guarantee of strict A-Z order (the BY PLAYER
        // list itself sorts by game count first, name only as a tiebreaker).
        auto row_letter = [&](int i) -> char {
            const std::string& s = catalog_.entries[i].name.empty()
                                   ? catalog_.entries[i].display_name
                                   : catalog_.entries[i].name;
            return s.empty() ? '\0' : (char)toupper((unsigned char)s[0]);
        };

        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (catalog_.index > 0) set_cat_index(catalog_.index - 1);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (catalog_.index + 1 < (int)catalog_.entries.size()) set_cat_index(catalog_.index + 1);
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            set_cat_index(catalog_.index - 10);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            set_cat_index(catalog_.index + 10);
            break;
        case 0xFD:   // L2: scrub back to the previous row whose leading letter differs
        case 0xFE: { // R2: scrub forward to the next row whose leading letter differs
            int n = (int)catalog_.entries.size();
            if (n > 1) {
                int dir   = (btn == 0xFE) ? 1 : -1;
                char start = row_letter(catalog_.index);
                int i = catalog_.index;
                for (int steps = 0; steps < n; steps++) {
                    i += dir;
                    if (i < 0 || i >= n) break;   // clamp at the ends, don't wrap
                    if (row_letter(i) != start) { set_cat_index(i); break; }
                }
            }
            break;
        }
        case SDL_CONTROLLER_BUTTON_A:
            catalog_.select();
            if (catalog_.selection_made) {
                load_sgf_for_review(catalog_.selected_path);
                catalog_.selection_made = false;
            }
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            // Delete the selected game — double-press confirm, same idiom as
            // resign/pass/mark. Only file entries (not directories/meta-entries),
            // and never in the read-only pro game library.
            if (catalog_.selected_entry_path().empty()) break;
            if (catalog_readonly_) {
                flash_       = "READ-ONLY — CAN'T DELETE";
                flash_until_ = SDL_GetTicks() + 1500;
            } else if (!catalog_delete_confirm_) {
                catalog_delete_confirm_ = true;
                flash_       = "PRESS " GLYPH_PS_TRIANGLE " AGAIN TO DELETE GAME";
                flash_until_ = SDL_GetTicks() + 2500;
            } else {
                catalog_delete_confirm_ = false;
                delete_catalog_game(catalog_.selected_entry_path());
            }
            break;
        case SDL_CONTROLLER_BUTTON_X: {
            // Autoplay from here, presented as the lobby screensaver. On a game,
            // plays the list it's part of starting there. On a directory/player/
            // year entry, drills in first (same as A) and, if that landed on a
            // list of games, starts autoplaying it from the top — so pressing X
            // on a player's name in the BY PLAYER root list is a single press to
            // "play this player's whole folder."
            if (catalog_.index < 0 || catalog_.index >= (int)catalog_.entries.size()) break;
            const CatalogEntry& e = catalog_.entries[catalog_.index];
            if (e.type == 0) {
                start_catalog_autoplay(catalog_.index);
            } else if (e.type != 2) {   // not ".."
                catalog_.select();
                bool has_files = false;
                for (const auto& ce : catalog_.entries)
                    if (ce.type == 0) { has_files = true; break; }
                if (has_files) start_catalog_autoplay(0);
            }
            break;
        }
        default:
            // B, START, or anything else — close catalog
            catalog_.close();
            break;
        }
        draw();
        return;
    }

    // START popup menu: intercepts all input while open
    if (popup_active_) {
        handle_popup_button(btn);
        return;
    }

    // Typing a drill name — keyboard owns all input until Enter/ESC
    if (drill_rename_active_ && state_ == AppState::PUZZLE_BROWSE)
        return;

    // History navigation: LT/RT work in any game state
    if (btn == 0xFD || btn == 0xFE) {
        if (state_ == AppState::JOSEKI) {
            if (btn == 0xFD) {
                jk_step_back();
            } else if (!jk_loading_ && !jk_path_.empty()) {
                // R2 follows the book's main line: first IDEAL, else first GOOD
                const JosekiNextMove* pick = nullptr;
                for (const auto& m : jk_path_.back().next_moves) {
                    int r, f;
                    if (!jk_parse_placement(m.placement, 19, r, f)) continue;
                    if (m.category == "IDEAL") { pick = &m; break; }
                    if (!pick && m.category == "GOOD") pick = &m;
                }
                if (pick) jk_fetch_node(pick->node_id);
            }
            return;
        }

        // Lobby screensaver: step the game currently being shown forward or back,
        // instead of waiting on the automatic one-move-per-second tick. Works for
        // both the default random screensaver and a scoped catalog playlist.
        if (demo_active_ && (state_ == AppState::LOBBY || state_ == AppState::SEARCHING ||
                             state_ == AppState::CONNECTING)) {
            if (btn == 0xFE) {   // R2: forward one move
                if (demo_.pos < (int)demo_.rows.size()) {
                    demo_.board.place_stone(demo_.rows[demo_.pos], demo_.cols[demo_.pos],
                                            demo_.colors[demo_.pos]);
                    demo_.pos++;
                }
            } else {   // L2: back one move — no stored history, so replay from the
                       // start up to the new position (cheap; a few hundred moves
                       // at most, and Go capture replay is fast).
                if (demo_.pos > 0) {
                    demo_.pos--;
                    demo_.board.reset();
                    demo_.board.board_size    = demo_.board_size;
                    demo_.board.turn_is_black = 1;
                    for (int i = 0; i < demo_.pos; i++)
                        demo_.board.place_stone(demo_.rows[i], demo_.cols[i], demo_.colors[i]);
                }
            }
            // Stepping by hand puts the game back in play: drop the end-of-game
            // fill so the real position shows through (stepping to the end again
            // re-triggers it on the next automatic tick).
            demo_clear_end_fill();
            demo_.next_tick = SDL_GetTicks() + 1000;  // don't also auto-advance right after
            draw();
            return;
        }

        bool has_game = (state_ == AppState::PLAYING ||
                         state_ == AppState::STONE_REMOVAL ||
                         state_ == AppState::GAME_OVER);
        if (has_game) {
            if (state_ == AppState::GAME_OVER && analysis_cur_) {
                // Navigate analysis tree
                if (btn == 0xFD) {
                    if (analysis_cur_->parent) {
                        analysis_cur_ = analysis_cur_->parent;
                        analysis_cur_->active_child = 0;  // RT follows main line after stepping back
                    }
                    build_analysis_tree_render();
                    kata_suggestion_count_ = 0;
                    kata_score_lead_ = cached_analysis_score(analysis_cur_);
                    kata_query_after_ = SDL_GetTicks() + 1000;
                } else {
                    analysis_step_forward();
                }
            } else {
                step_history(btn == 0xFD ? -1 : +1);
            }
            draw();
        } else if (state_ == AppState::PUZZLE_PLAY && pz_explore_) {
            // Review the sandbox moves made so far. Only wired while exploring —
            // pz_node_ is frozen there, so scrubbing never needs to re-resolve a
            // tree position; placing a stone from mid-review branches off via the
            // history_pos handling already in pz_place().
            step_history(btn == 0xFD ? -1 : +1);
            draw();
        }
        return;
    }

    // Y button: mark/unmark the current move for analysis attention.
    // Marking saves a standalone SGF snapshot of the position to games/<user>/marked/,
    // and unmarking deletes it — both are file operations, so both require a confirm press.
    // Scoped to the states where marking exists — an unconditional catch here would
    // swallow triangle everywhere (it silently ate LOBBY's free-analysis binding).
    if (btn == SDL_CONTROLLER_BUTTON_Y &&
        (state_ == AppState::PLAYING || state_ == AppState::GAME_OVER)) {
        int depth = -1;
        if (state_ == AppState::PLAYING) {
            depth = (game_.history_pos >= 0) ? game_.history_pos
                                             : (int)game_.history.size() - 1;
        } else if (state_ == AppState::GAME_OVER && analysis_cur_) {
            depth = analysis_cur_->depth;
        }
        if (depth >= 0 && depth < (int)move_marked_.size()) {
            if (!mark_confirm_) {
                mark_confirm_ = true;
                flash_       = move_marked_[depth] ? "PRESS " GLYPH_PS_TRIANGLE " AGAIN TO UNMARK"
                                                   : "PRESS " GLYPH_PS_TRIANGLE " AGAIN TO MARK";
                flash_until_ = SDL_GetTicks() + 2000;
                draw();
                return;
            }
            mark_confirm_ = false;
            move_marked_[depth] = !move_marked_[depth];
            if (move_marked_[depth]) {
                save_marked_position(depth);
                flash_ = "MARKED";
            } else {
                delete_marked_position(depth);
                flash_ = "UNMARKED";
            }
            flash_until_ = SDL_GetTicks() + 1200;
            if (state_ == AppState::GAME_OVER) build_analysis_tree_render();
        }
        draw();
        return;
    }

    // Undo request from opponent: A = accept, B or START = deny
    if (undo_pending_ && state_ == AppState::PLAYING) {
        if (btn == SDL_CONTROLLER_BUTTON_A) {
            undo_pending_ = false;
            net_.cmd_accept_undo(game_.game_id, undo_move_number_);
            if ((int)game_.history.size() > 1) {
                game_.history.pop_back();
                game_.board = game_.history.back();
            }
            game_.history_pos = -1;
            game_.pending_col = -2;
            game_.pending_row = -2;
            pass_confirm_    = false;
            bool btp = (game_.board.turn_is_black == 1);
            game_.my_turn = (btp && game_.my_color == 1) || (!btp && game_.my_color == 0);
            set_status(game_.my_turn ? "YOUR TURN" : "WAITING...");
        } else if (btn == SDL_CONTROLLER_BUTTON_B || btn == SDL_CONTROLLER_BUTTON_START) {
            undo_pending_ = false;
            net_.cmd_reject_undo(game_.game_id, undo_move_number_);
        }
        draw();
        return;
    }

    // Cancel pass confirm on any non-B button
    if (pass_confirm_ && btn != SDL_CONTROLLER_BUTTON_B) {
        pass_confirm_ = false;
        draw();
        return;
    }
    // Cancel mark confirm on any non-Y button
    if (mark_confirm_ && btn != SDL_CONTROLLER_BUTTON_Y) {
        mark_confirm_ = false;
        draw();
        return;
    }
    // Cancel find-match confirm on any non-A button
    if (find_match_confirm_ && btn != SDL_CONTROLLER_BUTTON_A) {
        find_match_confirm_ = false;
        draw();
        return;
    }

    // BACK: open the settings menu from anywhere — the same button that used to open
    // it from the lobby, standardized across every state (catalog interception and
    // CREDENTIAL_PROMPT's early return above already rule those out; MATCH_MENU itself
    // handles BACK as its own close action, in that switch below).
    if (btn == SDL_CONTROLLER_BUTTON_BACK && state_ != AppState::MATCH_MENU) {
        open_settings_menu();
        return;
    }

    if (state_ == AppState::PUZZLE_BROWSE || state_ == AppState::PUZZLE_PLAY) {
        handle_puzzle_button(btn);
        return;
    }

    if (state_ == AppState::JOSEKI) {
        handle_joseki_button(btn);
        return;
    }

    if (state_ == AppState::CORR_LIST) {
        handle_corr_button(btn);
        return;
    }

    if (state_ == AppState::CHALLENGES) {
        handle_chal_button(btn);
        return;
    }

    if (state_ == AppState::CHALLENGE_CREATE) {
        handle_cc_button(btn);
        return;
    }

    if (state_ == AppState::WHO_WON) {
        handle_wq_button(btn);
        return;
    }

    if (state_ == AppState::LOBBY) {
        if (btn == SDL_CONTROLLER_BUTTON_X) {
            open_game_catalog();
            draw();
        } else if (btn == SDL_CONTROLLER_BUTTON_Y) {
            start_free_analysis();
        } else if (btn == SDL_CONTROLLER_BUTTON_B) {
            open_puzzle_browser();
        } else if (btn == SDL_CONTROLLER_BUTTON_START) {
            open_popup_menu();
        } else if (btn == SDL_CONTROLLER_BUTTON_A) {
            // Finding a match queues a real opponent immediately, so a single press
            // was too easy to trigger by accident — double-press confirm, same
            // idiom as resign/pass/mark. START -> FIND MATCH also still works.
            if (offline_) {
                flash_       = "NOT LOGGED IN — LOG IN TO OGS FROM THE MENU";
                flash_until_ = SDL_GetTicks() + 2500;
            } else if (!find_match_confirm_) {
                find_match_confirm_ = true;
                flash_       = "PRESS " GLYPH_PS_CROSS " AGAIN TO SEARCH FOR MATCH";
                flash_until_ = SDL_GetTicks() + 2500;
            } else {
                find_match_confirm_ = false;
                net_.cmd_find_match(match_prefs_);
                state_ = AppState::SEARCHING;
                set_status("SEARCHING...");
            }
            draw();
        }
        return;
    }

    if (state_ == AppState::MATCH_MENU) {
        // Ingame: only the DISPLAY column exists (col 0) — board size/speed/mode don't
        // apply to the game already in progress. From LOBBY: 3 columns as before, plus
        // DISPLAY as a 3rd column so everything really is in "the one menu".
        // -1 on the KataGo screen: it has no DISPLAY column, so no focus_col can
        // ever match it and the display-toggle branch below is simply unreachable.
        int display_col = match_menu_.ingame ? 0 : (match_menu_.katago_mode ? -1 : 2);
        int col_sizes[3], col_count;
        match_menu_cols(col_sizes, col_count);
        int n = col_sizes[match_menu_.focus_col];

        auto save_prefs = [&]() {
            for (int i = 0; i < 3; i++) match_prefs_.sizes[i]  = match_menu_.size_sel[i];
            for (int i = 0; i < 3; i++) match_prefs_.speeds[i] = match_menu_.speed_sel[i];
            match_prefs_.katago_str  = match_menu_.katago_str;
            match_prefs_.katago_size = match_menu_.katago_size;
        };
        auto close_menu = [&]() {
            save_prefs();
            save_settings();   // persist match prefs + this visit's DISPLAY toggles
            if (match_menu_.ingame) {
                state_ = pre_menu_state_;
            } else {
                state_ = AppState::LOBBY;
                set_status("");
            }
            draw();
        };
        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            match_menu_.focus_row = (match_menu_.focus_row - 1 + n) % n;
            renderer_->draw_match_menu(match_menu_);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            match_menu_.focus_row = (match_menu_.focus_row + 1) % n;
            renderer_->draw_match_menu(match_menu_);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            if (match_menu_.focus_col > 0) match_menu_.focus_col--;
            match_menu_.focus_row = std::min(match_menu_.focus_row, col_sizes[match_menu_.focus_col] - 1);
            renderer_->draw_match_menu(match_menu_);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            if (match_menu_.focus_col < col_count - 1) match_menu_.focus_col++;
            match_menu_.focus_row = std::min(match_menu_.focus_row, col_sizes[match_menu_.focus_col] - 1);
            renderer_->draw_match_menu(match_menu_);
            break;
        // L1/R1 used to flip OGS <-> KataGo mode here. That toggle row sat outside
        // the column grid (unclickable with a mouse) and was a memorized hotkey with
        // no on-screen affordance; the two settings sets are separate screens off
        // the popup menu now, so it was free to repurpose as raise/lower for the
        // BOARD HUE/SAT/VAL rows below — a no-op anywhere else in the menu.
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: {
            if (match_menu_.focus_col != display_col || match_menu_.focus_row < 9) break;
            int delta = (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 5 : -5;
            adjust_board_hsv(match_menu_.focus_row, delta);
            renderer_->draw_match_menu(match_menu_);
            break;
        }
        case SDL_CONTROLLER_BUTTON_A: {
            int r = match_menu_.focus_row;
            if (match_menu_.focus_col == display_col) {
                if (r == 0) {
                    match_menu_.show_coords_sel = !match_menu_.show_coords_sel;
                    show_coords_ = match_menu_.show_coords_sel;
                } else if (r == 1) {
                    // Greyed out and inert when no KataGo process is running at all —
                    // matches VS KATAGO/PRACTICE VS KATAGO's existing pattern of not
                    // offering a control that can't do anything.
                    if (!match_menu_.analysis_available) { /* no-op */ }
                    else {
                        match_menu_.analysis_sel = !match_menu_.analysis_sel;
                        kata_analysis_enabled_   = match_menu_.analysis_sel;
                        if (kata_analysis_enabled_) {
                            kata_query_after_ = SDL_GetTicks() + 1000;
                        } else {
                            kata_suggestion_count_ = 0;
                            kata_score_lead_ = FLT_MAX;
                            kata_query_after_ = 0;
                        }
                    }
                } else if (r == 2) {
                    match_menu_.chain_sel = !match_menu_.chain_sel;
                    chain_mode_           = match_menu_.chain_sel;
                } else if (r == 3) {
                    // SQUARE and OCTAGON are one shape choice wearing two checkboxes:
                    // ticking either clears the other, so the pair behaves like radio
                    // buttons that can both be off (= round stones).
                    match_menu_.square_sel = !match_menu_.square_sel;
                    square_stones_         = match_menu_.square_sel;
                    if (square_stones_) { match_menu_.octagon_sel = false; octagon_stones_ = false; }
                } else if (r == 4) {
                    match_menu_.octagon_sel = !match_menu_.octagon_sel;
                    octagon_stones_         = match_menu_.octagon_sel;
                    if (octagon_stones_) { match_menu_.square_sel = false; square_stones_ = false; }
                } else if (r == 5) {
                    match_menu_.square_grid_sel = !match_menu_.square_grid_sel;
                    square_grid_                = match_menu_.square_grid_sel;
                } else if (r == 6) {
                    // Same "can't offer a control that can't work" pattern as ENGINE ANALYSIS
                    if (!match_menu_.territory_available) { /* no-op */ }
                    else {
                        match_menu_.territory_sel = !match_menu_.territory_sel;
                        show_live_ownership_      = match_menu_.territory_sel;
                        if (show_live_ownership_) {
                            memset(game_.ownership, 0, sizeof(game_.ownership));  // no stale flash
                            request_live_ownership();
                        } else {
                            live_own_query_after_ = 0;
                        }
                    }
                } else if (r == 7) {
                    match_menu_.next_move_sel = !match_menu_.next_move_sel;
                    show_next_move_           = match_menu_.next_move_sel;
                } else if (r == 8) {
                    match_menu_.tetris_sel = !match_menu_.tetris_sel;
                    tetris_captures_       = match_menu_.tetris_sel;
                } else {
                    adjust_board_hsv(r, 5);   // rows 9-11; the [-]/[+] buttons handle their own click before reaching here
                }
            } else if (match_menu_.focus_col == 0) {
                if (match_menu_.katago_mode) {
                    match_menu_.katago_size = r;   // radio — local play's own size
                } else {
                    match_menu_.size_sel[r] = !match_menu_.size_sel[r];
                }
            } else if (match_menu_.katago_mode) {
                match_menu_.katago_str = r;   // radio — single select
            } else {
                match_menu_.speed_sel[r] = !match_menu_.speed_sel[r];
            }
            renderer_->draw_match_menu(match_menu_);
            break;
        }
        case SDL_CONTROLLER_BUTTON_BACK:
        case SDL_CONTROLLER_BUTTON_B:
            close_menu();
            break;
        case SDL_CONTROLLER_BUTTON_START:
            // Starting a search/local game from here was too easy to trigger by
            // accident — this menu only edits preferences now; START just closes
            // it (same as B/BACK), matching how it already worked mid-game. FIND
            // MATCH is reachable only through the LOBBY's own popup/confirm.
            close_menu();
            break;
        default: break;
        }
        return;
    }

    if (state_ == AppState::SEARCHING) {
        if (btn == SDL_CONTROLLER_BUTTON_B) {
            net_.cmd_cancel_match();
            state_ = AppState::LOBBY;
            set_status("");
            draw();
        } else if (btn == SDL_CONTROLLER_BUTTON_START) {
            open_popup_menu();
        }
        return;
    }

    if (state_ == AppState::PLAYING) {
        int n = game_.board_size - 1;
        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            game_.cursor_r = std::max(0, game_.cursor_r - 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            game_.cursor_r = std::min(n, game_.cursor_r + 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            game_.cursor_f = std::max(0, game_.cursor_f - 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            game_.cursor_f = std::min(n, game_.cursor_f + 1); draw(); break;

        case SDL_CONTROLLER_BUTTON_A:
            if (game_.history_pos >= 0) { game_.history_pos = -1; draw(); break; }  // exit history
            if (!game_.my_turn) break;
            if (game_.board.board[game_.cursor_r][game_.cursor_f] != 0) break;
            if (GoRules::would_be_suicide(game_.board.board,
                    game_.cursor_r, game_.cursor_f, game_.my_color,
                    game_.board_size)) break;
            // Ko check: simulate the move and compare against the board 2 states ago
            if (game_.history.size() >= 2) {
                char test[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
                memcpy(test, game_.board.board, sizeof(test));
                test[game_.cursor_r][game_.cursor_f] = game_.my_color ? 1 : 2;
                int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                int cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                int cap_count = 0;
                GoRules::find_captured(test, game_.my_color, game_.cursor_r, game_.cursor_f,
                                       cap_r, cap_f, cap_count, game_.board_size);
                for (int i = 0; i < cap_count; i++) test[cap_r[i]][cap_f[i]] = 0;
                const auto& prev = game_.history[game_.history.size() - 2];
                if (memcmp(test, prev.board, sizeof(test)) == 0) {
                    ko_flash_until_ = SDL_GetTicks() + 400;
                    draw();
                    break;
                }
            }
            if (is_local_game_) {
                play_move_sound(/*mine=*/true, apply_move(game_.cursor_f, game_.cursor_r));
                local_prev_was_pass_ = false;
                game_.my_turn = false;
                kata_gtp_.send_play(game_.my_color, game_.cursor_r, game_.cursor_f,
                                    game_.board_size);
                kata_gtp_.request_genmove(1 - game_.my_color);
                set_status("KATAGO THINKING...");
            } else {
                net_.cmd_send_move(game_.game_id, game_.cursor_f, game_.cursor_r);
                // Apply optimistically so stone appears immediately; server echo skipped
                play_move_sound(/*mine=*/true, apply_move(game_.cursor_f, game_.cursor_r));
                game_.pending_col = game_.cursor_f;
                game_.pending_row = game_.cursor_r;
                game_.my_turn = false;
                // Fresh byo-yomi periods from the moment the move is played —
                // don't wait on the server echo to stop a stale countdown.
                reset_byo_countdowns(/*running_player_too=*/true);
                game_.clock_tick = SDL_GetTicks();
                set_status("WAITING...");
            }
            request_live_ownership();
            draw();
            break;

        case SDL_CONTROLLER_BUTTON_B:
            if (!game_.my_turn) break;
            if (pass_confirm_) {
                pass_confirm_ = false;
                if (is_local_game_) {
                    apply_pass();
                    kata_gtp_.send_play(game_.my_color, -1, -1, game_.board_size);
                    local_prev_was_pass_ = true;
                    game_.my_turn = false;
                    kata_gtp_.request_genmove(1 - game_.my_color);
                    set_status("PASSED — KATAGO THINKING...");
                } else {
                    net_.cmd_send_pass(game_.game_id);
                    apply_pass();  // optimistic, same pattern as apply_move for real moves
                    game_.pending_col = -1;
                    game_.pending_row = -1;
                    game_.my_turn = false;
                    // Same byo-yomi reset as a real move — a pass consumes the turn too
                    reset_byo_countdowns(/*running_player_too=*/true);
                    game_.clock_tick = SDL_GetTicks();
                    set_status("PASSED — WAITING...");
                }
            } else {
                pass_confirm_ = true;
            }
            draw();
            break;

        case SDL_CONTROLLER_BUTTON_START:
            open_popup_menu();
            break;

        case SDL_CONTROLLER_BUTTON_X:
            undo_local_move();
            break;

        default: break;
        }
        return;
    }

    if (state_ == AppState::STONE_REMOVAL) {
        if (btn == SDL_CONTROLLER_BUTTON_START)
            open_popup_menu();
        else if (btn == SDL_CONTROLLER_BUTTON_A)
            accept_stone_removal();
        else if (btn == SDL_CONTROLLER_BUTTON_Y) {
            stone_removal_fill_view_ = !stone_removal_fill_view_;
            draw();
        }
        return;
    }

    if (state_ == AppState::GAME_OVER) {
        int n = game_.board_size - 1;
        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            game_.cursor_r = std::max(0, game_.cursor_r - 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            game_.cursor_r = std::min(n, game_.cursor_r + 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            game_.cursor_f = std::max(0, game_.cursor_f - 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            game_.cursor_f = std::min(n, game_.cursor_f + 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            // Jump between ALL branches that reach the current move number, anywhere
            // in the tree — not just siblings of the nearest fork. (The old nearest-
            // fork version couldn't reach a branch whose common ancestor was farther
            // up than the last fork below the cursor.)
            if (analysis_cur_ && analysis_root_) {
                int target_depth = analysis_cur_->depth;
                // Collect every node at this depth, in DFS order (matches the tree
                // panel's column order, so the cycle direction feels consistent).
                std::vector<AnalysisNode*> peers;
                std::function<void(AnalysisNode*)> walk = [&](AnalysisNode* node) {
                    if (node->depth == target_depth) {
                        peers.push_back(node);
                        return;  // children are all deeper — no need to descend
                    }
                    for (auto& c : node->children) walk(c.get());
                };
                walk(analysis_root_.get());

                if (peers.size() > 1) {
                    int cur_idx = 0;
                    for (int i = 0; i < (int)peers.size(); i++)
                        if (peers[i] == analysis_cur_) { cur_idx = i; break; }
                    int delta = (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1 : -1;
                    AnalysisNode* dest =
                        peers[(cur_idx + delta + (int)peers.size()) % (int)peers.size()];

                    // Re-aim every ancestor's active_child at the path to the new
                    // node, so LT/RT stepping follows the branch we just landed on.
                    for (AnalysisNode* p = dest; p->parent; p = p->parent)
                        for (int i = 0; i < (int)p->parent->children.size(); i++)
                            if (p->parent->children[i].get() == p) {
                                p->parent->active_child = i;
                                break;
                            }

                    analysis_cur_ = dest;
                    build_analysis_tree_render();
                    kata_suggestion_count_ = 0;
                    kata_score_lead_ = cached_analysis_score(analysis_cur_);
                    kata_query_after_ = SDL_GetTicks() + 1000;
                    draw();
                }
            }
            break;
        case SDL_CONTROLLER_BUTTON_A:
            // Guessing owns cross while it's on: committing a guess is the whole
            // interaction, and branching a hypothetical line mid-quiz would desync
            // the very main line being guessed against.
            if (guess_mode_) { guess_commit(); break; }
            if (analysis_cur_) {
                int col = game_.cursor_f, row = game_.cursor_r;
                // Setup-stone mode: edit the current node's board directly —
                // raw add/remove, no tree nodes, no captures, no turn change.
                // Gated on a childless position (children derive their boards
                // from this one; editing under them would desync the tree).
                if (analysis_setup_color_ >= 0) {
                    if (!analysis_cur_->children.empty()) {
                        flash_       = "POSITION HAS CONTINUATIONS — DELETE THEM FIRST";
                        flash_until_ = SDL_GetTicks() + 2000;
                    } else if (analysis_cur_->board.board[row][col] != 0) {
                        analysis_cur_->board.board[row][col] = 0;   // remove any stone
                    } else {
                        analysis_cur_->board.board[row][col] =
                            (analysis_setup_color_ == 1) ? 1 : 2;
                    }
                    draw();
                    break;
                }
                if (analysis_cur_->board.board[row][col] == 0) {
                    apply_analysis_move(col, row);
                    kata_suggestion_count_ = 0;
                    kata_score_lead_ = FLT_MAX;
                    if (kata_analysis_enabled_) {
                        kata_query_after_ = 0;
                        kata_for(game_.board_size).query_moves(
                            analysis_cur_->board.board, game_.board_size,
                            analysis_cur_->board.turn_is_black == 1, review_komi_);
                    }
                    draw();
                }
            }
            break;
        case SDL_CONTROLLER_BUTTON_B:
            // Letter labels: circle toggles an "A"/"B"/"C"… mark on the cursor point.
            // (Stepping back is L2's job — circle's old step-back was redundant.)
            if (analysis_cur_) {
                auto& labels = analysis_cur_->labels;
                int r = game_.cursor_r, f = game_.cursor_f;
                bool removed = false;
                for (size_t i = 0; i < labels.size(); i++)
                    if (labels[i].r == r && labels[i].f == f) {
                        labels.erase(labels.begin() + i);   // second press removes
                        removed = true;
                        break;
                    }
                if (!removed) {
                    bool used[26] = {};
                    for (const auto& l : labels)
                        if (l.ch >= 'A' && l.ch <= 'Z') used[l.ch - 'A'] = true;
                    for (int c = 0; c < 26; c++)
                        if (!used[c]) {                     // lowest unused letter
                            labels.push_back({r, f, char('A' + c)});
                            break;
                        }
                }
                draw();
            }
            break;
        case SDL_CONTROLLER_BUTTON_X:
            open_game_catalog();
            draw();
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            review_cycle(-1);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            // L3/R3: jump to the previous/next SGF in the same directory as the
            // currently open review — cycle marked positions or study puzzles
            // without a round trip through the catalog.
            review_cycle(+1);
            break;
        case SDL_CONTROLLER_BUTTON_START:
            open_popup_menu();
            break;
        }
        return;
    }
}

// ── START popup menu ─────────────────────────────────────────────────────────

// Rebuild the display labels from the item list; the armed row (awaiting its
// confirm press) is rewritten as a question so the state is visible in place.
void App::popup_sync_labels() {
    popup_labels_.clear();
    popup_styles_.clear();
    bool sectioned = false;
    for (const auto& it : popup_items_)
        if (it.heading) { sectioned = true; break; }
    for (int i = 0; i < (int)popup_items_.size(); i++) {
        popup_labels_.push_back(i == popup_armed_
                                    ? "REALLY " + popup_items_[i].label + "?"
                                    : popup_items_[i].label);
        // Styles are all-or-nothing per menu: a short popup with no headings
        // stays the plain white list it has always been rather than picking up
        // an indent and a tint for no reason.
        if (sectioned)
            popup_styles_.push_back({popup_items_[i].color, popup_items_[i].heading});
    }
}

int App::popup_step(int from, int dir) const {
    int n = (int)popup_items_.size();
    if (n == 0) return 0;
    for (int k = 1; k <= n; k++) {
        int i = ((from + dir * k) % n + n) % n;
        if (!popup_items_[i].heading) return i;
    }
    return from;   // nothing selectable at all — shouldn't happen, but don't hang
}

void App::draw_popup_overlay() {
    if (!popup_active_) return;
    renderer_->draw_popup_menu(popup_title_.c_str(), popup_labels_.data(),
                               (int)popup_labels_.size(), popup_index_,
                               popup_styles_.empty() ? nullptr : popup_styles_.data());
}

int App::popup_hit(int mx, int my, bool* inside_panel) const {
    if (inside_panel) *inside_panel = false;
    if (!popup_active_ || popup_labels_.empty()) return -1;
    // Same arguments make_ds hands the renderer, so the hit-test matches what's drawn.
    return renderer_->popup_item_at(popup_title_.c_str(), popup_labels_.data(),
                                    (int)popup_labels_.size(), mx, my, inside_panel,
                                    popup_styles_.empty() ? nullptr : popup_styles_.data());
}

void App::close_popup_menu() {
    popup_active_ = false;
    popup_items_.clear();
    popup_labels_.clear();
    popup_styles_.clear();
    popup_index_ = 0;
    popup_armed_ = -1;
}

void App::open_popup_menu() {
    popup_items_.clear();
    // Colour of the section currently being filled; every add() after a
    // section() inherits it, so the grouping is stated once per block rather
    // than repeated on every row.
    SDL_Color section_color = Palette::TEXT_WHITE;
    auto add = [&](std::string label, std::function<void()> fn, bool confirm = false) {
        popup_items_.push_back({std::move(label), confirm, std::move(fn), false,
                                Palette::menu_item_tint(section_color)});
    };
    auto section = [&](const char* label, SDL_Color color) {
        section_color = color;
        popup_items_.push_back({label, false, nullptr, true, color});
    };
    auto lobby = [this]() {
        state_ = AppState::LOBBY;
        set_status("");
    };

    switch (state_) {
    case AppState::LOBBY:
        popup_title_ = "LOBBY";
        // The lobby menu is the one long enough to need grouping: a dozen-odd
        // commands that fall into four plain jobs — start a game, train against
        // problems, look at games, change settings. Ordered by how often each is
        // reached, so playing is at the top and settings near the bottom.
        section("PLAY", Palette::MENU_PLAY);
        // Two separate items rather than one that changes meaning: which kind of
        // game you start is chosen here, explicitly, and never depends on what a
        // settings screen was last showing.
        if (!offline_)
            add("FIND MATCH", [this]() {
                net_.cmd_find_match(match_prefs_);
                state_ = AppState::SEARCHING;
                set_status("SEARCHING...");
            });
        if (!kata_human_model_.empty())
            add("PLAY VS KATAGO", [this]() { start_local_game(); });
        if (offline_) {
            add("LOG IN TO OGS", [this]() { return_to_login(); });
        } else {
            add("MY GAMES (CORRESPONDENCE)", [this]() { open_corr_list(); });
            add("CHALLENGES",               [this]() { open_challenges(); });
            add("CREATE CHALLENGE",         [this]() { open_challenge_create(); });
        }

        section("TRAIN", Palette::MENU_TRAIN);
        add("PUZZLES",         [this]() { open_puzzle_browser(); });
        add("WHO WON? QUIZ",   [this]() { open_who_won(); });
        add("JOSEKI EXPLORER", [this]() { open_joseki_explorer(); });

        section("REVIEW", Palette::MENU_REVIEW);
        add("GAME CATALOG",  [this]() { open_game_catalog(); });
        add("PRO GAMES",     [this]() { open_pro_catalog(); });
        add("FREE ANALYSIS", [this]() { start_free_analysis(); });

        section("SETTINGS", Palette::MENU_SETTINGS);
        add("MATCH SETTINGS", [this]() { open_settings_menu(); });
        // Same gate as PLAY VS KATAGO — no point offering strength settings for an
        // engine that isn't configured.
        if (!kata_human_model_.empty())
            add("KATAGO SETTINGS", [this]() { open_katago_settings(); });
        break;

    case AppState::SEARCHING:
        popup_title_ = "SEARCHING";
        add("CANCEL SEARCH", [this, lobby]() {
            net_.cmd_cancel_match();
            lobby();
        });
        break;

    case AppState::CORR_LIST:
        popup_title_ = "MY GAMES";
        add("REFRESH", [this]() {
            refresh_corr_list();
            set_status("MY GAMES");
        });
        add("RETURN TO LOBBY", lobby);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::CHALLENGES:
        popup_title_ = "CHALLENGES";
        add("REFRESH", [this]() {
            set_status("CHALLENGES — LOADING...");
            refresh_challenges();
        });
        add("CREATE CHALLENGE", [this]() { open_challenge_create(); });
        add("RETURN TO LOBBY", lobby);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::WHO_WON:
        popup_title_ = "WHO WON?";
        add("NEXT POSITION", [this]() { wq_load_next(); });
        if (wq_answered_ && !wq_path_.empty())
            add("REVIEW THIS GAME", [this]() { load_sgf_for_review(wq_path_); });
        add("RETURN TO LOBBY", [this]() {
            state_ = AppState::LOBBY;
            set_status("");
            load_demo_game();
        });
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::CHALLENGE_CREATE:
        popup_title_ = "NEW CHALLENGE";
        add("INCOMING CHALLENGES", [this]() { open_challenges(); });
        add("RETURN TO LOBBY", lobby);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::PLAYING:
        popup_title_ = "GAME MENU";
        // Correspondence: step away without ending the game — it stays your turn (or
        // the opponent's) and you drop back to the list. Listed first so it's the
        // easy, safe exit; RESIGN stays confirm-gated below it.
        if (is_corr_game_)
            add("BACK TO MY GAMES", [this]() { leave_corr_game(); });
        if (is_local_game_)
            add("UNDO MOVE", [this]() { undo_local_move(); });
        add("RESIGN", [this]() { do_resign(); }, /*confirm=*/true);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::STONE_REMOVAL:
        popup_title_ = "SCORING";
        add(is_local_game_ ? "SHOW RESULT" : "ACCEPT SCORE",
            [this]() { accept_stone_removal(); });
        add(stone_removal_fill_view_ ? "SHOW TERRITORY SQUARES" : "FILL TERRITORY WITH STONES",
            [this]() { stone_removal_fill_view_ = !stone_removal_fill_view_; });
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::GAME_OVER:
        popup_title_ = "REVIEW MENU";
        add("RETURN TO LOBBY", [this]() { return_to_lobby(); });
        // Guess the move — only offered on a file open for review, which is the
        // only place there is a next move to be scored against.
        if (guess_mode_ || guess_available())
            add(guess_mode_ ? "EXIT GUESS THE MOVE" : "GUESS THE MOVE",
                [this]() { toggle_guess_mode(); });
        // Fork the position on screen into a live game vs KataGo (needs the human
        // SL model, same gate as the match menu's VS KATAGO mode). Strength comes
        // from the configured KataGo strength in match settings.
        if (analysis_cur_ && !kata_human_model_.empty())
            add("PRACTICE VS KATAGO", [this]() { start_practice_from_position(); });
        // ── Drill authoring (life-and-death drill trees) ──
        if (analysis_cur_ && analysis_cur_->parent)   // the root can't be an ending
            add(analysis_cur_->drill_correct ? "UNMARK CORRECT ENDING"
                                             : "MARK CORRECT ENDING",
                [this]() {
                    analysis_cur_->drill_correct = !analysis_cur_->drill_correct;
                    build_analysis_tree_render();
                });
        // Setup-stone mode: cross places/removes raw stones of one color on the
        // current node's board (no tree nodes, no alternation) — for laying out
        // a drill's initial shape. Only allowed while the position has no
        // continuations, since children's boards derive from this one.
        if (analysis_cur_ && analysis_cur_->children.empty()) {
            add(analysis_setup_color_ == 1 ? "SETUP BLACK STONES (ON — TURN OFF)"
                                           : "SETUP BLACK STONES",
                [this]() {
                    analysis_setup_color_ = (analysis_setup_color_ == 1) ? -1 : 1;
                    set_status(analysis_setup_color_ == 1
                                   ? "SETUP: " GLYPH_PS_CROSS " ADDS/REMOVES BLACK STONES"
                                   : "");
                });
            add(analysis_setup_color_ == 0 ? "SETUP WHITE STONES (ON — TURN OFF)"
                                           : "SETUP WHITE STONES",
                [this]() {
                    analysis_setup_color_ = (analysis_setup_color_ == 0) ? -1 : 0;
                    set_status(analysis_setup_color_ == 0
                                   ? "SETUP: " GLYPH_PS_CROSS " ADDS/REMOVES WHITE STONES"
                                   : "");
                });
        }
        // Flip whose turn it is at the current node (which side the drill's
        // first move belongs to). Only future children read this flag.
        if (analysis_cur_)
            add(analysis_cur_->board.turn_is_black == 1 ? "SWITCH SIDE TO MOVE (NOW: BLACK)"
                                                        : "SWITCH SIDE TO MOVE (NOW: WHITE)",
                [this]() {
                    analysis_cur_->board.turn_is_black =
                        (analysis_cur_->board.turn_is_black == 1) ? 0 : 1;
                });
        // Prune an accidental line: removes the current node and everything
        // below it, landing on the parent. Hypothetical branches only — the
        // real game's moves (is_main_line) are never deletable.
        if (analysis_cur_ && analysis_cur_->parent && !analysis_cur_->is_main_line)
            add("DELETE THIS BRANCH", [this]() { delete_analysis_branch(); },
                /*confirm=*/true);
        // Which color the human solves as, independent of who moves first (e.g.
        // black moves first per the position, but white is the one being drilled —
        // black's opening then plays itself before you take over). Unset (-1)
        // tracks whoever moves first, matching plain alternating drills. Gated on
        // the tree's actual root, same as SAVE AS DRILL below — this is a
        // whole-drill property, not something tied to wherever you're browsing.
        if (analysis_root_ && !analysis_root_->children.empty()) {
            bool root_black    = analysis_root_->board.turn_is_black == 1;
            bool solver_black  = (drill_solver_black_ < 0) ? root_black : (drill_solver_black_ == 1);
            add(solver_black ? "SOLVE AS BLACK (TAP FOR WHITE)" : "SOLVE AS WHITE (TAP FOR BLACK)",
                [this, solver_black]() { drill_solver_black_ = solver_black ? 0 : 1; });
        }
        // Gated on the root, not analysis_cur_: SAVE always writes the whole tree
        // from the top (see save_drill()), so its availability shouldn't depend on
        // wherever you've scrolled to while reviewing.
        if (analysis_root_ && !analysis_root_->children.empty())
            add(drill_edit_path_.empty() ? "SAVE AS DRILL" : "OVERWRITE DRILL",
                [this]() { save_drill_from_current(); });
        add("GAME CATALOG",    [this]() { open_game_catalog(); });
        add("PRO GAMES",       [this]() { open_pro_catalog(); });
        add("SETTINGS",        [this]() { open_settings_menu(); });
        break;

    case AppState::PUZZLE_BROWSE:
        popup_title_ = "PUZZLES";
        // Local file management, only with a file highlighted in one of the
        // local lists (MY DRILLS / MISSED PUZZLES / FAVORITE PUZZLES)
        if (local_pz_kind_ != LocalPzKind::NONE && pz_view_ == PzView::PUZZLES &&
            pz_index_ >= 0 && pz_index_ < (int)drill_paths_.size()) {
            const char* kind_label = local_pz_kind_ == LocalPzKind::DRILLS    ? "DRILL"
                                    : local_pz_kind_ == LocalPzKind::MISSED   ? "MISSED PUZZLE"
                                    : local_pz_kind_ == LocalPzKind::BOOK      ? "PROBLEM"
                                                                              : "FAVORITE";
            add(std::string("RENAME ") + kind_label + " (KEYBOARD)", [this]() {
                drill_rename_buf_    = pz_list_[pz_index_].second;   // prefill current name
                drill_rename_active_ = true;
            });
            add(std::string("DELETE ") + kind_label, [this]() { drill_delete_selected(); },
                /*confirm=*/true);
        }
        add("RETURN TO LOBBY", lobby);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::JOSEKI:
        popup_title_ = "JOSEKI";
        add("RESTART FROM EMPTY BOARD", [this]() { open_joseki_explorer(); });
        add("RETURN TO LOBBY", lobby);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::PUZZLE_PLAY:
        popup_title_ = "PUZZLE MENU";
        add("RETRY PUZZLE", [this]() { pz_start(); });
        if (local_pz_kind_ == LocalPzKind::DRILLS && !drill_play_path_.empty())
            add("EDIT DRILL", [this]() { drill_edit_current(); });
        // Favoriting only makes sense for a real, identifiable OGS puzzle —
        // drills have no OGS id to key the favorite file on.
        if (pz_.id > 0) {
            std::string fav_path = Catalog::join_path(favorite_puzzles_dir(false),
                                                       "puzzle-" + std::to_string(pz_.id) + ".sgf");
            bool is_fav = path_exists(fav_path);
            add(is_fav ? "REMOVE FROM FAVORITES" : "ADD TO FAVORITES",
                [this]() { toggle_favorite_puzzle(); });
        }
        add("PUZZLE LIST", [this]() {
            state_    = AppState::PUZZLE_BROWSE;
            pz_view_  = pz_list_.empty() ? PzView::COLLECTIONS : PzView::PUZZLES;
            pz_index_ = std::max(0, pz_list_pos_);
        });
        add("RETURN TO LOBBY", lobby);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    default:
        return;  // no popup in this state
    }

    // QUIT is available from every menu (confirmed), so the whole app — including
    // exiting — is reachable by arrow keys / d-pad without memorizing hotkeys.
    // Chalk annotation, offered everywhere — it's a screen overlay, not tied to any
    // one screen. Listed as well as bound to SPACE so it's discoverable without
    // knowing the key.
    // This tail is shared by every popup, so it only takes a heading on the menus
    // that already have them: a two-item popup does not need dividing up.
    for (const auto& it : popup_items_)
        if (it.heading) { section("SCREEN", Palette::MENU_SCREEN); break; }
    add(draw_mode_ ? "STOP DRAWING" : "DRAW ON SCREEN", [this]() { toggle_draw_mode(); });
    if (renderer_->annot_has_content())
        add("CLEAR DRAWING", [this]() { renderer_->annot_clear(); draw(); });
    add("QUIT", [this]() { quit_requested_ = true; }, /*confirm=*/true);

    popup_active_ = true;
    popup_armed_  = -1;
    // Row 0 is a section heading whenever the menu is grouped, so start on the
    // first row that can actually be chosen.
    popup_index_  = (!popup_items_.empty() && popup_items_[0].heading)
                        ? popup_step(0, +1) : 0;
    popup_sync_labels();
    draw();
}

void App::handle_popup_button(Uint8 btn) {
    int n = (int)popup_items_.size();
    if (n == 0) { close_popup_menu(); draw(); return; }
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        popup_index_ = popup_step(popup_index_, -1);
        popup_armed_ = -1;
        popup_sync_labels();
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        popup_index_ = popup_step(popup_index_, +1);
        popup_armed_ = -1;
        popup_sync_labels();
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_A: {
        PopupItem& it = popup_items_[popup_index_];
        if (it.confirm && popup_armed_ != popup_index_) {
            popup_armed_ = popup_index_;
            popup_sync_labels();
            draw();
            break;
        }
        auto action = it.action;  // keep alive — close_popup_menu clears the items
        close_popup_menu();
        if (action) action();
        draw();
        break;
    }
    case SDL_CONTROLLER_BUTTON_B:
    case SDL_CONTROLLER_BUTTON_BACK:
    case SDL_CONTROLLER_BUTTON_START:
        close_popup_menu();
        draw();
        break;
    default: break;
    }
}

void App::do_resign() {
    if (is_local_game_) {
        // Player resigned → the other color wins
        end_local_game(std::string(game_.my_color == 1 ? "W" : "B") + "+R");
    } else {
        net_.cmd_send_resign(game_.game_id);
        set_status("RESIGNED");
        draw();
    }
}

void App::accept_stone_removal() {
    if (is_local_game_) {
        if (!local_game_score_.empty())
            end_local_game(local_game_score_);
        // else still analyzing — ignore the press
        return;
    }
    if (!stone_removal_has_ogs_territory_) {
        // Server hasn't sent its own dead-stone detection yet — what's on screen
        // is only our local KataGo guess. Sending accept now would tell the
        // server we accept an empty/wrong stone set, not what's displayed.
        flash_       = "WAITING FOR SERVER SCORE...";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    net_.cmd_accept_stones(game_.game_id);
    my_accept_sent_   = true;
    accept_resend_at_ = SDL_GetTicks() + 6000;
    set_status("ACCEPTING... WAITING FOR OPPONENT");
    draw();
}

void App::return_to_lobby() {
    save_companion();  // persist scores + marks before leaving review
    is_local_game_ = false;
    guess_mode_    = false;   // the quiz belongs to the review being left
    state_ = AppState::LOBBY;
    game_.history_pos = -1;
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();
    kata_suggestion_count_ = 0;
    kata_score_lead_ = FLT_MAX;
    kata_query_after_ = 0;
    kata_analysis_enabled_ = true;
    set_status("");
    game_.board.reset();
    load_demo_game();
}

// ── SGF save (fetched from OGS API) ──────────────────────────────────────────

static std::string sgf_sanitize(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum((unsigned char)c) || c == '-') out += c;
        else if (c == ' ' || c == '_')                  out += '_';
    }
    return out;
}

void App::save_live_game() {
    // Locate my_games/ directory — personal games, kept separate from the pro
    // library in games/ (same two-path probe as everywhere else)
    std::string games_dir = exe_dir() + "my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir() + "../../my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir();

    std::string my_name  = (game_.my_color == 1) ? game_.black_name : game_.white_name;
    std::string opp_name = (game_.my_color == 1) ? game_.white_name : game_.black_name;
    std::string player_dir = Catalog::join_path(games_dir, my_name);
    make_dir(games_dir);
    make_dir(player_dir);

    // Local games (vs KataGo) have game_id 0 — there is nothing to fetch from OGS
    // (the old code tried anyway, got HTTP 404, and silently saved no SGF at all,
    // leaving the .katago companion orphaned and the game impossible to reopen).
    // Write the SGF locally from our own move history instead — into a katago/
    // subdirectory so practice games don't mingle with the real OGS archive.
    // Local filenames get a time component: the opponent label is just the
    // strength ("15 KYU"), so two same-strength games on the same day would
    // otherwise overwrite each other.
    bool local = (game_.game_id == 0);
    std::string save_dir = player_dir;
    if (local) {
        save_dir = Catalog::join_path(player_dir, "katago");
        make_dir(save_dir);
    }

    time_t t = time(nullptr);
    char date[32];
    strftime(date, sizeof(date), local ? "%Y%m%d-%H%M%S" : "%Y%m%d", localtime(&t));
    std::string filename = std::string(date) + "-"
                         + sgf_sanitize(my_name) + "-"
                         + sgf_sanitize(opp_name) + ".sgf";
    std::string path = Catalog::join_path(save_dir, filename);

    // Derive companion path before writing/fetching the SGF
    companion_path_ = path.substr(0, path.rfind('.')) + ".katago";

    if (local) {
        save_local_sgf(path);
        return;
    }

    int game_id = game_.game_id;
    std::thread([this, game_id, path] {
        net_.fetch_sgf(game_id, path);
    }).detach();
}

// Write an SGF for a locally-played game by diffing consecutive history snapshots
// (the same move-recovery approach build_analysis_tree() uses). A snapshot pair with
// no added stone is a pass.
void App::save_local_sgf(const std::string& path) {
    if (game_.history.size() < 2) return;  // no moves — nothing worth saving

    FILE* f = Catalog::fopen_utf8(path, "wb");
    if (!f) return;

    char today[16];
    time_t t = time(nullptr);
    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&t));
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]SZ[%d]KM[%.1f]PB[%s]PW[%s]DT[%s]",
            game_.board_size, local_game_komi_,
            game_.black_name.c_str(), game_.white_name.c_str(), today);
    if (!game_.result.empty())
        fprintf(f, "RE[%s]", game_.result.c_str());

    // Practice-from-position games start from a non-empty board: write those
    // stones as AB[]/AW[] setup (not moves), plus PL[] for the side to move —
    // exactly the shape load_sgf() already parses for marked positions.
    const GameState& start = game_.history[0];
    bool has_setup = false;
    for (int color = 1; color <= 2; color++) {
        bool wrote_prop = false;
        for (int r = 0; r < game_.board_size; r++)
            for (int fcol = 0; fcol < game_.board_size; fcol++)
                if (start.board[r][fcol] == color) {
                    if (!wrote_prop) {
                        fprintf(f, "%s", color == 1 ? "AB" : "AW");
                        wrote_prop = true;
                        has_setup  = true;
                    }
                    fprintf(f, "[%c%c]", 'a' + fcol, 'a' + r);
                }
    }
    if (has_setup)
        fprintf(f, "PL[%c]", start.turn_is_black == 1 ? 'B' : 'W');
    fprintf(f, "\n");

    for (size_t i = 1; i < game_.history.size(); i++) {
        const GameState& prev = game_.history[i - 1];
        const GameState& cur  = game_.history[i];
        int mr = -1, mf = -1;
        for (int r = 0; r < game_.board_size && mr < 0; r++)
            for (int fcol = 0; fcol < game_.board_size; fcol++)
                if (cur.board[r][fcol] != 0 && prev.board[r][fcol] == 0) {
                    mr = r; mf = fcol; break;
                }
        // Mover color: the stone value where one was added, else (for a pass) whoever's
        // turn it was in the pre-move snapshot. Board values: 1 = black, 2 = white.
        int is_black = (mr >= 0) ? (cur.board[mr][mf] == 1) : (prev.turn_is_black == 1);
        if (mr >= 0)
            fprintf(f, ";%c[%c%c]", is_black ? 'B' : 'W', 'a' + mf, 'a' + mr);
        else
            fprintf(f, ";%c[]", is_black ? 'B' : 'W');
    }
    fprintf(f, ")\n");
    fclose(f);
}

// ── Marked positions (standalone flattened-SGF snapshots) ───────────────────

std::string App::marked_position_path(int depth) const {
    std::string games_dir = exe_dir() + "my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir() + "../../my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir();

    std::string player_dir = Catalog::join_path(games_dir, my_username_.empty() ? "You" : my_username_);
    std::string marked_dir = Catalog::join_path(player_dir, "marked");

    std::string opp_name = (game_.my_color == 1) ? game_.white_name : game_.black_name;
    time_t t = time(nullptr);
    char date[16];
    strftime(date, sizeof(date), "%Y%m%d", localtime(&t));
    std::string filename = std::string(date) + "-" + sgf_sanitize(opp_name)
                          + "-move" + std::to_string(depth) + ".sgf";
    return Catalog::join_path(marked_dir, filename);
}

void App::save_marked_position(int depth) {
    const GameState* gs = nullptr;
    if (state_ == AppState::PLAYING) {
        if (depth >= 0 && depth < (int)game_.history.size()) gs = &game_.history[depth];
    } else if (state_ == AppState::GAME_OVER && analysis_cur_) {
        gs = &analysis_cur_->board;
    }
    if (!gs) return;

    std::string path = marked_position_path(depth);
    std::string marked_dir = path.substr(0, path.find_last_of("/\\"));
    std::string player_dir = marked_dir.substr(0, marked_dir.find_last_of("/\\"));
    std::string games_dir  = player_dir.substr(0, player_dir.find_last_of("/\\"));
    make_dir(games_dir);
    make_dir(player_dir);
    make_dir(marked_dir);

    FILE* f = Catalog::fopen_utf8(path, "w");
    if (!f) return;

    auto sgf_escape = [](const std::string& s) {
        std::string out;
        for (char c : s) { if (c == ']' || c == '\\') out += '\\'; out += c; }
        return out;
    };

    int n = gs->board_size;
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]SZ[%d]", n);
    for (int color = 1; color >= 0; color--) {  // black setup stones first, then white
        bool any = false;
        for (int r = 0; r < n && !any; r++)
            for (int c = 0; c < n; c++)
                if (gs->board[r][c] == (color ? 1 : 2)) { any = true; break; }
        if (!any) continue;
        fprintf(f, "%s", color ? "AB" : "AW");
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
                if (gs->board[r][c] == (color ? 1 : 2))
                    fprintf(f, "[%c%c]", char('a' + c), char('a' + r));
    }
    fprintf(f, "PL[%s]", gs->turn_is_black ? "B" : "W");
    fprintf(f, "PB[%s]PW[%s]", sgf_escape(game_.black_name).c_str(), sgf_escape(game_.white_name).c_str());
    time_t t = time(nullptr);
    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", localtime(&t));
    fprintf(f, "DT[%s]", date);
    fprintf(f, "C[Marked position, move %d]", depth);
    fprintf(f, ")\n");
    fclose(f);

    if (depth < (int)marked_paths_.size()) marked_paths_[depth] = path;
}

void App::delete_marked_position(int depth) {
    if (depth < 0 || depth >= (int)marked_paths_.size()) return;
    if (marked_paths_[depth].empty()) return;  // marked in a prior session — no known path to clean up
    remove(marked_paths_[depth].c_str());
    marked_paths_[depth].clear();
}

// ── Life-and-death drill trees ───────────────────────────────────────────────
// Authored in analysis mode, saved as standard variation SGFs, drilled through
// the puzzle player. Format: root = AB/AW/PL setup (same shape as marked
// positions) + GN, tree = nested parens of ;B[xy]/;W[xy], correct endings
// marked C[RIGHT] (unmarked dead-ends are implicitly wrong — matches the
// puzzle player's judging).

static constexpr int DRILL_MAX_DEPTH = 400;

static std::string sgf_escape_text(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == ']' || c == '\\') out += '\\'; out += c; }
    return out;
}

// Shared resolver for every locally-stored puzzle set (drills, missed
// puzzles, favorites) — same my_games/<user>/<name>/ layout for each.
static std::string local_puzzle_subdir(const std::string& my_username, const char* name, bool create) {
    std::string games_dir = exe_dir() + "my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir() + "../../my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir();
    std::string player_dir = Catalog::join_path(games_dir, my_username.empty() ? "You" : my_username);
    std::string dir        = Catalog::join_path(player_dir, name);
    if (create) {
        make_dir(games_dir);
        make_dir(player_dir);
        make_dir(dir);
    }
    return dir;
}

std::string App::drills_dir(bool create) {
    return local_puzzle_subdir(my_username_, "drills", create);
}
std::string App::missed_puzzles_dir(bool create) {
    return local_puzzle_subdir(my_username_, "missed_puzzles", create);
}
std::string App::favorite_puzzles_dir(bool create) {
    return local_puzzle_subdir(my_username_, "favorite_puzzles", create);
}
std::string App::current_local_pz_dir(bool create) {
    switch (local_pz_kind_) {
        case LocalPzKind::MISSED:    return missed_puzzles_dir(create);
        case LocalPzKind::FAVORITES: return favorite_puzzles_dir(create);
        case LocalPzKind::BOOK:      return local_book_.dir;   // already resolved when opened
        default:                     return drills_dir(create);
    }
}

// Imported problem books live outside my_games/ — they aren't anyone's game
// history, they're shared content that travels with the repo (and with a
// packaged release, hence the same exe_dir()/../../ probe as the game library).
static std::string problem_sets_root() {
    std::string dir = exe_dir() + "problem_sets";
    if (!is_dir(dir)) dir = exe_dir() + "../../problem_sets";
    return dir;
}

// Read one integer SGF property (ZI[]/ZC[]) out of a file's root node without
// parsing the whole thing — listing a 400-problem set shouldn't mean building
// 400 move trees just to learn 400 ids. The root is the first node in the
// file, so a peek at the head is enough; 0 means absent.
static int sgf_peek_int(const std::string& path, const char* prop) {
    FILE* f = Catalog::fopen_utf8(path, "rb");
    if (!f) return 0;
    char head[4096];
    size_t n = fread(head, 1, sizeof(head) - 1, f);
    fclose(f);
    head[n] = '\0';
    std::string pat = std::string(prop) + "[";
    const char* at = strstr(head, pat.c_str());
    return at ? atoi(at + pat.size()) : 0;
}

// One line of "key=value" metadata out of a set.txt, or "" if absent.
static std::string set_meta(const std::string& path, const char* key) {
    FILE* f = Catalog::fopen_utf8(path, "r");
    if (!f) return "";
    char line[512];
    std::string want = std::string(key) + "=";
    std::string val;
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.compare(0, want.size(), want) == 0) { val = s.substr(want.size()); break; }
    }
    fclose(f);
    return val;
}

// Enumerate problem_sets/<slug>/ — every subfolder holding at least one SGF is
// a book. Display name and collection id come from its set.txt; a folder
// written by hand without one still works, it just loses solved tracking
// (id 0) and shows under its folder name.
void App::scan_local_books() {
    local_books_.clear();
    gp_dir_.clear();
    std::string root = problem_sets_root();
    if (!is_dir(root)) return;

    std::vector<std::string> names;
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(
        Catalog::utf8_to_wide(Catalog::join_path(root, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string name = Catalog::wide_to_utf8(fd.cFileName);
        if (name == "." || name == "..") continue;
        names.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(root.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (!is_dir(Catalog::join_path(root, name))) continue;
        names.push_back(name);
    }
    closedir(d);
#endif
    std::sort(names.begin(), names.end(),
              [](const std::string& a, const std::string& b) { return str_icmp(a, b) < 0; });

    for (const std::string& name : names) {
        LocalBook b;
        b.dir = Catalog::join_path(root, name);
        // The goproblems download isn't a book: it gets its own pin, ordered by
        // its index — and listing its ~13,000 files on every visit would drag.
        if (path_exists(Catalog::join_path(b.dir, "goproblems_index.tsv"))) {
            if (gp_dir_.empty()) gp_dir_ = b.dir;
            continue;
        }
        std::vector<std::string> files;
        if (!Catalog::list_sgf_files(b.dir, files) || files.empty()) continue;
        b.name          = set_meta(Catalog::join_path(b.dir, "set.txt"), "name");
        b.collection_id = atoi(set_meta(Catalog::join_path(b.dir, "set.txt"), "id").c_str());
        if (b.name.empty()) {
            b.name = name;
            for (char& c : b.name) if (c == '_' || c == '-') c = ' ';
        }
        // Pin ids start below the three fixed local pins (-1..-3) and are
        // assigned by scan order, so a row's id always resolves back here.
        b.pin_id = -10 - (int)local_books_.size();
        local_books_.push_back(std::move(b));
    }
}

// Validate the drill subtree below `n`: strictly alternating colors, no passes,
// bounded depth; notes whether any node is marked as a correct ending.
static bool drill_validate(const AnalysisNode* n, int expect_color,
                           bool& any_correct, int depth) {
    if (depth > DRILL_MAX_DEPTH) return false;
    for (const auto& ch : n->children) {
        if (ch->move_col < 0 || ch->move_row < 0) return false;   // pass node
        if (ch->move_color != expect_color)       return false;   // non-alternating
        if (ch->drill_correct) any_correct = true;
        if (!drill_validate(ch.get(), 1 - expect_color, any_correct, depth + 1))
            return false;
    }
    return true;
}

// Emit the subtree below `n`: a single child chains inline (;B[..];W[..]),
// multiple children each get their own (...) variation.
static void drill_write(FILE* f, const AnalysisNode* n) {
    const AnalysisNode* cur = n;
    while (true) {
        if (cur->children.empty()) return;
        if (cur->children.size() == 1) {
            const AnalysisNode* ch = cur->children[0].get();
            fprintf(f, ";%c[%c%c]", ch->move_color == 1 ? 'B' : 'W',
                    char('a' + ch->move_col), char('a' + ch->move_row));
            if (ch->drill_correct) fprintf(f, "C[RIGHT]");
            for (const auto& lb : ch->labels)
                fprintf(f, "LB[%c%c:%c]", char('a' + lb.f), char('a' + lb.r), lb.ch);
            cur = ch;   // chain without recursing
        } else {
            for (const auto& chp : cur->children) {
                const AnalysisNode* ch = chp.get();
                fprintf(f, "(;%c[%c%c]", ch->move_color == 1 ? 'B' : 'W',
                        char('a' + ch->move_col), char('a' + ch->move_row));
                if (ch->drill_correct) fprintf(f, "C[RIGHT]");
                for (const auto& lb : ch->labels)
                    fprintf(f, "LB[%c%c:%c]", char('a' + lb.f), char('a' + lb.r), lb.ch);
                drill_write(f, ch);
                fprintf(f, ")");
            }
            return;
        }
    }
}

bool App::save_drill(const std::string& path) {
    // Always the whole tree from its true root — not wherever the cursor
    // happens to be sitting while reviewing. Saving from analysis_cur_ used to
    // silently drop everything above/around it (setup stones included) the
    // moment you'd navigated deep into the tree before hitting save.
    if (!analysis_root_ || analysis_root_->children.empty()) {
        flash_       = "NOTHING TO SAVE — PLAY OUT SOME LINES FIRST";
        flash_until_ = SDL_GetTicks() + 2500;
        return false;
    }
    AnalysisNode* root_node = analysis_root_.get();
    const GameState& root = root_node->board;   // reference — GameState is huge
    bool any_correct = false;
    int  first_color = (root.turn_is_black == 1) ? 1 : 0;
    if (!drill_validate(root_node, first_color, any_correct, 0)) {
        flash_       = "DRILL MUST ALTERNATE COLORS, NO PASSES";
        flash_until_ = SDL_GetTicks() + 2500;
        return false;
    }
    if (!any_correct) {
        flash_       = "MARK A CORRECT ENDING FIRST";
        flash_until_ = SDL_GetTicks() + 2500;
        return false;
    }

    FILE* f = Catalog::fopen_utf8(path, "w");
    if (!f) {
        flash_       = "COULDN'T WRITE DRILL FILE";
        flash_until_ = SDL_GetTicks() + 2500;
        return false;
    }
    int n = root.board_size;
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]SZ[%d]", n);
    for (int color = 1; color >= 0; color--) {  // black setup stones first, then white
        bool any = false;
        for (int r = 0; r < n && !any; r++)
            for (int c = 0; c < n; c++)
                if (root.board[r][c] == (color ? 1 : 2)) { any = true; break; }
        if (!any) continue;
        fprintf(f, "%s", color ? "AB" : "AW");
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
                if (root.board[r][c] == (color ? 1 : 2))
                    fprintf(f, "[%c%c]", char('a' + c), char('a' + r));
    }
    fprintf(f, "PL[%s]", root.turn_is_black ? "B" : "W");
    bool solver_black_eff = (drill_solver_black_ < 0) ? (root.turn_is_black == 1)
                                                        : (drill_solver_black_ == 1);
    fprintf(f, "ZS[%s]", solver_black_eff ? "B" : "W");
    // GN = filename stem, for display in the drill list
    std::string stem = path;
    size_t slash = stem.find_last_of("/\\");
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    fprintf(f, "GN[%s]", sgf_escape_text(stem).c_str());
    time_t t = time(nullptr);
    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", localtime(&t));
    fprintf(f, "DT[%s]", date);
    // The task statement is about the solver's decision, not board truth — when
    // the opponent's opening move plays itself, it's still "your" puzzle to solve
    // as whichever color that is, not the color that happens to move first.
    fprintf(f, "C[%s TO PLAY]", solver_black_eff ? "BLACK" : "WHITE");
    drill_write(f, root_node);
    fprintf(f, ")\n");
    fclose(f);
    return true;
}

void App::save_drill_from_current() {
    std::string path = drill_edit_path_;
    if (path.empty()) {
        time_t t = time(nullptr);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
        path = Catalog::join_path(drills_dir(true), std::string("drill-") + stamp + ".sgf");
    }
    if (save_drill(path)) {
        drill_edit_path_ = path;
        flash_       = "DRILL SAVED";
        flash_until_ = SDL_GetTicks() + 2000;
    }
    draw();
}

// ── Drill SGF parser (variation-preserving) ─────────────────────────────────
// A separate parser from load_sgf(), which deliberately flattens variations —
// drills need the whole tree. Handles the subset our writer emits plus sane
// hand edits: root setup props, nested (…) variations, ;B[xy]/;W[xy] chains,
// C[RIGHT] correct marks, LB[] labels. Rejects passes, non-alternating
// colors, and pathological nesting.

// Read one "[...]" property value (assumes *p == '['), unescaping \] and \\.
static const char* drill_read_value(const char* p, std::string& out) {
    out.clear();
    p++;  // '['
    while (*p && *p != ']') {
        if (*p == '\\') { p++; if (!*p) break; }
        out += *p++;
    }
    return (*p == ']') ? p + 1 : p;
}

static void drill_trim(std::string& s) {
    while (!s.empty() && isspace((unsigned char)s.back()))  s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    s.erase(0, i);
}

// Parse "Sequence {GameTree}" — everything between a consumed '(' and its ')'.
// New move nodes chain onto *attach; sibling (…) subtrees attach to the current
// chain end. expect_color = color the next move node must have (1=B, 0=W).
static bool drill_parse_seq(const char*& p, PuzzleMoveNode* attach, int expect_color,
                            int move_depth, int nest_depth, int bs, std::string& err) {
    if (nest_depth > DRILL_MAX_DEPTH) { err = "TOO DEEPLY NESTED"; return false; }
    bool seen_subtree = false;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ')') { p++; return true; }
        if (*p == '(') {
            p++;
            seen_subtree = true;
            if (!drill_parse_seq(p, attach, expect_color, move_depth,
                                 nest_depth + 1, bs, err))
                return false;
            continue;
        }
        if (*p == ';') {
            if (seen_subtree) { err = "NODE AFTER VARIATION"; return false; }
            p++;
            // One node: a run of properties, each IDENT + one or more [values]
            PuzzleMoveNode* made = nullptr;
            while (*p) {
                while (*p && isspace((unsigned char)*p)) p++;
                if (!isupper((unsigned char)*p)) break;   // node/tree boundary
                std::string ident;
                while (isalpha((unsigned char)*p)) ident += *p++;
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p != '[') { err = "PROPERTY WITHOUT VALUE"; return false; }
                // Collect all values of this property
                std::vector<std::string> vals;
                while (*p == '[') {
                    std::string v;
                    p = drill_read_value(p, v);
                    vals.push_back(std::move(v));
                    while (*p && isspace((unsigned char)*p)) p++;
                }
                if (ident == "B" || ident == "W") {
                    if (made) { err = "TWO MOVES IN ONE NODE"; return false; }
                    int color = (ident == "B") ? 1 : 0;
                    if (color != expect_color) { err = "NON-ALTERNATING DRILL TREE"; return false; }
                    const std::string& v = vals[0];
                    bool is_pass = v.empty() || (bs <= 19 && v == "tt");
                    if (is_pass) { err = "PASSES NOT SUPPORTED"; return false; }
                    if (v.size() != 2) { err = "BAD MOVE COORD"; return false; }
                    int x = v[0] - 'a', y = v[1] - 'a';
                    if (x < 0 || x >= bs || y < 0 || y >= bs) { err = "MOVE OFF BOARD"; return false; }
                    if (move_depth + 1 > DRILL_MAX_DEPTH) { err = "DRILL TOO DEEP"; return false; }
                    attach->branches.emplace_back();
                    made = &attach->branches.back();
                    made->x = x;
                    made->y = y;
                    attach = made;
                    expect_color = 1 - expect_color;
                    move_depth++;
                } else if (ident == "C" && made) {
                    std::string c = vals[0];
                    drill_trim(c);
                    if (c == "RIGHT") made->correct = true;
                    else              made->text    = vals[0];
                } else if (ident == "LB" && made) {
                    for (const auto& v : vals) {
                        // "xy:C"
                        if (v.size() >= 4 && v[2] == ':') {
                            int x = v[0] - 'a', y = v[1] - 'a';
                            if (x >= 0 && x < bs && y >= 0 && y < bs)
                                made->marks.push_back({x, y, v[3]});
                        }
                    }
                }
                // all other properties: values already consumed, ignore
            }
            continue;
        }
        // Unexpected character — malformed
        err = "MALFORMED DRILL SGF";
        return false;
    }
    err = "UNTERMINATED DRILL SGF";
    return false;
}

// Shared core: parse a variation SGF (our writer's format, drill or saved-
// puzzle-copy alike) into pz. Recognizes the same root setup properties plus
// three custom ones (ZI/ZC/ZR — id/collection_id/rank) that saved-puzzle
// copies use to keep their real OGS identity; drill files simply never have
// them, so they're left at their zero defaults there. Returns false with no
// partial state on any parse error.
static bool load_variation_sgf_core(const std::string& path, OgsPuzzle& pz, std::string& err) {
    FILE* fp = Catalog::fopen_utf8(path, "rb");
    if (!fp) { err = "CAN'T OPEN"; return false; }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    rewind(fp);
    if (fsz <= 0 || fsz > 8 * 1024 * 1024) { fclose(fp); err = "BAD FILE SIZE"; return false; }
    std::vector<char> buf((size_t)fsz + 1);
    size_t nread = fread(buf.data(), 1, (size_t)fsz, fp);
    fclose(fp);
    buf[nread] = '\0';

    const char* p = buf.data();
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') { err = "NOT AN SGF"; return false; }
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ';') { err = "NO ROOT NODE"; return false; }
    p++;

    bool seen_zs = false;   // ZS[] present → explicit solver color, else defaults to PL[]
    // Root node: setup properties
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!isupper((unsigned char)*p)) break;   // end of root properties
        std::string ident;
        while (isalpha((unsigned char)*p)) ident += *p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '[') { err = "PROPERTY WITHOUT VALUE"; return false; }
        std::vector<std::string> vals;
        while (*p == '[') {
            std::string v;
            p = drill_read_value(p, v);
            vals.push_back(std::move(v));
            while (*p && isspace((unsigned char)*p)) p++;
        }
        if (ident == "SZ") {
            int sz = atoi(vals[0].c_str());
            pz.width = pz.height = sz;
        } else if (ident == "AB" || ident == "AW") {
            std::string& dst = (ident == "AB") ? pz.initial_black : pz.initial_white;
            for (const auto& v : vals)
                if (v.size() == 2) dst += v;
        } else if (ident == "PL") {
            pz.black_to_play = !vals[0].empty() && (vals[0][0] == 'B' || vals[0][0] == 'b' || vals[0][0] == '1');
        } else if (ident == "ZS") {
            // Solver's color, when it differs from who moves first (PL[]) — a drill
            // where the opponent's opening move plays itself before you take over.
            pz.solver_black = !vals[0].empty() && (vals[0][0] == 'B' || vals[0][0] == 'b' || vals[0][0] == '1');
            seen_zs = true;
        } else if (ident == "GN") {
            pz.name = vals[0];
        } else if (ident == "C") {
            pz.description = vals[0];
        } else if (ident == "ZI") {
            pz.id = atoi(vals[0].c_str());
        } else if (ident == "ZC") {
            pz.collection_id = atoi(vals[0].c_str());
        } else if (ident == "ZR") {
            pz.rank = atoi(vals[0].c_str());
        } else if (ident == "B" || ident == "W") {
            err = "MOVE IN ROOT NODE";   // our format keeps the root move-free
            return false;
        }
        // everything else (GM/FF/CA/DT/PB/PW/KM/...) ignored
    }
    if (!seen_zs) pz.solver_black = pz.black_to_play;

    if (pz.width != pz.height || pz.width < 2 || pz.width > MAX_BOARD_SIZE) {
        err = "BAD BOARD SIZE";
        return false;
    }

    // Variation tree: chained nodes + sibling subtrees until the closing ')'
    int first_color = pz.black_to_play ? 1 : 0;
    if (!drill_parse_seq(p, &pz.tree, first_color, 0, 0, pz.width, err))
        return false;
    if (pz.tree.branches.empty()) { err = "NO MOVES IN FILE"; return false; }
    pz.tree.correct = false;   // a C[RIGHT] on the root would insta-solve
    return true;
}

static std::string filename_stem(const std::string& path) {
    std::string stem = path;
    size_t slash = stem.find_last_of("/\\");
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    return stem;
}

// Load a drill SGF into a synthesized local OgsPuzzle. Always id=0 — drills
// are the player's own invention, never an OGS solve to track, even if a
// hand-edited file happens to carry ZI/ZC/ZR from being copy-pasted.
static bool load_drill_sgf(const std::string& path, OgsPuzzle& out_pz, std::string& err) {
    OgsPuzzle pz;
    if (!load_variation_sgf_core(path, pz, err)) return false;
    pz.id              = 0;
    pz.opponent_auto   = true;
    pz.type            = "life_and_death";
    pz.collection_id   = 0;
    pz.collection_name = "MY DRILLS";
    if (pz.name.empty()) pz.name = filename_stem(path);
    if (pz.description.empty())
        pz.description = pz.solver_black ? "BLACK TO PLAY" : "WHITE TO PLAY";
    out_pz = std::move(pz);
    return true;
}

// Load a saved copy of a real OGS puzzle (missed or favorited) — unlike
// drills, the real id/collection_id/rank are preserved (from ZI/ZC/ZR, if
// present), so re-solving it correctly updates OGS solved-puzzle tracking
// exactly as solving it live would.
static bool load_saved_puzzle_sgf(const std::string& path, OgsPuzzle& out_pz, std::string& err) {
    OgsPuzzle pz;
    if (!load_variation_sgf_core(path, pz, err)) return false;
    pz.opponent_auto = true;
    if (pz.type.empty())            pz.type            = "study";
    if (pz.collection_name.empty()) pz.collection_name = "SAVED";
    if (pz.name.empty()) pz.name = filename_stem(path);
    if (pz.description.empty())
        pz.description = pz.solver_black ? "BLACK TO PLAY" : "WHITE TO PLAY";
    out_pz = std::move(pz);
    return true;
}

// Write a full copy of a fetched OGS puzzle (setup + entire authored tree,
// including the real id/collection_id/rank) to a standalone SGF — the
// missed-puzzle and favorites counterpart of save_drill(), but sourced
// directly from a PuzzleMoveNode tree rather than an AnalysisNode one (a
// fetched puzzle already IS in that shape, no conversion needed).
static void pz_tree_write(FILE* f, const PuzzleMoveNode& n, int color) {
    if (n.branches.empty()) return;
    auto emit_node = [&](const PuzzleMoveNode& ch) {
        fprintf(f, ";%c[%c%c]", color ? 'B' : 'W',
                char('a' + ch.x), char('a' + ch.y));
        if (ch.correct)            fprintf(f, "C[RIGHT]");
        else if (!ch.text.empty()) fprintf(f, "C[%s]", sgf_escape_text(ch.text).c_str());
        for (const auto& m : ch.marks)
            fprintf(f, "LB[%c%c:%c]", char('a' + m.x), char('a' + m.y), m.ch);
    };
    if (n.branches.size() == 1) {
        emit_node(n.branches[0]);
        pz_tree_write(f, n.branches[0], 1 - color);
    } else {
        for (const auto& ch : n.branches) {
            fprintf(f, "(");
            emit_node(ch);
            pz_tree_write(f, ch, 1 - color);
            fprintf(f, ")");
        }
    }
}

static bool write_puzzle_copy_sgf(const std::string& path, const OgsPuzzle& pz) {
    FILE* f = Catalog::fopen_utf8(path, "w");
    if (!f) return false;
    int n = pz.width;
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]SZ[%d]", n);
    auto emit_setup = [&](const std::string& coords, const char* prop) {
        if (coords.empty()) return;
        fprintf(f, "%s", prop);
        for (size_t i = 0; i + 1 < coords.size(); i += 2)
            fprintf(f, "[%c%c]", coords[i], coords[i + 1]);
    };
    emit_setup(pz.initial_black, "AB");
    emit_setup(pz.initial_white, "AW");
    fprintf(f, "PL[%s]", pz.black_to_play ? "B" : "W");
    fprintf(f, "GN[%s]", sgf_escape_text(pz.name).c_str());
    if (!pz.description.empty())
        fprintf(f, "C[%s]", sgf_escape_text(pz.description).c_str());
    time_t t = time(nullptr);
    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", localtime(&t));
    fprintf(f, "DT[%s]", date);
    if (pz.id > 0)            fprintf(f, "ZI[%d]", pz.id);
    if (pz.collection_id > 0) fprintf(f, "ZC[%d]", pz.collection_id);
    if (pz.rank > 0)          fprintf(f, "ZR[%d]", pz.rank);
    pz_tree_write(f, pz.tree, pz.black_to_play ? 1 : 0);
    fprintf(f, ")\n");
    fclose(f);
    return true;
}

// Apply a random rotation/reflection (one of the 8 board symmetries) to a
// loaded drill — setup stones, every tree move, and author marks together —
// so a shape isn't only ever learned in the corner it was authored in.
// The board cursor (cur_f/cur_r, may be null) rides along through the same
// map, so the player's hand stays on the shape instead of chasing it.
// Exact integer maps; composing another one on a later retry is still a
// symmetry, so repeated in-place application never drifts.
static void drill_random_transform(OgsPuzzle& pz, int* cur_f, int* cur_r) {
    int op = rand() % 8;
    if (op == 0) return;   // identity
    int n = pz.width;
    auto tf = [op, n](int& x, int& y) {
        int ox = x, oy = y;
        switch (op) {
        case 1: x = n - 1 - ox; y = oy;          break;  // mirror horizontally
        case 2: x = ox;         y = n - 1 - oy;  break;  // mirror vertically
        case 3: x = n - 1 - ox; y = n - 1 - oy;  break;  // rotate 180
        case 4: x = oy;         y = ox;          break;  // transpose
        case 5: x = oy;         y = n - 1 - ox;  break;  // rotate 90
        case 6: x = n - 1 - oy; y = ox;          break;  // rotate 270
        case 7: x = n - 1 - oy; y = n - 1 - ox;  break;  // anti-transpose
        }
    };
    auto tf_coords = [&](std::string& s) {
        for (size_t i = 0; i + 1 < s.size(); i += 2) {
            int x = s[i] - 'a', y = s[i + 1] - 'a';
            if (x < 0 || x >= n || y < 0 || y >= n) continue;
            tf(x, y);
            s[i]     = char('a' + x);
            s[i + 1] = char('a' + y);
        }
    };
    tf_coords(pz.initial_black);
    tf_coords(pz.initial_white);
    std::function<void(PuzzleMoveNode&)> rec = [&](PuzzleMoveNode& nd) {
        if (nd.x >= 0 && nd.y >= 0) tf(nd.x, nd.y);
        for (auto& m : nd.marks)
            if (m.x >= 0 && m.y >= 0) tf(m.x, m.y);
        for (auto& b : nd.branches) rec(b);
    };
    rec(pz.tree);
    if (cur_f && cur_r &&
        *cur_f >= 0 && *cur_f < n && *cur_r >= 0 && *cur_r < n)
        tf(*cur_f, *cur_r);
}

// ── Auto-detected study puzzles ──────────────────────────────────────────────

// Evaluate depth d ("position before the move played at d") against the study-
// puzzle criterion: it was my turn, exactly one move kept the game, and I played
// something else that lost it. Uses only data the background sweep already
// produced — no extra KataGo queries. Called only once the game is over.
void App::check_puzzle(int d) {
    if (game_.my_color != 0 && game_.my_color != 1) return;   // review — not my game
    if (puzzle_saved_.count(d)) return;
    if (d < 0 || d + 1 >= (int)game_.history.size()) return;
    auto it = puzzle_eval_.find(d);
    if (it == puzzle_eval_.end() || it->second.best_sl == FLT_MAX) return;
    const GameState& pos = game_.history[d];
    if ((int)pos.turn_is_black != game_.my_color) return;     // opponent's move
    if (d + 1 >= (int)move_scores_.size() || move_scores_[d + 1] == FLT_MAX) return;

    const float SAVABLE    = -2.0f;  // best move leaves me at worst ~2 pts behind
    const float ONLY_MOVE  =  8.0f;  // every alternative loses at least this much more
    const float AFTER_LOST = -7.0f;  // my actual move left me at least this far behind

    float mp        = (game_.my_color == 1) ? 1.f : -1.f;     // to my perspective
    float best_my   = mp * it->second.best_sl;
    float second_my = (it->second.second_sl == FLT_MAX) ? -1e9f : mp * it->second.second_sl;
    float after_my  = mp * move_scores_[d + 1];

    if (best_my < SAVABLE)               return;  // game was already gone
    if (second_my > best_my - ONLY_MOVE) return;  // more than one way to save it
    if (after_my > AFTER_LOST)           return;  // my move didn't actually lose it

    // Direct coordinate check as a final guard: if I somehow played the saving
    // move itself (and the score drop came from elsewhere/noise), don't mark.
    int pr = -1, pf = -1;
    const GameState& nxt = game_.history[d + 1];
    for (int r = 0; r < pos.board_size && pr < 0; r++)
        for (int c = 0; c < pos.board_size; c++)
            if (nxt.board[r][c] != 0 && pos.board[r][c] == 0) { pr = r; pf = c; break; }
    if (pr == it->second.best_r && pf == it->second.best_f) return;

    save_puzzle_position(d);
    puzzle_saved_.insert(d);
    flash_       = "STUDY PUZZLE SAVED (MOVE " + std::to_string(d) + ")";
    flash_until_ = SDL_GetTicks() + 2500;
}

// Write the puzzle as a flattened setup SGF (same format as manual marked
// positions) into games/<user>/puzzles/. The saving move is deliberately NOT
// recorded anywhere in the file — reviewing the puzzle with engine analysis
// reveals the answer on demand instead of spoiling it up front.
void App::save_puzzle_position(int depth) {
    if (depth < 0 || depth >= (int)game_.history.size()) return;
    const GameState* gs = &game_.history[depth];

    std::string games_dir = exe_dir() + "my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir() + "../../my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir();
    std::string player_dir = Catalog::join_path(games_dir, my_username_.empty() ? "You" : my_username_);
    std::string puzzle_dir = Catalog::join_path(player_dir, "puzzles");
    make_dir(player_dir);
    make_dir(puzzle_dir);

    std::string opp_name = (game_.my_color == 1) ? game_.white_name : game_.black_name;
    time_t t = time(nullptr);
    char date[32];
    strftime(date, sizeof(date), "%Y%m%d-%H%M%S", localtime(&t));
    std::string path = Catalog::join_path(puzzle_dir,
        std::string(date) + "-" + sgf_sanitize(opp_name) + "-move" + std::to_string(depth) + ".sgf");

    FILE* f = Catalog::fopen_utf8(path, "w");
    if (!f) return;
    auto sgf_escape = [](const std::string& s) {
        std::string out;
        for (char c : s) { if (c == ']' || c == '\\') out += '\\'; out += c; }
        return out;
    };
    int n = gs->board_size;
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]SZ[%d]", n);
    for (int color = 1; color >= 0; color--) {  // black setup stones first, then white
        bool any = false;
        for (int r = 0; r < n && !any; r++)
            for (int c = 0; c < n; c++)
                if (gs->board[r][c] == (color ? 1 : 2)) { any = true; break; }
        if (!any) continue;
        fprintf(f, "%s", color ? "AB" : "AW");
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
                if (gs->board[r][c] == (color ? 1 : 2))
                    fprintf(f, "[%c%c]", char('a' + c), char('a' + r));
    }
    fprintf(f, "PL[%s]", gs->turn_is_black ? "B" : "W");
    fprintf(f, "PB[%s]PW[%s]", sgf_escape(game_.black_name).c_str(), sgf_escape(game_.white_name).c_str());
    char day[16];
    strftime(day, sizeof(day), "%Y-%m-%d", localtime(&t));
    fprintf(f, "DT[%s]", day);
    fprintf(f, "C[Study puzzle: one move keeps the game. From move %d.]", depth);
    fprintf(f, ")\n");
    fclose(f);
}

void App::save_companion() {
    if (companion_path_.empty()) return;
    if (move_scores_.empty() && move_marked_.empty()) return;

    FILE* f = Catalog::fopen_utf8(companion_path_, "w");
    if (!f) return;

    // Scores line: "S val val val ..." (nan for unknown)
    fprintf(f, "S");
    for (int i = 0; i < (int)move_scores_.size(); i++) {
        float v = move_scores_[i];
        if (v == FLT_MAX) fprintf(f, " nan");
        else              fprintf(f, " %.4g", v);
    }
    fprintf(f, "\n");

    // Marked line: "M depth depth ..."
    fprintf(f, "M");
    for (int i = 0; i < (int)move_marked_.size(); i++)
        if (move_marked_[i]) fprintf(f, " %d", i);
    fprintf(f, "\n");

    fclose(f);
}

void App::load_companion() {
    if (companion_path_.empty()) return;
    FILE* f = Catalog::fopen_utf8(companion_path_, "r");
    if (!f) return;

    char line[65536];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'S') {
            char* p = line + 1;
            int idx = 0;
            while (*p && idx < (int)move_scores_.size()) {
                while (*p == ' ') p++;
                if (strncmp(p, "nan", 3) == 0) { p += 3; idx++; }
                else { move_scores_[idx++] = (float)strtod(p, &p); }
            }
        } else if (line[0] == 'M') {
            char* p = line + 1;
            while (*p) {
                while (*p == ' ' || *p == '\n' || *p == '\r') p++;
                if (!*p) break;
                int d = (int)strtol(p, &p, 10);
                if (d >= 0 && d < (int)move_marked_.size())
                    move_marked_[d] = true;
            }
        }
    }
    fclose(f);
}

// ── Local game vs KataGo ─────────────────────────────────────────────────────

static const char* const KATA_GTP_PROFILES[8] = {
    "preaz_20k", "preaz_15k", "preaz_10k", "preaz_5k",
    "preaz_1k",  "preaz_1d",  "preaz_5d",  "preaz_9d"
};
static const char* const KATA_GTP_NAMES[8] = {
    "20 KYU", "15 KYU", "10 KYU", "5 KYU",
    "1 KYU",  "1 DAN",  "5 DAN",  "9 DAN"
};

// Adaptive strength: a fractional position on KataGo's full human-profile ladder,
// rank index 0..28 → 20k..1k (0..19) then 1d..9d (20..28).
static constexpr int KATA_RANK_MAX = 28;
static std::string kata_rank_profile(int idx) {
    return idx <= 19 ? "preaz_" + std::to_string(20 - idx) + "k"
                     : "preaz_" + std::to_string(idx - 19) + "d";
}
static std::string kata_rank_label(int idx) {
    return idx <= 19 ? std::to_string(20 - idx) + " KYU"
                     : std::to_string(idx - 19) + " DAN";
}

void App::start_local_game() {
    is_corr_game_ = false;  // a local game is never a correspondence game
    // Board size comes from KATAGO SETTINGS' own single-select, not the OGS
    // multi-select — those are separate settings on separate screens now.
    static const int kSizes[3] = {9, 13, 19};
    int bs = kSizes[std::max(0, std::min(2, match_prefs_.katago_size))];

    int str = match_prefs_.katago_str;
    if (str < 0 || str > 8) str = 2;

    // Index 8 = ADAPTIVE: resolve the fractional rating to the nearest profile rank
    std::string profile, opp_label;
    adaptive_game_ = (str == 8);
    if (adaptive_game_) {
        int idx = (int)std::lround(adaptive_rank_);
        idx = std::max(0, std::min(KATA_RANK_MAX, idx));
        profile   = kata_rank_profile(idx);
        opp_label = "ADAPTIVE (" + kata_rank_label(idx) + ")";
    } else {
        profile   = KATA_GTP_PROFILES[str];
        opp_label = KATA_GTP_NAMES[str];
    }

    if (!kata_gtp_.start(kata_exe_, kata_model_, kata_human_model_,
                         profile, bs, 7.5f)) {
        flash_       = "FAILED TO START KATAGO";
        flash_until_ = SDL_GetTicks() + 3000;
        state_ = AppState::LOBBY;
        set_status("");
        draw();
        return;
    }

    game_.result.clear();
    game_.pending_col   = -2;
    game_.pending_row   = -2;
    game_.history.clear();
    game_.history_pos   = -1;
    memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
    memset(game_.ownership,   0, sizeof(game_.ownership));
    live_own_pending_     = false;
    live_own_query_after_ = 0;
    game_.game_id       = 0;
    game_.board_size    = bs;
    game_.my_color      = rand() % 2;   // randomly Black or White each game
    game_.my_player_id  = 0;
    std::string my_name = my_username_.empty() ? "You" : my_username_;
    game_.black_name    = (game_.my_color == 1) ? my_name : opp_label;
    game_.white_name    = (game_.my_color == 0) ? my_name : opp_label;
    game_.black_rank    = game_.white_rank = "";
    game_.black_secs = game_.white_secs = -1;
    game_.black_periods = game_.white_periods = -1;
    game_.black_period_secs = game_.white_period_secs = -1;
    game_.clock_tick    = 0;
    game_.cursor_r = game_.cursor_f = bs / 2;
    game_.board.reset();
    game_.board.board_size    = bs;
    game_.board.turn_is_black = 1;
    game_.handicap      = 0;
    game_.free_handicap = false;
    game_.history.push_back(game_.board);

    // Fresh score-graph / mark storage — never inherit the previous game's arrays.
    // (Leftovers made the new game's graph show the *previous* game's scores, made
    // the background sweep think those depths were already done, and let undo pop
    // scores that belonged to a different game entirely.)
    move_scores_.assign(game_.history.size(), FLT_MAX);
    move_marked_.assign(game_.history.size(), false);
    marked_paths_.assign(game_.history.size(), "");
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    puzzle_eval_.clear();
    puzzle_saved_.clear();
    review_path_.clear();  // this GAME_OVER will be a live game's, not a loaded file

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;

    is_local_game_        = true;
    local_prev_was_pass_  = false;
    game_.my_turn         = (game_.my_color == 1);   // Black always moves first

    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_analysis_enabled_ = false;   // analysis overlay off during live play
    review_komi_           = 7.5f;    // fresh local games use the standard komi
    local_game_komi_       = 7.5f;

    state_ = AppState::PLAYING;
    sound_.play_game_start();
    if (game_.my_turn) {
        set_status(std::string("YOUR TURN  (") + (game_.my_color == 1 ? "BLACK" : "WHITE") + ")");
    } else {
        // Player is White — KataGo (Black) opens
        kata_gtp_.request_genmove(1 - game_.my_color);
        set_status("KATAGO THINKING...");
    }
    draw();
}

void App::start_practice_from_position() {
    if (state_ != AppState::GAME_OVER || !analysis_cur_) return;
    is_corr_game_ = false;  // a practice game is local, never correspondence

    // Reference the position in place — GameState is ~12MB (it embeds a full
    // GameSnapshot[MAX_MOVES] by value), so it must never be a local/by-value
    // copy (that mistake stack-overflowed the joseki explorer; see jk_arrive's
    // fix). analysis_cur_ stays valid until analysis_root_.reset() below, so
    // everything that needs the board data must happen before that point.
    const GameState& pos = analysis_cur_->board;
    int       bs   = game_.board_size;
    float     komi = review_komi_;
    bool      pos_turn_is_black = (pos.turn_is_black == 1);

    int str = match_prefs_.katago_str;
    if (str < 0 || str > 8) str = 2;
    std::string profile, opp_label;
    if (str == 8) {
        int idx = std::max(0, std::min(KATA_RANK_MAX, (int)std::lround(adaptive_rank_)));
        profile   = kata_rank_profile(idx);
        opp_label = "ADAPTIVE (" + kata_rank_label(idx) + ")";
    } else {
        profile   = KATA_GTP_PROFILES[str];
        opp_label = KATA_GTP_NAMES[str];
    }
    // Practice positions can be arbitrarily lopsided — a win or loss says nothing
    // about rank, so never let one move the adaptive level.
    adaptive_game_ = false;

    if (!kata_gtp_.start(kata_exe_, kata_model_, kata_human_model_,
                         profile, bs, komi)) {
        flash_       = "FAILED TO START KATAGO";
        flash_until_ = SDL_GetTicks() + 3000;
        draw();
        return;
    }
    // Seed the position stone by stone as raw GTP play commands (GTP doesn't
    // require alternating colors). Order can't matter: any subset of a legal
    // position is itself legal — a partially-placed group's missing stones are
    // liberties — so no placement can ever trigger a capture mid-seed.
    for (int r = 0; r < bs; r++)
        for (int f = 0; f < bs; f++)
            if (pos.board[r][f] != 0)
                kata_gtp_.send_play(pos.board[r][f] == 1 ? 1 : 0, r, f, bs);

    // Copy the position into its final destination now, while `pos` (a
    // reference into the analysis tree) is still valid — the tree is torn
    // down right below.
    game_.board            = pos;
    game_.board.board_size = bs;

    save_companion();  // persist the review's scores + marks before leaving it
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();

    game_.result.clear();
    game_.pending_col   = -2;
    game_.pending_row   = -2;
    game_.history.clear();
    game_.history_pos   = -1;
    memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
    memset(game_.ownership,   0, sizeof(game_.ownership));
    live_own_pending_     = false;
    live_own_query_after_ = 0;
    game_.game_id       = 0;
    game_.board_size    = bs;
    game_.my_color      = pos_turn_is_black ? 1 : 0;  // player takes the side to move
    game_.my_player_id  = 0;
    std::string my_name = my_username_.empty() ? "You" : my_username_;
    game_.black_name    = (game_.my_color == 1) ? my_name : opp_label;
    game_.white_name    = (game_.my_color == 0) ? my_name : opp_label;
    game_.black_rank    = game_.white_rank = "";
    game_.black_secs = game_.white_secs = -1;
    game_.black_periods = game_.white_periods = -1;
    game_.black_period_secs = game_.white_period_secs = -1;
    game_.clock_tick    = 0;
    // game_.board was already copied from pos above, before the analysis tree
    // that backs it was torn down
    game_.handicap      = 0;
    game_.free_handicap = false;
    game_.history.push_back(game_.board);

    // Fresh score-graph / mark storage, same as start_local_game
    move_scores_.assign(game_.history.size(), FLT_MAX);
    move_marked_.assign(game_.history.size(), false);
    marked_paths_.assign(game_.history.size(), "");
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    puzzle_eval_.clear();
    puzzle_saved_.clear();
    review_path_.clear();      // the coming GAME_OVER belongs to the practice game
    companion_path_.clear();   // save_live_game assigns the practice game its own

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;

    is_local_game_        = true;
    local_prev_was_pass_  = false;
    local_game_score_.clear();
    local_game_komi_      = komi;
    game_.my_turn         = true;   // by construction — player took the side to move

    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_query_after_      = 0;
    kata_analysis_enabled_ = false;   // analysis overlay off during live play

    state_ = AppState::PLAYING;
    sound_.play_game_start();
    set_status(std::string("PRACTICE — YOUR TURN  (")
               + (game_.my_color == 1 ? "BLACK" : "WHITE") + ")");
    draw();
}

void App::handle_katago_gtp_move(int row, int col) {
    const char* my_col_str = (game_.my_color == 1) ? "B" : "W";
    std::string my_turn_status = std::string("YOUR TURN  (")
                                + (game_.my_color == 1 ? "BLACK" : "WHITE") + ")";
    if (row == -2) {
        // KataGo resigned — show territory view before going to analysis
        flash_       = "KATAGO RESIGNED — YOU WIN!";
        flash_until_ = SDL_GetTicks() + 4000;
        kata_gtp_.stop();  // no more GTP commands needed
        begin_local_stone_removal(std::string(my_col_str) + "+R");
        return;
    }
    if (row == -1) {
        // KataGo passed
        flash_       = "KATAGO PASSED";
        flash_until_ = SDL_GetTicks() + 3000;
        apply_pass();
        if (local_prev_was_pass_) {
            begin_local_stone_removal();
            return;
        }
        local_prev_was_pass_ = true;
        game_.my_turn = true;
        request_live_ownership();
        set_status(my_turn_status);
        draw();
        return;
    }
    // Normal move
    play_move_sound(/*mine=*/false, apply_move(col, row));
    local_prev_was_pass_ = false;
    pass_confirm_        = false;
    game_.my_turn        = true;
    request_live_ownership();
    set_status(my_turn_status);
    draw();
}

// Debounced request for a fresh live-territory query — every point during
// PLAYING where the board actually changes calls this; the main loop tick
// fires the query itself once things settle (see the live_own_query_after_
// handling there), so several rapid calls collapse into one query.
void App::request_live_ownership() {
    if (!show_live_ownership_ || state_ != AppState::PLAYING) return;
    live_own_query_after_ = SDL_GetTicks() + 500;
}

void App::begin_local_stone_removal(const std::string& forced_result) {
    state_ = AppState::STONE_REMOVAL;
    close_popup_menu();  // a GAME MENU popup is stale once scoring starts
    stone_removal_has_ogs_territory_ = false;
    stone_removal_fill_view_ = false;
    game_.history_pos = -1;
    memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
    memset(game_.ownership,   0, sizeof(game_.ownership));
    my_accept_sent_    = false;
    local_game_score_  = forced_result;  // empty = double-pass, non-empty = resignation

    if (forced_result.empty()) {
        // Double-pass: ask GTP for dead stones first; ownership query follows in the poll loop.
        if (kata_gtp_.running())
            kata_gtp_.request_final_status();
    } else {
        // Resignation: result already known, no dead stones.
        // Query territory directly so the user can see the board state.
        if (kata_for(game_.board_size).running())
            kata_for(game_.board_size).query_ownership(
                game_.board.board, game_.board_size, game_.dead_stones, 7.5f, 100);
    }
    set_status("ANALYZING...");
    draw();
}

// ── Persisted match/display settings ──────────────────────────────────────────
// Simple whitespace-separated "key value" lines — order-independent and
// forward/backward compatible (unknown keys ignored on load; keys missing from
// an older file just leave that field at its compiled-in default).

void App::load_settings() {
    FILE* f = fopen((exe_dir() + "settings.txt").c_str(), "r");
    if (!f) return;
    char key[64];
    int val;
    while (fscanf(f, "%63s %d", key, &val) == 2) {
        std::string k = key;
        if      (k == "size_9")        match_prefs_.sizes[0]   = val != 0;
        else if (k == "size_13")       match_prefs_.sizes[1]   = val != 0;
        else if (k == "size_19")       match_prefs_.sizes[2]   = val != 0;
        else if (k == "speed_fast")    match_prefs_.speeds[0]  = val != 0;
        else if (k == "speed_medium")  match_prefs_.speeds[1]  = val != 0;
        else if (k == "speed_slow")    match_prefs_.speeds[2]  = val != 0;
        // "katago_mode" may still be present in older config files — silently ignored.
        else if (k == "katago_str")    match_prefs_.katago_str  = val;
        else if (k == "katago_size")   match_prefs_.katago_size = std::max(0, std::min(2, val));
        else if (k == "show_coords")   show_coords_            = val != 0;
        else if (k == "analysis")      kata_analysis_enabled_  = val != 0;
        else if (k == "chain_mode")    chain_mode_             = val != 0;
        else if (k == "square_stones") square_stones_          = val != 0;
        else if (k == "octagon_stones") octagon_stones_        = val != 0;
        else if (k == "square_grid")   square_grid_            = val != 0;
        else if (k == "show_territory") show_live_ownership_   = val != 0;
        else if (k == "show_next_move") show_next_move_        = val != 0;
        else if (k == "tetris_captures") tetris_captures_      = val != 0;
        else if (k == "board_hue")     Palette::set_board_hue(val);
        else if (k == "board_sat")     Palette::set_board_sat(val);
        else if (k == "board_val")     Palette::set_board_val(val);
    }
    fclose(f);
    // A hand-edited file could tick both shapes; the menu can't. Square loses, to
    // match the renderer's own tie-break, so the file and the screen never disagree.
    if (octagon_stones_) square_stones_ = false;
}

void App::save_settings() {
    FILE* f = fopen((exe_dir() + "settings.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "size_9 %d\n",        match_prefs_.sizes[0]    ? 1 : 0);
    fprintf(f, "size_13 %d\n",       match_prefs_.sizes[1]    ? 1 : 0);
    fprintf(f, "size_19 %d\n",       match_prefs_.sizes[2]    ? 1 : 0);
    fprintf(f, "speed_fast %d\n",    match_prefs_.speeds[0]   ? 1 : 0);
    fprintf(f, "speed_medium %d\n",  match_prefs_.speeds[1]   ? 1 : 0);
    fprintf(f, "speed_slow %d\n",    match_prefs_.speeds[2]   ? 1 : 0);
    fprintf(f, "katago_str %d\n",    match_prefs_.katago_str);
    fprintf(f, "katago_size %d\n",   match_prefs_.katago_size);
    fprintf(f, "show_coords %d\n",   show_coords_             ? 1 : 0);
    fprintf(f, "analysis %d\n",      kata_analysis_enabled_   ? 1 : 0);
    fprintf(f, "chain_mode %d\n",    chain_mode_              ? 1 : 0);
    fprintf(f, "square_stones %d\n", square_stones_           ? 1 : 0);
    fprintf(f, "octagon_stones %d\n", octagon_stones_         ? 1 : 0);
    fprintf(f, "square_grid %d\n",   square_grid_             ? 1 : 0);
    fprintf(f, "show_territory %d\n", show_live_ownership_    ? 1 : 0);
    fprintf(f, "show_next_move %d\n", show_next_move_         ? 1 : 0);
    fprintf(f, "tetris_captures %d\n", tetris_captures_       ? 1 : 0);
    fprintf(f, "board_hue %d\n",     Palette::BOARD_HUE);
    fprintf(f, "board_sat %d\n",     Palette::BOARD_SAT);
    fprintf(f, "board_val %d\n",     Palette::BOARD_VAL);
    fclose(f);
}

// ── Adaptive strength bookkeeping ─────────────────────────────────────────────

void App::load_adaptive() {
    FILE* f = fopen((exe_dir() + "adaptive_level.txt").c_str(), "r");
    if (!f) return;
    float v = 0.f;
    if (fscanf(f, "%f", &v) == 1 && v >= 0.f && v <= (float)KATA_RANK_MAX)
        adaptive_rank_ = v;
    fclose(f);
}

void App::save_adaptive() {
    FILE* f = fopen((exe_dir() + "adaptive_level.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "%.3f\n", adaptive_rank_);
    fclose(f);
}

// Nudge the adaptive rating after a finished adaptive game. The player is always
// Black in local games, so "B+..." is a win. Step size scales with the score
// margin — a 40-point blowout moves a full rank, a nail-biter barely moves —
// with resignations counted as a solid but not extreme result.
void App::update_adaptive(const std::string& result) {
    if (result.size() < 3 || (result[0] != 'B' && result[0] != 'W')) return;
    bool won = (result[0] == 'B');

    float step;
    std::string margin = result.substr(2);
    if (!margin.empty() && (margin[0] == 'R' || margin[0] == 'T')) {
        step = 0.6f;
    } else {
        float pts = (float)atof(margin.c_str());
        step = std::max(0.15f, std::min(1.0f, pts / 25.0f));
    }

    adaptive_rank_ += won ? step : -step;
    adaptive_rank_  = std::max(0.f, std::min((float)KATA_RANK_MAX, adaptive_rank_));
    save_adaptive();

    int idx = std::max(0, std::min(KATA_RANK_MAX, (int)std::lround(adaptive_rank_)));
    flash_       = std::string("ADAPTIVE LEVEL: ") + kata_rank_label(idx)
                 + (won ? " (UP)" : " (DOWN)");
    flash_until_ = SDL_GetTicks() + 3500;
}

void App::end_local_game(const std::string& result) {
    kata_gtp_.stop();
    if (adaptive_game_) {
        update_adaptive(result);
        adaptive_game_ = false;
    }
    is_local_game_        = false;
    // Restart the background sweep from move 0 so any depths that missed scoring
    // during play (lost responses, mid-game undos) get filled in during review.
    bg_analysis_next_ = 0;
    close_popup_menu();  // popup items built for PLAYING/STONE_REMOVAL are stale now
    review_komi_           = local_game_komi_;  // 7.5 for fresh games; inherited for practice games
    state_ = AppState::GAME_OVER;
    game_.result = result;
    save_live_game();
    // Scan for auto study puzzles (remaining depths get checked as the background
    // sweep fills them in during review)
    for (int d = 0; d + 1 < (int)game_.history.size(); d++) check_puzzle(d);
    // Straight into analysis — the score was already shown big on the stone-removal
    // screen, so a second result-confirmation screen here was redundant.
    build_analysis_tree();
    kata_suggestion_count_ = 0;
    kata_score_lead_       = cached_analysis_score(analysis_cur_);
    kata_analysis_enabled_ = false;
    set_status("GAME OVER — " + result);
    draw();
}

// ── Game catalog ──────────────────────────────────────────────────────────────

void App::open_game_catalog() {
    if (catalog_.active) return;
    save_companion();  // persist any in-progress review scores/marks before switching away

    // Locate my_games/<username>/ — personal games, separate from the games/ pro
    // library (same two-path probe as demo loader)
    std::string gdir = exe_dir() + "my_games";
    if (!is_dir(gdir)) gdir = exe_dir() + "../../my_games";

    std::string my_dir = Catalog::join_path(gdir, my_username_);

    // Reopening the catalog resumes where it was last time (same subdirectory —
    // e.g. puzzles/ or marked/ — and roughly the same cursor spot) instead of
    // resetting to the root on every visit. First open of the session still
    // starts fresh at the root.
    bool resume = catalog_.base_dir == my_dir;
    int  old_index  = catalog_.index;
    int  old_scroll = catalog_.scroll;
    if (!resume) {
        catalog_.open(my_dir);   // full reset (also kicks off the search index)
        catalog_.current_subdir = "";
    } else {
        catalog_.selection_made = false;
        catalog_.selected_path.clear();
        catalog_.search_query.clear();
        catalog_.search_mode = false;
        catalog_.active      = true;
    }
    // Flat filesystem mode (skip the virtual player browser)
    catalog_.virtual_player_mode = false;
    catalog_.virtual_year_mode   = false;
    catalog_.load_entries();     // refresh — new games/puzzles may have appeared

    int n = (int)catalog_.entries.size();
    catalog_.index  = 0;
    catalog_.scroll = 0;
    if (resume && n > 0) {
        catalog_.index  = std::min(old_index, n - 1);
        catalog_.scroll = old_scroll;
        // If a review is open and it lives in the displayed directory, park the
        // cursor on it — after L3/R3 cycling this lands on the current file.
        if (!review_path_.empty()) {
            size_t sep = review_path_.find_last_of("/\\");
            std::string rdir  = (sep == std::string::npos) ? "" : review_path_.substr(0, sep);
            std::string rname = (sep == std::string::npos) ? review_path_ : review_path_.substr(sep + 1);
            std::string shown = catalog_.current_subdir.empty()
                                ? catalog_.base_dir
                                : Catalog::join_path(catalog_.base_dir, catalog_.current_subdir);
            if (rdir == shown)
                for (int i = 0; i < n; i++)
                    if (catalog_.entries[i].type == 0 && catalog_.entries[i].name == rname) {
                        catalog_.index = i;
                        break;
                    }
        }
        // Keep the cursor row within the visible window (15 rows)
        catalog_.scroll = std::min(catalog_.scroll, catalog_.index);
        if (catalog_.index >= catalog_.scroll + 15)
            catalog_.scroll = catalog_.index - 14;
    }
    catalog_delete_confirm_ = false;
    catalog_readonly_       = false;   // the user's own games are always deletable
    // Parse player names for every entry up front, not just the first visible page —
    // each parse only reads a 4KB SGF header, so even a few hundred games open fast,
    // and the whole list shows "Black vs White" immediately instead of raw filenames
    // trickling in as rows scroll into view.
    catalog_.ensure_names_loaded(0, (int)catalog_.entries.size());
    thumb_path_ = "";                     // force thumbnail reload on first draw
    update_catalog_thumb();
}

// Curated professional-game library — games/ itself (one subdirectory per pro
// player). The user's own games live separately under my_games/<username>/
// (see open_game_catalog()), so this tree is pure pro content. Browsed with
// virtual player/year views. Read-only: see catalog_readonly_.
void App::open_pro_catalog() {
    if (catalog_.active) return;
    save_companion();  // persist any in-progress review scores/marks before switching away

    // Locate games/ — same two-path probe as open_game_catalog()/demo loader
    std::string gdir = exe_dir() + "games";
    if (!is_dir(gdir)) gdir = exe_dir() + "../../games";

    // The pro database never changes mid-session, so unlike open_game_catalog()
    // there is nothing to refresh on resume — just leave `entries` exactly as
    // they were (whatever player/year/directory view was open) and reactivate.
    bool resume = catalog_.base_dir == gdir;
    if (!resume) {
        catalog_.open(gdir);   // full reset — lands on BY PLAYER (Catalog::open()'s default)
    } else {
        catalog_.selection_made = false;
        catalog_.selected_path.clear();
        catalog_.search_query.clear();
        catalog_.search_mode = false;
        catalog_.active      = true;
    }
    catalog_readonly_       = true;
    catalog_delete_confirm_ = false;
    catalog_.ensure_names_loaded(0, (int)catalog_.entries.size());
    thumb_path_ = "";
    update_catalog_thumb();
}

void App::delete_catalog_game(const std::string& sgf_path) {
    if (sgf_path.empty()) return;
    if (!remove_path(sgf_path)) {
        flash_       = "DELETE FAILED";
        flash_until_ = SDL_GetTicks() + 2000;
        return;
    }
    // Companion score/mark file goes with it (fails silently if there isn't one)
    size_t dot = sgf_path.rfind('.');
    if (dot != std::string::npos) {
        std::string companion = sgf_path.substr(0, dot) + ".katago";
        remove_path(companion);
        // If the deleted game is the one currently loaded behind the catalog,
        // forget its companion path so exiting review can't resurrect the file.
        if (companion == companion_path_) companion_path_.clear();
    }
    flash_       = "GAME DELETED";
    flash_until_ = SDL_GetTicks() + 1500;

    // Refresh the listing in place, keeping the cursor near where it was
    int old_index = catalog_.index;
    catalog_.load_entries();
    int n = (int)catalog_.entries.size();
    catalog_.index  = (n == 0) ? 0 : std::min(old_index, n - 1);
    catalog_.scroll = std::min(catalog_.scroll, std::max(0, catalog_.index));
    catalog_.ensure_names_loaded(0, n);
    thumb_path_ = "";
    update_catalog_thumb();
}

void App::open_settings_menu() {
    pre_menu_state_          = state_;
    match_menu_.ingame       = (state_ != AppState::LOBBY);
    match_menu_.focus_col    = 0;
    match_menu_.focus_row    = 0;
    // This is the OGS/match screen. open_katago_settings() flips it after calling
    // us — never restored from prefs, or a previous visit to KATAGO SETTINGS would
    // make MATCH SETTINGS come back up showing the KataGo columns.
    match_menu_.katago_mode  = false;
    match_menu_.katago_str   = match_prefs_.katago_str;
    match_menu_.katago_size  = match_prefs_.katago_size;
    {
        int idx = std::max(0, std::min(KATA_RANK_MAX, (int)std::lround(adaptive_rank_)));
        match_menu_.adaptive_label = "ADAPTIVE (" + kata_rank_label(idx) + ")";
    }
    for (int i = 0; i < 3; i++) match_menu_.size_sel[i]  = match_prefs_.sizes[i];
    for (int i = 0; i < 3; i++) match_menu_.speed_sel[i] = match_prefs_.speeds[i];
    match_menu_.show_coords_sel = show_coords_;
    match_menu_.analysis_sel    = kata_analysis_enabled_;
    match_menu_.analysis_available = kata_.running() || kata_9_.running();
    match_menu_.chain_sel       = chain_mode_;
    match_menu_.square_sel      = square_stones_;
    match_menu_.octagon_sel     = octagon_stones_;
    match_menu_.square_grid_sel = square_grid_;
    match_menu_.territory_sel       = show_live_ownership_;
    match_menu_.territory_available = kata_.running() || kata_9_.running();
    match_menu_.next_move_sel   = show_next_move_;
    match_menu_.tetris_sel      = tetris_captures_;
    state_ = AppState::MATCH_MENU;
    renderer_->draw_match_menu(match_menu_);
}

// KATAGO SETTINGS — the same menu screen showing the local-play settings (board
// size, engine strength) instead of the OGS ones. Its own popup entry rather than
// a mode toggle inside MATCH SETTINGS: that toggle was an L1-only hotkey on a row
// that sat outside the column grid, so it couldn't be clicked and didn't announce
// itself. Which game you start is already chosen explicitly in the popup
// (FIND MATCH vs PLAY VS KATAGO), so the mode never needed to be a setting.
void App::toggle_draw_mode() {
    draw_mode_ = !draw_mode_;
    pen_down_  = false;
    // The chalk crosshair replaces the OS pointer while drawing; hand the pointer
    // back on the way out (but only if the mouse was the active input, so a pad
    // session doesn't suddenly sprout a cursor).
    if (draw_mode_) {
        SDL_ShowCursor(SDL_DISABLE);
    } else if (cursor_shown_) {
        SDL_ShowCursor(SDL_ENABLE);
    }
    flash_       = draw_mode_ ? "DRAWING MODE" : "DRAWING MODE OFF";
    flash_until_ = SDL_GetTicks() + 1500;
    draw();
}

void App::open_katago_settings() {
    open_settings_menu();               // shared population of every field
    match_menu_.ingame      = false;    // always a lobby-level screen
    match_menu_.katago_mode = true;
    match_menu_.focus_col   = 0;
    match_menu_.focus_row   = 0;
    renderer_->draw_match_menu(match_menu_);
}

// Triangle from LOBBY: fresh empty 19x19 board in the standard analysis mode —
// same tree panel, branching, labels, and engine toggle as a game review, just
// with nothing played yet.
void App::start_free_analysis() {
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = 19;
    game_.board.reset();
    game_.board.board_size    = 19;
    game_.board.turn_is_black = 1;
    game_.cursor_r = game_.cursor_f = 9;
    game_.black_name = "BLACK";
    game_.white_name = "WHITE";
    game_.black_rank = game_.white_rank = "";
    game_.result.clear();
    game_.my_color   = -1;   // pure analysis — no "my side"
    game_.game_id    = 0;
    game_.black_secs = game_.white_secs = -1;
    game_.history.push_back(game_.board);

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;
    review_komi_ = 7.5f;

    move_scores_.assign(game_.history.size(), FLT_MAX);
    move_marked_.assign(game_.history.size(), false);
    marked_paths_.assign(game_.history.size(), "");
    puzzle_eval_.clear();
    puzzle_saved_.clear();
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    companion_path_.clear();   // nothing on disk to persist scores/marks to
    review_path_.clear();      // L3/R3 file cycling doesn't apply here
    is_local_game_ = false;

    build_analysis_tree();
    state_ = AppState::GAME_OVER;
    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_analysis_enabled_ = false;  // toggle on via the settings menu as usual
    set_status("FREE ANALYSIS");
    draw();
}

// ── OGS puzzle browser / player ───────────────────────────────────────────────

void App::load_solved_puzzles() {
    FILE* f = fopen((exe_dir() + "solved_puzzles.txt").c_str(), "r");
    if (!f) return;
    // One puzzle per line: "id" (legacy) or "id collection_id". Parse line-wise —
    // a bare %d-%d scan would pair up ids across lines in a legacy file.
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int id = 0, col = 0;
        int n = sscanf(line, "%d %d", &id, &col);
        if (n >= 1 && id > 0) {
            pz_solved_ids_.insert(id);
            pz_solved_col_[id] = (n >= 2 && col > 0) ? col : 0;
        }
    }
    fclose(f);
}

void App::save_solved_puzzles() {
    FILE* f = fopen((exe_dir() + "solved_puzzles.txt").c_str(), "w");
    if (!f) return;
    for (int id : pz_solved_ids_) {
        auto it = pz_solved_col_.find(id);
        fprintf(f, "%d %d\n", id, it != pz_solved_col_.end() ? it->second : 0);
    }
    fclose(f);
}

// puzzle_collections.txt: one collection per line, tab-separated —
// id, starting_puzzle_id, puzzle_count, min_rank, max_rank, rating, owner, name.
// Name is last so it may contain anything except a tab.
void App::load_known_collections() {
    FILE* f = fopen((exe_dir() + "puzzle_collections.txt").c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        std::vector<std::string> parts;
        size_t pos = 0;
        for (int i = 0; i < 7; i++) {
            size_t tab = s.find('\t', pos);
            if (tab == std::string::npos) break;
            parts.push_back(s.substr(pos, tab - pos));
            pos = tab + 1;
        }
        if (parts.size() != 7) continue;   // malformed line
        OgsPuzzleCollection c;
        c.id                 = atoi(parts[0].c_str());
        c.starting_puzzle_id = atoi(parts[1].c_str());
        c.puzzle_count       = atoi(parts[2].c_str());
        c.min_rank           = atoi(parts[3].c_str());
        c.max_rank           = atoi(parts[4].c_str());
        c.rating             = (float)atof(parts[5].c_str());
        c.owner              = parts[6];
        c.name               = s.substr(pos);
        if (c.id > 0) pz_known_cols_[c.id] = c;
    }
    fclose(f);
}

void App::save_known_collections() {
    FILE* f = fopen((exe_dir() + "puzzle_collections.txt").c_str(), "w");
    if (!f) return;
    for (const auto& kv : pz_known_cols_) {
        const OgsPuzzleCollection& c = kv.second;
        fprintf(f, "%d\t%d\t%d\t%d\t%d\t%.2f\t%s\t%s\n",
                c.id, c.starting_puzzle_id, c.puzzle_count,
                c.min_rank, c.max_rank, c.rating,
                c.owner.c_str(), c.name.c_str());
    }
    fclose(f);
}

void App::pz_rebuild_display() {
    pz_display_cols_.clear();
    std::map<int, int> solved = pz_solved_per_collection();

    // Pinned section: every known collection with progress but not finished
    std::vector<const OgsPuzzleCollection*> pinned;
    for (const auto& kv : pz_known_cols_) {
        auto it = solved.find(kv.first);
        int s = (it == solved.end()) ? 0 : it->second;
        if (s > 0 && (kv.second.puzzle_count <= 0 || s < kv.second.puzzle_count))
            pinned.push_back(&kv.second);
    }
    std::sort(pinned.begin(), pinned.end(),
              [](const OgsPuzzleCollection* a, const OgsPuzzleCollection* b) {
                  return str_icmp(a->name, b->name) < 0;
              });
    std::set<int> pinned_ids;
    for (const OgsPuzzleCollection* p : pinned) {
        pz_display_cols_.push_back(*p);
        pinned_ids.insert(p->id);
    }

    // Then the fetched page, minus collections already pinned above
    for (const auto& c : pz_collections_)
        if (!pinned_ids.count(c.id))
            pz_display_cols_.push_back(c);

    // Local puzzle sets, pinned at the very top (negative sentinel ids, routed
    // to open_local_pz_list() instead of a network fetch). Each only shown
    // when at least one file exists in its folder.
    std::vector<OgsPuzzleCollection> local_pins;
    auto add_local_pin = [&](int id, const char* label, const std::string& dir) {
        std::vector<std::string> files;
        if (Catalog::list_sgf_files(dir, files) && !files.empty()) {
            OgsPuzzleCollection c;
            c.id           = id;
            c.name         = label;
            c.owner        = my_username_.empty() ? "You" : my_username_;
            c.puzzle_count = (int)files.size();
            local_pins.push_back(c);
        }
    };
    add_local_pin(-1, "[MY DRILLS]",        drills_dir(false));
    add_local_pin(-2, "[MISSED PUZZLES]",   missed_puzzles_dir(false));
    add_local_pin(-3, "[FAVORITE PUZZLES]", favorite_puzzles_dir(false));

    // Imported problem books, below the personal sets. Their rows carry the
    // routing pin id; the real collection id (for progress colouring) is
    // looked back up through local_books_ — see draw_puzzle_browser.
    scan_local_books();

    // goproblems download — indexed once per folder (tools/goproblems_index.py)
    if (gp_dir_ != gp_index_dir_) {
        gp_index_dir_ = gp_dir_;
        gp_index_.clear();
        if (!gp_dir_.empty())
            gp_load_index(Catalog::join_path(gp_dir_, "goproblems_index.tsv"), gp_index_);
    }
    if (!gp_index_.empty()) {
        OgsPuzzleCollection c;
        c.id           = -4;
        c.name         = "[GOPROBLEMS.COM — EASIEST FIRST]";
        c.owner        = "GOPROBLEMS.COM";
        c.puzzle_count = (int)gp_index_.size();
        c.min_rank     = std::max(1, gp_index_.front().level);   // 0 (30k) reads as "?"
        c.max_rank     = gp_index_.back().level;
        local_pins.push_back(c);
    }

    for (const auto& b : local_books_)
        add_local_pin(b.pin_id, ("[" + b.name + "]").c_str(), b.dir);
    pz_display_cols_.insert(pz_display_cols_.begin(), local_pins.begin(), local_pins.end());
}

// Enter a local puzzle list (drills / missed / favorites): the puzzle
// browser's PUZZLES view backed by local files instead of a fetched
// collection.
void App::open_local_pz_list(LocalPzKind kind) {
    std::string dir, title, empty_msg;
    switch (kind) {
    case LocalPzKind::MISSED:
        dir = missed_puzzles_dir(false);
        title = "MISSED PUZZLES";
        empty_msg = "NO MISSED PUZZLES SAVED YET";
        break;
    case LocalPzKind::FAVORITES:
        dir = favorite_puzzles_dir(false);
        title = "FAVORITE PUZZLES";
        empty_msg = "NO FAVORITES SAVED YET";
        break;
    case LocalPzKind::BOOK:
        dir = local_book_.dir;            // set by the caller that picked the book
        title = local_book_.name;
        empty_msg = "NO PROBLEMS IN THIS SET";
        break;
    default:
        dir = drills_dir(false);
        title = "MY DRILLS";
        empty_msg = "NO DRILLS YET — SAVE ONE FROM ANALYSIS";
        break;
    }
    std::vector<std::string> files;
    Catalog::list_sgf_files(dir, files);
    if (files.empty()) {
        flash_       = empty_msg;
        flash_until_ = SDL_GetTicks() + 2500;
        draw();
        return;
    }
    std::sort(files.begin(), files.end(),
              [](const std::string& a, const std::string& b) {
                  return str_icmp(a, b) < 0;
              });
    drill_paths_.clear();
    pz_list_.clear();
    for (int i = 0; i < (int)files.size(); i++) {
        drill_paths_.push_back(Catalog::join_path(dir, files[i]));
        std::string stem = files[i];
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        // Filenames store spaces as underscores (sgf_sanitize) — show them
        // as the spaces the user actually typed. Rename's prefill picks up
        // spaces too; sanitizing maps them back on commit, so it round-trips.
        for (char& c : stem)
            if (c == '_') c = ' ';
        // The id is a throwaway sentinel for personal sets (nothing reads it —
        // opening and stepping both go by index). Book problems carry a real
        // ZI[] though, so read it back and the list can mark solved ones the
        // same way an OGS collection does.
        int id = (kind == LocalPzKind::BOOK) ? sgf_peek_int(drill_paths_.back(), "ZI")
                                             : -(i + 1);
        pz_list_.push_back({id ? id : -(i + 1), stem});
    }
    local_pz_kind_   = kind;
    pz_view_         = PzView::PUZZLES;
    pz_list_title_   = title;
    // Books have real solved tracking — resume at the last solved problem.
    // Personal sets carry sentinel ids, so they stay at the top.
    pz_index_        = (kind == LocalPzKind::BOOK) ? pz_last_solved_index() : 0;
    pz_open_col_id_  = (kind == LocalPzKind::BOOK) ? local_book_.collection_id : 0;
    drill_thumb_idx_ = -1;   // file list may have changed — recompute the preview
    draw();
}

// Load drill_paths_[idx] into pz_ and start it — the local (no network)
// counterpart of pz_launch_fetch(2, id). Which loader runs depends on
// local_pz_kind_: drills always get id=0, missed/favorites keep their real
// OGS identity (see load_drill_sgf / load_saved_puzzle_sgf).
void App::local_pz_load_and_start(int idx) {
    if (idx < 0 || idx >= (int)drill_paths_.size()) return;
    std::string err;
    bool ok = (local_pz_kind_ == LocalPzKind::DRILLS)
                  ? load_drill_sgf(drill_paths_[idx], pz_, err)
                  : load_saved_puzzle_sgf(drill_paths_[idx], pz_, err);
    if (!ok) {
        flash_       = "BAD FILE: " + err;
        flash_until_ = SDL_GetTicks() + 2500;
        draw();
        return;
    }
    pz_visits_.clear();
    pz_more_lines_   = false;
    pz_list_pos_     = idx;
    drill_play_path_ = drill_paths_[idx];
    pz_start();
}

// Commit the keyboard-entered new name for the selected drill file.
void App::drill_commit_rename() {
    drill_rename_active_ = false;
    std::string name = sgf_sanitize(drill_rename_buf_);
    drill_rename_buf_.clear();
    if (pz_index_ < 0 || pz_index_ >= (int)drill_paths_.size()) { draw(); return; }
    if (name.empty()) {
        flash_       = "INVALID NAME";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    std::string old_path = drill_paths_[pz_index_];
    std::string new_path = Catalog::join_path(current_local_pz_dir(false), name + ".sgf");
    if (new_path == old_path) { draw(); return; }
    if (path_exists(new_path)) {
        flash_       = "NAME ALREADY TAKEN";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    if (!rename_path(old_path, new_path)) {
        flash_       = "RENAME FAILED";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    if (drill_play_path_ == old_path) drill_play_path_ = new_path;
    LocalPzKind kind = local_pz_kind_;
    open_local_pz_list(kind);   // re-list; park the cursor back on the renamed file
    for (int i = 0; i < (int)drill_paths_.size(); i++)
        if (drill_paths_[i] == new_path) { pz_index_ = i; break; }
    flash_       = "RENAMED";
    flash_until_ = SDL_GetTicks() + 1500;
    draw();
}

// Delete the selected local file (reached via the popup's confirmed item).
void App::drill_delete_selected() {
    if (pz_index_ < 0 || pz_index_ >= (int)drill_paths_.size()) return;
    std::string path = drill_paths_[pz_index_];
    if (!remove_path(path)) {
        flash_       = "DELETE FAILED";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    if (drill_play_path_ == path) drill_play_path_.clear();
    flash_       = "DELETED";
    flash_until_ = SDL_GetTicks() + 1500;
    int old_index = pz_index_;
    LocalPzKind kind = local_pz_kind_;
    std::vector<std::string> remaining;
    Catalog::list_sgf_files(current_local_pz_dir(false), remaining);
    if (remaining.empty()) {
        // Last file gone — this pinned collection itself disappears
        local_pz_kind_ = LocalPzKind::NONE;
        drill_paths_.clear();
        pz_view_  = PzView::COLLECTIONS;
        pz_index_ = 0;
        pz_rebuild_display();
        draw();
        return;
    }
    open_local_pz_list(kind);
    pz_index_ = std::min(old_index, (int)drill_paths_.size() - 1);
    draw();
}

// Reopen the drill being played as an editable analysis tree: the drill's
// setup becomes the analysis root, its variation tree becomes AnalysisNode
// children (correct-marks and labels intact), and SAVE overwrites the file.
void App::drill_edit_current() {
    if (local_pz_kind_ != LocalPzKind::DRILLS || drill_play_path_.empty()) return;
    // Reload from disk: the in-memory pz_ may carry a random drill orientation
    // (see pz_start) — editing must happen in the file's authored orientation
    // or every save would rewrite the drill rotated.
    std::string reload_err;
    if (!load_drill_sgf(drill_play_path_, pz_, reload_err)) {
        flash_       = "CAN'T RELOAD DRILL: " + reload_err;
        flash_until_ = SDL_GetTicks() + 2500;
        draw();
        return;
    }

    // Rebuild game_ at the drill's root, same steps as pz_start()
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = pz_.width;
    game_.board.reset();
    game_.board.board_size    = pz_.width;
    game_.board.turn_is_black = pz_.black_to_play ? 1 : 0;
    game_.black_name = "BLACK";
    game_.white_name = "WHITE";
    game_.black_rank = game_.white_rank = "";
    game_.result.clear();
    game_.my_color   = -1;   // analysis — no "my side", no study-puzzle detection
    game_.game_id    = 0;
    game_.black_secs = game_.white_secs = -1;
    game_.cursor_r = std::max(0, std::min(pz_.width - 1, game_.cursor_r));
    game_.cursor_f = std::max(0, std::min(pz_.width - 1, game_.cursor_f));
    auto apply_stones = [&](const std::string& coords, char stone) {
        for (size_t i = 0; i + 1 < coords.size(); i += 2) {
            int f = coords[i]     - 'a';
            int r = coords[i + 1] - 'a';
            if (r >= 0 && r < pz_.width && f >= 0 && f < pz_.width)
                game_.board.board[r][f] = stone;
        }
    };
    apply_stones(pz_.initial_black, 1);
    apply_stones(pz_.initial_white, 2);
    game_.board.save_snapshot();
    game_.history.push_back(game_.board);
    last_move_r_ = last_move_f_ = -1;

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;
    review_komi_ = 7.5f;
    move_scores_.assign(1, FLT_MAX);
    move_marked_.assign(1, false);
    marked_paths_.assign(1, "");
    puzzle_eval_.clear();
    puzzle_saved_.clear();
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    companion_path_.clear();
    review_path_.clear();
    is_local_game_ = false;

    // Root from the 1-entry history (also clears drill_edit_path_ — reset below)
    build_analysis_tree();
    // Preserve the file's solver pin across edits — but only an explicit one.
    // A plain drill (solver == mover) stays "unset" so SWITCH SIDE TO MOVE keeps
    // dragging the solver along with it, same as before this decoupling existed.
    drill_solver_black_ = (pz_.solver_black == pz_.black_to_play)
                               ? -1 : (pz_.solver_black ? 1 : 0);

    // Convert the puzzle tree into analysis children. Every node is heap-owned
    // (unique_ptr chain) and boards are copied member-to-member inside those
    // heap blocks — no GameState ever touches the stack (see the CRITICAL
    // FOOTGUN note: sizeof(GameState) is ~12MB).
    std::function<void(const PuzzleMoveNode&, AnalysisNode*, int)> conv =
        [&](const PuzzleMoveNode& src, AnalysisNode* parent, int color) {
            for (const auto& b : src.branches) {
                if (b.x < 0 || b.y < 0) continue;
                auto ch = std::make_unique<AnalysisNode>();
                ch->board = parent->board;
                if (!ch->board.place_stone(b.y, b.x, color)) continue;  // illegal in file — drop
                ch->board.turn_is_black = color ? 0 : 1;
                ch->move_col      = b.x;
                ch->move_row      = b.y;
                ch->move_color    = color;
                ch->depth         = parent->depth + 1;
                ch->parent        = parent;
                ch->is_main_line  = false;
                ch->drill_correct = b.correct;
                for (const auto& m : b.marks)
                    ch->labels.push_back({m.y, m.x, m.ch});
                parent->children.push_back(std::move(ch));
                conv(b, parent->children.back().get(), 1 - color);
            }
        };
    conv(pz_.tree, analysis_root_.get(), pz_.black_to_play ? 1 : 0);

    analysis_cur_ = analysis_root_.get();
    build_analysis_tree_render();
    drill_edit_path_ = drill_play_path_;   // SAVE becomes OVERWRITE DRILL

    state_ = AppState::GAME_OVER;
    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_analysis_enabled_ = false;
    set_status("EDITING DRILL — SAVE VIA OPTIONS MENU");
    draw();
}

// Auto-save a wrong attempt at a real (network-fetched) OGS puzzle for later
// review. Deduplicated by id — a puzzle only ever gets saved once, no matter
// how many times it's missed again. Returns true only when newly written, so
// the caller can flash a one-time confirmation instead of every retry.
bool App::save_missed_puzzle() {
    if (pz_.id <= 0) return false;
    std::string path = Catalog::join_path(missed_puzzles_dir(true),
                                           "puzzle-" + std::to_string(pz_.id) + ".sgf");
    if (path_exists(path))
        return false;   // already saved from an earlier miss
    return write_puzzle_copy_sgf(path, pz_);
}

// Manually toggle whether the current (real) puzzle is in favorites.
void App::toggle_favorite_puzzle() {
    if (pz_.id <= 0) return;
    std::string path = Catalog::join_path(favorite_puzzles_dir(true),
                                           "puzzle-" + std::to_string(pz_.id) + ".sgf");
    bool exists = path_exists(path);
    if (exists) {
        remove_path(path);
        flash_ = "REMOVED FROM FAVORITES";
    } else if (write_puzzle_copy_sgf(path, pz_)) {
        flash_ = "ADDED TO FAVORITES";
    } else {
        flash_ = "COULDN'T SAVE FAVORITE";
    }
    flash_until_ = SDL_GetTicks() + 1800;
    draw();
}

// ── Joseki explorer ───────────────────────────────────────────────────────────

// OJE "pretty" coordinate ("Q16") → our (row, col). Columns skip 'I'; row 1 is
// the bottom edge. Returns false for "pass"/"root"/anything unparseable.
static bool jk_parse_placement(const std::string& p, int bs, int& r, int& f) {
    if (p.size() < 2) return false;
    char c = (char)toupper((unsigned char)p[0]);
    if (c < 'A' || c > 'T' || c == 'I') return false;
    f = c - 'A' - (c > 'I' ? 1 : 0);
    int num = atoi(p.c_str() + 1);
    if (num < 1 || num > bs || f < 0 || f >= bs) return false;
    r = bs - num;
    return true;
}

// OJE move-quality category → marker color (matches the website's palette)
static SDL_Color jk_category_color(const std::string& cat) {
    if (cat == "IDEAL")    return {0,   195, 40,  235};
    if (cat == "GOOD")     return {150, 190, 0,   235};
    if (cat == "MISTAKE")  return {210, 30,  45,  235};
    if (cat == "TRICK")    return {255, 215, 0,   235};
    if (cat == "QUESTION") return {0,   180, 220, 235};
    return {160, 160, 160, 235};
}

// Strip the markdown-isms OJE descriptions carry (headings, emphasis, code
// ticks) so the plain-text comment box doesn't render them as noise.
static std::string jk_scrub_markdown(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '#' || c == '*' || c == '`' || c == '\r') continue;
        out += c;
    }
    return out;
}

void App::open_joseki_explorer() {
    state_ = AppState::JOSEKI;
    jk_path_.clear();
    jk_boards_.clear();
    jk_markers_.clear();
    jk_comment_.clear();
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = 19;
    game_.board.reset();
    game_.board.board_size    = 19;
    game_.board.turn_is_black = 1;
    game_.my_color   = -1;   // explorer — no "my side"
    game_.my_turn    = true; // full-brightness cursor
    game_.black_secs = game_.white_secs = -1;
    // Start the cursor on Q16 — the database's canonical corner
    game_.cursor_r = 3;
    game_.cursor_f = 15;
    set_status("JOSEKI — LOADING...");
    jk_fetch_node("root");
    draw();
}

void App::jk_fetch_node(const std::string& node_id) {
    auto it = jk_cache_.find(node_id);
    if (it != jk_cache_.end()) {
        jk_arrive(it->second);
        return;
    }
    if (jk_loading_) return;   // one in-flight fetch at a time
    jk_loading_ = true;
    auto res = std::make_shared<JosekiFetch>();
    res->node_id = node_id;
    jk_fetch_ = res;
    std::thread([res] {
        res->ok = ogs_fetch_joseki(res->node_id, res->pos);
        res->ready.store(true);
        SDL_Event ev{};
        ev.type = g_net_event_type;   // wake the event loop
        SDL_PushEvent(&ev);
    }).detach();
}

void App::poll_joseki_fetch() {
    if (!jk_fetch_ || !jk_fetch_->ready.load()) return;
    auto res = jk_fetch_;
    jk_fetch_.reset();
    jk_loading_ = false;
    if (state_ != AppState::JOSEKI) return;   // user left while it was in flight
    if (!res->ok) {
        flash_       = "OJE FETCH FAILED";
        flash_until_ = SDL_GetTicks() + 2500;
        if (jk_path_.empty()) set_status("JOSEKI — OFFLINE?");
        draw();
        return;
    }
    jk_cache_[res->node_id] = res->pos;
    jk_arrive(res->pos);
}

// Land on a node: apply its placement to the current board (with captures —
// trick lines do capture), snapshot, and refresh the overlay.
//
// GameState is ~12MB (GameState::history embeds a full GameSnapshot[MAX_MOVES]
// by value, not a vector) — it must NEVER be declared as a local/stack variable
// or by-value parameter; the very first version of this function did exactly
// that (`GameState b;`) and stack-overflowed on the spot (confirmed via the
// crash's fault address landing inside ___chkstk_ms). Everything below works
// through a reference into jk_boards_ instead, which is heap-backed.
void App::jk_arrive(const JosekiPosition& pos) {
    if (jk_boards_.empty()) {
        jk_boards_.emplace_back();
        GameState& b0 = jk_boards_.back();
        b0.reset();
        b0.board_size    = 19;
        b0.turn_is_black = 1;
    } else {
        // Two-step (emplace fresh, then assign) rather than push_back(back()):
        // avoids relying on push_back's self-referencing-argument guarantee for
        // a type this size, at the cost of one harmless extra default-construct.
        jk_boards_.emplace_back();
        jk_boards_.back() = jk_boards_[jk_boards_.size() - 2];
    }
    GameState& b = jk_boards_.back();

    if (!jk_path_.empty()) {   // not root — this node is a move on the parent board
        int r, f;
        if (jk_parse_placement(pos.placement, 19, r, f)) {
            int color = b.turn_is_black ? 1 : 0;
            b.board[r][f] = b.turn_is_black ? 1 : 2;
            int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
            int cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
            int cap_count = 0;
            GoRules::find_captured(b.board, color, r, f, cap_r, cap_f, cap_count, 19);
            for (int i = 0; i < cap_count; i++) b.board[cap_r[i]][cap_f[i]] = 0;
            b.turn_is_black = !b.turn_is_black;
            game_.cursor_r = r;   // cursor follows the line being walked
            game_.cursor_f = f;
        } else if (pos.placement == "pass") {
            b.turn_is_black = !b.turn_is_black;
        }
    }
    jk_path_.push_back(pos);
    game_.board = b;   // safe: copies INTO an existing member, not a new stack frame
    jk_show_current();
}

void App::jk_show_current() {
    const JosekiPosition& cur = jk_path_.back();

    jk_markers_.clear();
    for (const auto& m : cur.next_moves) {
        int r, f;
        if (!jk_parse_placement(m.placement, 19, r, f)) continue;   // skips "pass"
        if (game_.board.board[r][f] != 0) continue;
        jk_markers_.push_back({r, f, jk_category_color(m.category)});
    }

    jk_comment_ = jk_scrub_markdown(cur.description);
    if (!cur.source_desc.empty())
        jk_comment_ += "\n\nSOURCE: " + cur.source_desc;

    int depth = (int)jk_path_.size() - 1;
    std::string st = "JOSEKI — ";
    st += (depth == 0) ? "EMPTY BOARD" : "MOVE " + std::to_string(depth)
                                          + "  [" + cur.placement + "]";
    if (!cur.category.empty() && cur.category != "IDEAL" && depth > 0)
        st += "  " + cur.category;
    st += "  —  " + std::to_string((int)cur.next_moves.size()) + " LINES";
    set_status(st);
    draw();
}

void App::jk_step_back() {
    if (jk_path_.size() <= 1) return;
    jk_path_.pop_back();
    jk_boards_.pop_back();
    game_.board = jk_boards_.back();
    jk_show_current();
}

void App::handle_joseki_button(Uint8 btn) {
    int n = game_.board_size - 1;
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        game_.cursor_r = std::max(0, game_.cursor_r - 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        game_.cursor_r = std::min(n, game_.cursor_r + 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        game_.cursor_f = std::max(0, game_.cursor_f - 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        game_.cursor_f = std::min(n, game_.cursor_f + 1); draw(); break;

    case SDL_CONTROLLER_BUTTON_A: {
        // Play the book move under the cursor
        if (jk_loading_ || jk_path_.empty()) break;
        const JosekiPosition& cur = jk_path_.back();
        for (const auto& m : cur.next_moves) {
            int r, f;
            if (jk_parse_placement(m.placement, 19, r, f) &&
                r == game_.cursor_r && f == game_.cursor_f) {
                jk_fetch_node(m.node_id);
                return;
            }
        }
        flash_       = "NOT IN BOOK";
        flash_until_ = SDL_GetTicks() + 1200;
        draw();
        break;
    }

    case SDL_CONTROLLER_BUTTON_B:
        jk_step_back();
        break;

    case SDL_CONTROLLER_BUTTON_START:
        open_popup_menu();
        break;

    default: break;
    }
}

// ── Correspondence games (MY GAMES) ───────────────────────────────────────────

// Snapshot the games OGS has pushed over the socket as active_game events (cached
// in OgsNet). No network round-trip — the REST ui/overview endpoint rejects the
// socket JWT (403). While MY GAMES is open, CORR_LIST_UPDATED pushes keep it live.
void App::refresh_corr_list() {
    corr_games_   = net_.corr_games_snapshot();
    corr_loading_ = false;
    if (corr_index_ >= (int)corr_games_.size())
        corr_index_ = std::max(0, (int)corr_games_.size() - 1);
}

void App::open_corr_list() {
    save_companion();  // persist any open review before switching away
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();
    is_local_game_ = false;
    state_      = AppState::CORR_LIST;
    corr_index_ = 0;
    corr_hover_ = -1;
    refresh_corr_list();
    set_status("MY GAMES");
    draw();
}

void App::draw_corr_list() {
    std::vector<std::string> lines;
    std::vector<SDL_Color>   colors;
    static const SDL_Color MOVE_COL = {120, 230, 120, 255};  // your-move rows, green
    static const SDL_Color WAIT_COL = {190, 190, 190, 255};  // waiting on opponent, grey

    for (const auto& g : corr_games_) {
        std::string row = g.my_move ? GLYPH_PS_CROSS " " : "  ";
        row += g.opp;
        row += "   " + std::to_string(g.board_size) + "x" + std::to_string(g.board_size);
        row += g.my_move ? "   YOUR MOVE" : "   WAITING";
        lines.push_back(row);
        colors.push_back(g.my_move ? MOVE_COL : WAIT_COL);
    }
    if (corr_loading_ && corr_games_.empty()) {
        lines.push_back("LOADING...");
        colors.push_back(WAIT_COL);
    } else if (corr_games_.empty()) {
        lines.push_back("(no correspondence games)");
        colors.push_back(WAIT_COL);
    }

    int to_move = 0;
    for (const auto& g : corr_games_) if (g.my_move) to_move++;
    std::string title = "MY GAMES — " + std::to_string(to_move) + " TO MOVE / " +
                        std::to_string((int)corr_games_.size()) + " ACTIVE";
    std::string footer = GLYPH_PS_CROSS " OPEN   " GLYPH_PS_TRIANGLE " REFRESH   "
                         GLYPH_PS_CIRCLE " LOBBY";
    // See draw_challenge_create — list screens don't render flash_ on their own.
    if (!flash_.empty() && flash_until_ > SDL_GetTicks()) {
        footer = flash_;
        flash_on_list_ = true;
    }

    bool self_present = !popup_active_;
    corr_lines_drawn_ = (int)lines.size();
    renderer_->draw_list_screen(title.c_str(), lines, corr_index_, footer.c_str(),
                                self_present, colors.data(), corr_hover_);
    draw_popup_overlay();
    if (!self_present)
        SDL_RenderPresent(renderer_->sdl);
}

void App::handle_corr_button(Uint8 btn) {
    int n = (int)corr_games_.size();
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        if (n > 0) { corr_index_ = (corr_index_ - 1 + n) % n; draw(); }
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        if (n > 0) { corr_index_ = (corr_index_ + 1) % n; draw(); }
        break;
    case SDL_CONTROLLER_BUTTON_A:
        // Open the highlighted game: game/connect → GAME_CONNECTED rebuilds the
        // board and switches to PLAYING, exactly as a live game does.
        if (n > 0 && corr_index_ >= 0 && corr_index_ < n) {
            is_corr_game_ = true;
            set_status("OPENING GAME...");
            net_.cmd_open_game(corr_games_[corr_index_].id);
            draw();
        }
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        refresh_corr_list();
        set_status("MY GAMES");
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_B:
        state_ = AppState::LOBBY;
        set_status("");
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_START:
        open_popup_menu();
        break;
    default: break;
    }
}

// Leave the open correspondence game without ending it: drop its live stream and
// return to MY GAMES, re-fetching so the list reflects any move just made.
void App::leave_corr_game() {
    if (game_.game_id) net_.cmd_disconnect_game(game_.game_id);
    is_corr_game_ = false;
    open_corr_list();
}

// Row under a screen point, using the same row count the last draw passed the list
// screen (which can include a LOADING/empty placeholder that isn't a real game).
int App::corr_hit(int mx, int my) const {
    if (state_ != AppState::CORR_LIST || corr_lines_drawn_ <= 0) return -1;
    return renderer_->list_screen_item_at(corr_lines_drawn_, corr_index_, mx, my);
}

// ── Credential prompt ─────────────────────────────────────────────────────────

void App::draw_credential_prompt() {
    // Login failures arrive as a flash; the credential screen renders it in the
    // footer (the board view that normally draws flash_ isn't used in this state).
    std::string footer = "KEYBOARD ENTRY";
    if (!flash_.empty() && flash_until_ > SDL_GetTicks()) {
        footer = flash_;
        flash_on_list_ = true;
    }
    renderer_->draw_credential_screen(cred_step_, cred_username_, cred_buf_, footer.c_str());
}

// Skip the login and use the app without an OGS account. Same lobby entry as
// AUTH_OK, minus the network: the socket never opens, so nothing will arrive.
void App::continue_offline() {
    offline_   = true;
    cred_step_ = 0;
    cred_username_.clear();
    cred_password_.clear();
    cred_buf_.clear();
    state_ = AppState::LOBBY;
    set_status("");
    load_demo_game();
    draw();
}

// LOG IN TO OGS from the offline lobby: back to the prompt, which ESC leaves again.
void App::return_to_login() {
    offline_   = false;
    state_     = AppState::CREDENTIAL_PROMPT;
    cred_step_ = 1;
    cred_buf_.clear();
    draw();
}

// ── "Who won?" quiz ───────────────────────────────────────────────────────────

// Can this game be a quiz question? Two kinds qualify. A counted game (numeric
// margin, e.g. "B+2.5") was played out and scored, so its final position is
// genuinely scorable. A resignation only qualifies if it ran deep enough that the
// position is essentially complete — most of the library's resignations stop well
// before that. Jigo is excluded outright: a drawn game has no winner to guess, and
// so are timeouts and forfeits, where the board doesn't tell you who won.
static const int WQ_MIN_RESIGN_MOVES = 250;

static bool wq_eligible(const SgfGame& g, int& winner_black) {
    const char* r = g.result;
    if ((r[0] != 'B' && r[0] != 'W') || r[1] != '+') return false;
    winner_black = (r[0] == 'B') ? 1 : 0;
    char kind = r[2];
    if (kind >= '0' && kind <= '9') return true;                 // counted
    if ((kind == 'R' || kind == 'r') && g.move_count >= WQ_MIN_RESIGN_MOVES)
        return true;                                             // deep resignation
    return false;
}

void App::wq_load_next() {
    // List the professional library once per session (same lookup the lobby
    // screensaver uses: next to the exe, else the dev layout two levels up).
    if (wq_pool_.empty()) {
        std::string dir = exe_dir() + "games";
        std::vector<std::string> rel;
        if (!Catalog::list_sgf_files(dir, rel) || rel.empty()) {
            dir = exe_dir() + "../../games";
            rel.clear();
            Catalog::list_sgf_files(dir, rel);
        }
        for (const auto& r : rel) wq_pool_.push_back(Catalog::join_path(dir, r));
    }
    if (wq_pool_.empty()) {
        set_status("NO PRO GAMES FOUND");
        return;
    }

    // About a third of the library qualifies, so a handful of random draws lands
    // one; the cap just stops a pathological library from spinning forever.
    SgfGame g;
    int winner = 1;
    bool found = false;
    for (int tries = 0; tries < 60 && !found; tries++) {
        const std::string& path = wq_pool_[(size_t)std::rand() % wq_pool_.size()];
        if (path == wq_path_) continue;          // don't ask the same game twice running
        if (!load_sgf(path, g))     continue;
        if (!wq_eligible(g, winner)) continue;
        wq_path_ = path;
        found = true;
    }
    if (!found) {
        set_status("NO SCORABLE POSITIONS FOUND");
        return;
    }

    // Replay to the final position. Same rules pass as sgf_board_at(), but writing
    // into a GameState so the full board renderer can draw it.
    int sz = g.board_size;
    wq_bs_ = sz;
    wq_board_.reset();
    wq_board_.board_size    = sz;
    wq_board_.turn_is_black = 1;
    int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
    for (int i = 0; i < g.move_count; i++) {
        int r, f;
        if (!parse_sgf_move(g.moves[i], r, f)) continue;
        if (r < 0 || r >= sz || f < 0 || f >= sz) continue;
        if (wq_board_.board[r][f] != 0) continue;
        bool is_black = (g.colors[i] == 1);
        if (GoRules::would_be_suicide(wq_board_.board, r, f, is_black, sz)) continue;
        wq_board_.board[r][f] = is_black ? 1 : 2;
        int cap_count = 0;
        GoRules::find_captured(wq_board_.board, is_black, r, f, cap_r, cap_f, cap_count, sz);
        for (int j = 0; j < cap_count; j++) wq_board_.board[cap_r[j]][cap_f[j]] = 0;
    }

    wq_black_    = g.black_name;
    wq_white_    = g.white_name;
    wq_date_     = g.date;
    wq_result_   = g.result;
    wq_winner_   = winner;
    wq_answered_ = false;
    wq_correct_  = false;
}

void App::open_who_won() {
    save_companion();  // persist any open review before switching away
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();
    is_local_game_ = false;
    demo_active_   = false;   // the lobby screensaver would fight for the board
    state_    = AppState::WHO_WON;
    wq_right_ = 0;            // a fresh run each time it's opened
    wq_total_ = 0;
    wq_path_.clear();
    set_status("");
    wq_load_next();
    draw();
}

void App::wq_answer(int guess_black) {
    if (wq_answered_) return;   // already revealed — ignore until the next question
    wq_answered_ = true;
    wq_correct_  = (guess_black == wq_winner_);
    wq_total_++;
    if (wq_correct_) wq_right_++;
    draw();
}

void App::handle_wq_button(Uint8 btn) {
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  wq_answer(1); break;   // guess black
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: wq_answer(0); break;   // guess white
    case SDL_CONTROLLER_BUTTON_A:
        if (wq_answered_) { wq_load_next(); draw(); }
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        // Study the game this position came from — only once the answer is out,
        // since the review shows the result.
        if (wq_answered_ && !wq_path_.empty()) load_sgf_for_review(wq_path_);
        break;
    case SDL_CONTROLLER_BUTTON_B:
        state_ = AppState::LOBBY;
        set_status("");
        load_demo_game();   // hand the board back to the screensaver
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_START:
        open_popup_menu();
        break;
    default: break;
    }
}

// ── Incoming challenges (accept / decline) ────────────────────────────────────

// GET me/challenges/ on a worker thread; the reply arrives as CHALLENGES_UPDATED.
void App::refresh_challenges() {
    if (chal_loading_) return;
    chal_loading_ = true;
    std::thread([this] { net_.fetch_challenges(); }).detach();
}

void App::open_challenges() {
    save_companion();
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();
    is_local_game_ = false;
    state_      = AppState::CHALLENGES;
    chal_index_ = 0;
    chal_hover_ = -1;
    chal_accept_confirm_  = false;
    chal_decline_confirm_ = false;
    set_status("CHALLENGES — LOADING...");
    refresh_challenges();
    draw();
}

void App::draw_challenges() {
    std::vector<std::string> lines;
    std::vector<SDL_Color>   colors;
    static const SDL_Color ROW_COL = {210, 210, 210, 255};

    for (const auto& c : challenges_) {
        std::string row = c.challenger;
        row += "   " + std::to_string(c.board_size) + "x" + std::to_string(c.board_size);
        row += c.ranked ? "   RANKED" : "   CASUAL";
        row += c.correspondence ? "   CORR" : "   LIVE";
        lines.push_back(row);
        colors.push_back(ROW_COL);
    }
    if (chal_loading_ && challenges_.empty()) {
        lines.push_back("LOADING...");
        colors.push_back(ROW_COL);
    } else if (challenges_.empty()) {
        lines.push_back("(no incoming challenges)");
        colors.push_back(ROW_COL);
    }

    std::string title  = "CHALLENGES — " + std::to_string((int)challenges_.size()) + " INCOMING";
    std::string footer = GLYPH_PS_CROSS " ACCEPT   " GLYPH_PS_TRIANGLE " DECLINE   "
                         GLYPH_PS_CIRCLE " BACK";
    // See draw_challenge_create: list screens don't render flash_, so the accept/
    // decline confirm prompts and their results would otherwise be invisible.
    if (!flash_.empty() && flash_until_ > SDL_GetTicks()) {
        footer = flash_;
        flash_on_list_ = true;
    }

    bool self_present = !popup_active_;
    chal_lines_drawn_ = (int)lines.size();
    renderer_->draw_list_screen(title.c_str(), lines, chal_index_, footer.c_str(),
                                self_present, colors.data(), chal_hover_);
    draw_popup_overlay();
    if (!self_present)
        SDL_RenderPresent(renderer_->sdl);
}

void App::handle_chal_button(Uint8 btn) {
    int n = (int)challenges_.size();
    auto disarm = [&]() { chal_accept_confirm_ = false; chal_decline_confirm_ = false; };
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        if (n > 0) { chal_index_ = (chal_index_ - 1 + n) % n; disarm(); draw(); }
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        if (n > 0) { chal_index_ = (chal_index_ + 1) % n; disarm(); draw(); }
        break;
    case SDL_CONTROLLER_BUTTON_A:
        // Accepting starts a real game — double-press confirm, same idiom as resign.
        if (n > 0 && chal_index_ >= 0 && chal_index_ < n) {
            if (!chal_accept_confirm_) {
                chal_accept_confirm_  = true;
                chal_decline_confirm_ = false;
                flash_       = "PRESS " GLYPH_PS_CROSS " AGAIN TO ACCEPT vs " +
                               challenges_[chal_index_].challenger;
                flash_until_ = SDL_GetTicks() + 2500;
            } else {
                chal_accept_confirm_ = false;
                int id = challenges_[chal_index_].id;
                set_status("ACCEPTING...");
                std::thread([this, id] { net_.accept_challenge(id); }).detach();
            }
            draw();
        }
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        // Declining is destructive too — same confirm gate.
        if (n > 0 && chal_index_ >= 0 && chal_index_ < n) {
            if (!chal_decline_confirm_) {
                chal_decline_confirm_ = true;
                chal_accept_confirm_  = false;
                flash_       = "PRESS " GLYPH_PS_TRIANGLE " AGAIN TO DECLINE";
                flash_until_ = SDL_GetTicks() + 2500;
            } else {
                chal_decline_confirm_ = false;
                int id = challenges_[chal_index_].id;
                set_status("DECLINING...");
                std::thread([this, id] { net_.decline_challenge(id); }).detach();
            }
            draw();
        }
        break;
    case SDL_CONTROLLER_BUTTON_B:
        state_ = AppState::LOBBY;
        set_status("");
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_START:
        open_popup_menu();
        break;
    default: break;
    }
}

int App::chal_hit(int mx, int my) const {
    if (state_ != AppState::CHALLENGES || chal_lines_drawn_ <= 0) return -1;
    return renderer_->list_screen_item_at(chal_lines_drawn_, chal_index_, mx, my);
}

// ── Composing a challenge ─────────────────────────────────────────────────────

// Correspondence pace presets: seconds of initial time / per-move increment, and
// the cap on banked time. Index matches cc_pace_.
static const int  kCcPaceInit[3] = { 86400,  259200,  604800  };  // 1d / 3d / 7d
static const int  kCcPaceMax [3] = { 259200, 604800,  1209600 };  // 3d / 7d / 14d
static const char* kCcPaceLbl[3] = { "1 DAY / MOVE", "3 DAYS / MOVE", "7 DAYS / MOVE" };
static const int  kCcSizes   [3] = { 9, 13, 19 };
static const char* kCcSizeLbl[3] = { "9x9", "13x13", "19x19" };
static const char* kCcColorLbl[3]= { "AUTOMATIC", "BLACK", "WHITE" };
static const char* kCcColorVal[3]= { "automatic", "black", "white" };

void App::open_challenge_create() {
    save_companion();
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();
    is_local_game_ = false;
    state_    = AppState::CHALLENGE_CREATE;
    cc_row_   = 0;
    cc_hover_ = -1;
    cc_send_confirm_ = false;
    // Friends are the opponent picker — fetch once per visit (open challenge still
    // works if the list is empty or the fetch fails).
    if (!friends_loading_) {
        friends_loading_ = true;
        std::thread([this] { net_.fetch_friends(); }).detach();
    }
    set_status("NEW CHALLENGE");
    draw();
}

void App::draw_challenge_create() {
    std::string opp;
    if (cc_opp_ > 0 && cc_opp_ - 1 < (int)friends_.size())
        opp = friends_[cc_opp_ - 1].username;
    else if (friends_loading_ && friends_.empty())
        opp = "OPEN CHALLENGE (LOADING FRIENDS...)";
    else
        opp = "OPEN CHALLENGE (ANYONE)";

    std::vector<std::string> lines;
    std::vector<SDL_Color>   colors;
    static const SDL_Color ROW_COL  = {210, 210, 210, 255};
    static const SDL_Color SEND_COL = {120, 230, 120, 255};

    lines.push_back(std::string("OPPONENT:     ") + opp);            colors.push_back(ROW_COL);
    lines.push_back(std::string("BOARD SIZE:   ") + kCcSizeLbl[cc_size_]);  colors.push_back(ROW_COL);
    lines.push_back(std::string("RANKED:       ") + (cc_ranked_ ? "YES" : "NO")); colors.push_back(ROW_COL);
    lines.push_back(std::string("PACE:         ") + kCcPaceLbl[cc_pace_]);  colors.push_back(ROW_COL);
    lines.push_back(std::string("YOUR COLOR:   ") + kCcColorLbl[cc_color_]); colors.push_back(ROW_COL);
    lines.push_back("SEND CHALLENGE");                                colors.push_back(SEND_COL);

    std::string footer = "D-PAD L/R CHANGE   " GLYPH_PS_CROSS " SEND   "
                         GLYPH_PS_CIRCLE " BACK";
    // Flash messages are drawn by the board view, not the list screen — without this
    // the double-press confirm prompt and the send result are both invisible here.
    if (!flash_.empty() && flash_until_ > SDL_GetTicks()) {
        footer = flash_;
        flash_on_list_ = true;
    }

    bool self_present = !popup_active_;
    cc_lines_drawn_ = (int)lines.size();
    renderer_->draw_list_screen("NEW CHALLENGE (CORRESPONDENCE)", lines, cc_row_,
                                footer.c_str(), self_present, colors.data(), cc_hover_);
    draw_popup_overlay();
    if (!self_present)
        SDL_RenderPresent(renderer_->sdl);
}

void App::cc_cycle(int delta) {
    switch (cc_row_) {
    case 0: {   // opponent: 0 = open challenge, then one slot per friend
        int n = (int)friends_.size() + 1;
        cc_opp_ = (cc_opp_ + delta + n) % n;
        break;
    }
    case 1: cc_size_  = (cc_size_  + delta + 3) % 3; break;
    case 2: cc_ranked_= !cc_ranked_;                 break;
    case 3: cc_pace_  = (cc_pace_  + delta + 3) % 3; break;
    case 4: cc_color_ = (cc_color_ + delta + 3) % 3; break;
    default: break;   // SEND row has no value to cycle
    }
}

void App::cc_send() {
    ChallengeRequest req;
    req.opponent_id = (cc_opp_ > 0 && cc_opp_ - 1 < (int)friends_.size())
                          ? friends_[cc_opp_ - 1].id : 0;
    req.board_size     = kCcSizes[cc_size_];
    req.ranked         = cc_ranked_;
    req.initial_time   = kCcPaceInit[cc_pace_];
    req.time_increment = kCcPaceInit[cc_pace_];
    req.max_time       = kCcPaceMax [cc_pace_];
    req.color          = kCcColorVal[cc_color_];
    set_status("SENDING CHALLENGE...");
    std::thread([this, req] { net_.send_challenge(req); }).detach();
}

void App::handle_cc_button(Uint8 btn) {
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        cc_row_ = (cc_row_ - 1 + CC_ROWS) % CC_ROWS;
        cc_send_confirm_ = false;
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        cc_row_ = (cc_row_ + 1) % CC_ROWS;
        cc_send_confirm_ = false;
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        cc_cycle(-1);
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        cc_cycle(+1);
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_A:
        if (cc_row_ == CC_ROWS - 1) {
            // Sending creates a real challenge — double-press confirm.
            if (!cc_send_confirm_) {
                cc_send_confirm_ = true;
                flash_       = "PRESS " GLYPH_PS_CROSS " AGAIN TO SEND CHALLENGE";
                flash_until_ = SDL_GetTicks() + 2500;
            } else {
                cc_send_confirm_ = false;
                cc_send();
            }
        } else {
            cc_cycle(+1);   // on a settings row, cross advances the value
        }
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_B:
        state_ = AppState::LOBBY;
        set_status("");
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_START:
        open_popup_menu();
        break;
    default: break;
    }
}

int App::cc_hit(int mx, int my) const {
    if (state_ != AppState::CHALLENGE_CREATE || cc_lines_drawn_ <= 0) return -1;
    return renderer_->list_screen_item_at(cc_lines_drawn_, cc_row_, mx, my);
}

// OGS rank number → display string: 1..29 = 29k..1k, 30+ = 1d+. 0 = unrated.
static std::string ogs_rank_str(int r) {
    if (r <= 0)  return "?";
    if (r < 30)  return std::to_string(30 - r) + "K";
    return std::to_string(r - 29) + "D";
}

void App::open_puzzle_browser() {
    save_companion();  // persist any open review before switching away
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();
    is_local_game_ = false;
    state_    = AppState::PUZZLE_BROWSE;
    pz_view_  = PzView::COLLECTIONS;
    pz_index_ = 0;
    local_pz_kind_ = LocalPzKind::NONE;   // always land on the collections view, local lists re-entered from there
    pz_rebuild_display();   // pinned sets show immediately, even before any fetch
    if (pz_collections_.empty())
        pz_launch_fetch(1, pz_col_page_);   // first visit — load page 1
    else
        draw();                              // reuse the cached page
}

// Enter the goproblems list: every indexed problem, easiest first, cursor on
// the first one not yet solved so each visit resumes where the last left off.
void App::open_gp_list() {
    local_pz_kind_ = LocalPzKind::NONE;
    drill_paths_.clear();
    pz_list_.clear();
    pz_list_.reserve(gp_index_.size());
    int first_open = -1;
    for (const auto& e : gp_index_) {
        std::string genre = e.genre;
        for (char& ch : genre) ch = (char)toupper((unsigned char)ch);
        int id = GP_ID_BASE + e.gp_id;
        pz_list_.push_back({id, ogs_rank_str(std::max(1, e.level)) + "  " + genre
                                    + "  (" + std::to_string(e.gp_id) + ")"});
        if (first_open < 0 && !pz_solved_ids_.count(id)) first_open = (int)pz_list_.size() - 1;
    }
    pz_list_title_  = "GOPROBLEMS.COM";
    pz_open_col_id_ = GP_COLLECTION_ID;
    pz_view_        = PzView::PUZZLES;
    pz_index_       = std::max(0, first_open);
    draw();
}

void App::pz_launch_fetch(int kind, int arg) {
    if (pz_loading_) return;
    pz_loading_ = true;
    auto res  = std::make_shared<PuzzleFetch>();
    pz_fetch_ = res;
    int page_size = PZ_PAGE_SIZE;
    // goproblems ids load from the local download instead of the network
    std::string gp_path = (kind == 2 && arg >= GP_ID_BASE)
        ? Catalog::join_path(gp_dir_, std::to_string(arg - GP_ID_BASE) + ".sgf") : "";
    std::thread([res, kind, arg, page_size, gp_path] {
        switch (kind) {
        case 1: res->ok = ogs_fetch_puzzle_collections(arg, page_size,
                                                       res->collections, res->total); break;
        case 2: res->ok = !gp_path.empty()
                              ? gp_load_problem(gp_path, arg - GP_ID_BASE, res->puzzle, res->err)
                              : ogs_fetch_puzzle(arg, res->puzzle);
                break;
        case 3: res->ok = ogs_fetch_collection_puzzles(arg, res->siblings); break;
        }
        res->kind = kind;
        res->ready.store(true);
        SDL_Event ev{};
        ev.type = g_net_event_type;   // wake the event loop
        SDL_PushEvent(&ev);
    }).detach();
    draw();   // repaint with the LOADING row
}

void App::poll_puzzle_fetch() {
    if (!pz_fetch_ || !pz_fetch_->ready.load()) return;
    auto res = pz_fetch_;
    pz_fetch_.reset();
    pz_loading_ = false;
    // User may have left for another state while the fetch was in flight
    if (state_ != AppState::PUZZLE_BROWSE && state_ != AppState::PUZZLE_PLAY) return;
    if (!res->ok) {
        // A goproblems problem the solver can't use — while stepping, skip on
        // past it (bounded) rather than stalling on the same one every press
        if (res->kind == 2 && !res->err.empty() && pz_step_dir_ != 0 && pz_step_target_ >= 0 && pz_step_skips_ < 5) {
            pz_list_pos_ = pz_step_target_;
            pz_step_skips_++;
            pz_step(pz_step_dir_);
            return;
        }
        pz_step_dir_   = 0;
        pz_step_skips_ = 0;
        flash_       = res->err.empty() ? "OGS FETCH FAILED" : "GOPROBLEMS: " + res->err;
        flash_until_ = SDL_GetTicks() + 2500;
        draw();
        return;
    }
    pz_step_dir_   = 0;
    pz_step_skips_ = 0;
    switch (res->kind) {
    case 1:
        pz_collections_ = std::move(res->collections);
        pz_rebuild_display();
        pz_col_total_   = res->total;
        pz_view_        = PzView::COLLECTIONS;
        pz_index_       = std::min(pz_index_, std::max(0, (int)pz_display_cols_.size() - 1));
        break;
    case 2:
        pz_ = std::move(res->puzzle);
        pz_visits_.clear();   // fresh puzzle — line coverage starts over
        drill_play_path_.clear();   // a network puzzle displaced any local file
        local_pz_kind_ = LocalPzKind::NONE;
        pz_list_pos_ = -1;
        for (int i = 0; i < (int)pz_list_.size(); i++)
            if (pz_list_[i].first == pz_.id) { pz_list_pos_ = i; break; }
        pz_start();
        return;   // pz_start draws
    case 3:
        pz_list_  = std::move(res->siblings);
        pz_view_  = PzView::PUZZLES;
        pz_index_ = pz_last_solved_index();
        // Backfill collection ids for solves recorded before the mapping existed,
        // so old progress starts coloring the collections list too.
        if (pz_open_col_id_ > 0) {
            bool changed = false;
            for (const auto& p : pz_list_)
                if (pz_solved_ids_.count(p.first) &&
                    pz_solved_col_[p.first] != pz_open_col_id_) {
                    pz_solved_col_[p.first] = pz_open_col_id_;
                    changed = true;
                }
            if (changed) save_solved_puzzles();
        }
        break;
    }
    draw();
}

static constexpr int PZ_OPPONENT_REPLY_DELAY_MS = 500;

// Set the board to the puzzle's initial position and enter PUZZLE_PLAY.
// Also serves as "retry" — everything is rebuilt from pz_.
void App::pz_start() {
    if (pz_.width != pz_.height || pz_.width < 2 || pz_.width > MAX_BOARD_SIZE) {
        flash_       = "UNSUPPORTED BOARD SIZE";
        flash_until_ = SDL_GetTicks() + 2500;
        state_ = AppState::PUZZLE_BROWSE;
        draw();
        return;
    }
    // Local drills get a fresh random orientation on every attempt (including
    // circle-retry) so the shape is learned, not its screen position. Tree
    // pointers are untouched — pz_visits_ line coverage survives the reshuffle.
    // The cursor is mapped along with the stones so it stays on the shape.
    // (EDIT DRILL reloads from disk, so saves stay in authored orientation.)
    if (!drill_play_path_.empty())
        drill_random_transform(pz_, &game_.cursor_f, &game_.cursor_r);
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = pz_.width;
    game_.board.reset();
    game_.board.board_size    = pz_.width;
    game_.board.turn_is_black = pz_.black_to_play ? 1 : 0;
    game_.my_color   = pz_.solver_black ? 1 : 0;
    game_.my_turn    = true;
    game_.black_name = "BLACK";
    game_.white_name = "WHITE";
    game_.black_rank = game_.white_rank = "";
    game_.black_secs = game_.white_secs = -1;
    // Keep the cursor where it was (clamped to this board) — recentering on every
    // puzzle jump/retry forces a re-approach when grinding through a collection.
    game_.cursor_r = std::max(0, std::min(pz_.width - 1, game_.cursor_r));
    game_.cursor_f = std::max(0, std::min(pz_.width - 1, game_.cursor_f));
    auto apply_stones = [&](const std::string& coords, char stone) {
        for (size_t i = 0; i + 1 < coords.size(); i += 2) {
            int f = coords[i]     - 'a';
            int r = coords[i + 1] - 'a';
            if (r >= 0 && r < pz_.width && f >= 0 && f < pz_.width)
                game_.board.board[r][f] = stone;
        }
    };
    apply_stones(pz_.initial_black, 1);
    apply_stones(pz_.initial_white, 2);
    game_.board.save_snapshot();
    game_.history.push_back(game_.board);
    last_move_r_ = last_move_f_ = -1;

    pz_node_            = &pz_.tree;
    pz_done_            = false;
    pz_solved_          = false;
    pz_explore_         = false;
    pz_explore_anchor_  = 0;
    pz_more_lines_      = false;
    pz_pending_reply_   = nullptr;
    pz_refresh_marks();       // root-node marks annotate the initial position
    pz_build_tree_render();   // solution tree in the left panel
    pz_banner_.clear();
    pz_comment_  = pz_.description;   // the author's task statement
    black_label_ = game_.black_name;
    white_label_ = game_.white_name;
    state_ = AppState::PUZZLE_PLAY;
    std::string rank = (pz_.rank > 0) ? "  (" + ogs_rank_str(pz_.rank) + ")" : "";
    set_status("YOU ARE " + std::string(pz_.solver_black ? "BLACK" : "WHITE") + rank);
    // Solver differs from who moves first: the opponent's opening move isn't a
    // real decision point for the human, so it plays itself (same delayed,
    // least-visited-first choice as any other automatic reply) before control
    // passes over — see pz_fire_pending_reply().
    if (pz_.solver_black != pz_.black_to_play && !pz_.tree.branches.empty()) {
        const PuzzleMoveNode* reply = pz_pick_reply(&pz_.tree);
        if (reply) {
            pz_pending_reply_ = reply;
            pz_reply_at_      = SDL_GetTicks() + PZ_OPPONENT_REPLY_DELAY_MS;
        }
    }
    draw();
}

// Project the puzzle's solution tree into the analysis-tree renderer's node
// format (same column-assignment walk as build_analysis_tree_render). Correct-
// answer nodes get the "marked" highlight so solution endpoints are visible.
void App::pz_build_tree_render() {
    pz_tree_render_.clear();
    pz_cur_depth_ = 0;
    int max_col      = 0;
    int player_black = pz_.black_to_play ? 1 : 0;
    std::function<void(const PuzzleMoveNode*, int, int, int, int)> dfs =
        [&](const PuzzleMoveNode* node, int depth, int col, int parent_col, int parent_depth) {
            AnalysisTreeRenderNode rn;
            rn.depth        = depth;
            rn.col          = col;
            rn.current      = (node == pz_node_);
            if (rn.current) pz_cur_depth_ = depth;
            rn.parent_depth = parent_depth;
            rn.parent_col   = parent_col;
            // Root has no move; odd depths are the solver's moves
            rn.move_color   = (depth == 0) ? -1
                            : (depth % 2 == 1) ? player_black : 1 - player_black;
            rn.goal         = node->correct;   // green halo on the node itself —
                                               // full-row marked highlights read as
                                               // noise when several goals coexist
            pz_tree_render_.push_back(rn);
            for (int i = 0; i < (int)node->branches.size(); i++) {
                int child_col = (i == 0) ? col : ++max_col;
                dfs(&node->branches[i], depth + 1, child_col, col, depth);
            }
        };
    dfs(&pz_.tree, 0, 0, 0, -1);
}

// Land on a solution-tree node (just reached by whoever moved), judge it, and
// let the automatic opponent respond when the line continues.
// Does this subtree contain a correct ending at all? Wrong lines and judged
// dead-ends exist to punish mistakes — they shouldn't be required viewing for
// coverage, and the opponent shouldn't steer toward them.
static bool pz_leads_to_correct(const PuzzleMoveNode* n) {
    if (n->correct) return true;
    for (const auto& b : n->branches)
        if (pz_leads_to_correct(&b)) return true;
    return false;
}

// True if a never-traversed node lies on some path to a correct ending within
// this subtree — drives both the opponent's steer-toward-unseen-lines choice
// and the "MORE LINES REMAIN" / "ALL LINES SEEN" verdict after a solve.
bool App::pz_subtree_unexplored(const PuzzleMoveNode* n) const {
    if (!pz_leads_to_correct(n)) return false;
    auto it = pz_visits_.find(n);
    if (it == pz_visits_.end() || it->second == 0) return true;
    for (const auto& b : n->branches)
        if (pz_subtree_unexplored(&b)) return true;
    return false;
}

// Opponent resistance choice among `node`'s branches. First preference: a
// branch whose subtree still contains something never traversed — raw
// least-visited counts alone can ping-pong between an already-exhausted
// shallow branch and a deep bushy one, re-treading known lines while unseen
// ones wait. Only when everything below `node` has been seen does it fall
// back to least-visited for variety. Ties (usually several branches all at
// 0 visits) are broken randomly rather than by authored order, so the same
// drill doesn't show its lines in the same sequence every time. Shared by
// mid-line automatic replies and, when the solver isn't who moves first, the
// opponent's opening move too (see pz_start()).
const PuzzleMoveNode* App::pz_pick_reply(const PuzzleMoveNode* node) {
    auto pick_least_visited = [&](bool require_unexplored) -> const PuzzleMoveNode* {
        int fewest = INT_MAX;
        std::vector<const PuzzleMoveNode*> tied;
        for (const auto& b : node->branches) {
            if (require_unexplored && !pz_subtree_unexplored(&b)) continue;
            auto it = pz_visits_.find(&b);
            int v = (it == pz_visits_.end()) ? 0 : it->second;
            if (v < fewest) { fewest = v; tied.clear(); tied.push_back(&b); }
            else if (v == fewest) tied.push_back(&b);
        }
        if (tied.empty()) return nullptr;
        return tied[(size_t)std::rand() % tied.size()];
    };
    const PuzzleMoveNode* reply = pick_least_visited(true);
    if (!reply) reply = pick_least_visited(false);
    return reply;
}

void App::pz_advance(const PuzzleMoveNode* node, bool opponent_follows) {
    pz_node_ = node;
    pz_visits_[node]++;   // every traversed node counts toward line coverage
    if (!node->text.empty()) pz_comment_ = node->text;
    if (!node->marks.empty()) pz_refresh_marks();   // new annotations replace the old
    pz_build_tree_render();                          // move the tree-panel highlight

    if (node->correct) {
        pz_done_   = true;
        pz_solved_ = true;
        pz_banner_ = "SOLVED!";
        if (pz_.id > 0) {
            bool fresh = pz_solved_ids_.insert(pz_.id).second;
            // Record/repair the collection mapping too — legacy solves have 0 here
            bool remap = pz_.collection_id > 0 &&
                         pz_solved_col_[pz_.id] != pz_.collection_id;
            if (remap) pz_solved_col_[pz_.id] = pz_.collection_id;
            if (fresh || remap) save_solved_puzzles();
        }
        // Whole-tree truth, not just this run's path: anything anywhere in the
        // authored tree never yet traversed (opponent resistance you haven't
        // faced, or your own alternative lines you haven't tried) keeps the
        // "more lines" nudge alive.
        pz_more_lines_ = false;
        for (const auto& b : pz_.tree.branches)
            if (pz_subtree_unexplored(&b)) { pz_more_lines_ = true; break; }
        set_status(pz_more_lines_
                       ? "MORE LINES REMAIN\n" GLYPH_PS_CIRCLE ": RETRY"
                       : "ALL LINES SEEN\nR3: NEXT   " GLYPH_PS_CIRCLE ": RETRY   " GLYPH_PS_SQUARE ": LIST");
        draw();
        return;
    }
    if (node->wrong || node->branches.empty()) {
        pz_done_   = true;
        pz_solved_ = false;
        pz_banner_ = "WRONG";
        // Auto-save real (network) puzzles for later review — never for
        // drills, which already live in their own local file with this
        // wrong line baked right into the tree.
        bool newly_saved = drill_play_path_.empty() && save_missed_puzzle();
        set_status(newly_saved
                       ? "SAVED TO MISSED PUZZLES\nPRESS " GLYPH_PS_CIRCLE " TO RETRY"
                       : "PRESS " GLYPH_PS_CIRCLE " TO RETRY");
        draw();
        return;
    }
    if (opponent_follows && pz_.opponent_auto) {
        const PuzzleMoveNode* reply = pz_pick_reply(node);

        // Delay placing it slightly so the player's own move is visible on
        // its own for a beat, instead of both stones landing in the same
        // frame — fired from the main loop tick via pz_fire_pending_reply().
        pz_pending_reply_ = reply;
        pz_reply_at_      = SDL_GetTicks() + PZ_OPPONENT_REPLY_DELAY_MS;
        draw();
        return;
    }
    set_status("YOUR MOVE");
    draw();
}

// Apply the opponent's chosen reply once the visibility delay has elapsed,
// then keep judging from there (it may itself be solved/wrong/branch again).
void App::pz_fire_pending_reply() {
    const PuzzleMoveNode* reply = pz_pending_reply_;
    pz_pending_reply_ = nullptr;
    if (!reply) return;

    if (reply->x >= 0 && reply->y >= 0 &&
        reply->y < game_.board_size && reply->x < game_.board_size &&
        game_.board.board[reply->y][reply->x] == 0) {
        int opp_black = (game_.board.turn_is_black == 1);
        game_.board.save_snapshot();
        // Only mutate turn/history if the placement actually happened —
        // a refused move must never flip whose turn it is
        if (game_.board.place_stone(reply->y, reply->x, opp_black)) {
            game_.board.turn_is_black = opp_black ? 0 : 1;
            game_.history.push_back(game_.board);
            last_move_r_ = reply->y;
            last_move_f_ = reply->x;
        }
    }
    pz_advance(reply, false);
}

// Player plays at (r, f): match against the current node's branches — or, once
// exploring (triangle, off the authored tree, or after a verdict), free
// exploration: stones alternate colors with no judging and no auto-opponent, so
// lines the author didn't cover can be tested by playing both sides. Triggers
// review the moves made so far; circle returns to the judged position explore
// mode branched off from (or retries the whole puzzle if not exploring).
void App::pz_place(int r, int f) {
    if (!pz_node_) return;
    if (pz_pending_reply_) return;  // opponent's reply is mid-delay — not your turn yet
    if (r < 0 || f < 0 || r >= game_.board_size || f >= game_.board_size) return;

    if (game_.history_pos >= 0) {
        // Placing from a reviewed (non-live) position discards whatever came
        // after, same as a normal undo-then-move — only ever reachable while
        // already exploring (triggers are gated on pz_explore_), so pz_node_
        // (frozen since exploration began) never needs to move here. If this
        // rewinds past the explore anchor (scrubbed back into the judged
        // prefix, then branched), pull the anchor in too — it must never sit
        // beyond the live history length, or "back to solving" would try to
        // GROW game_.history, default-constructing GameState elements (each
        // ~12MB — see the CRITICAL FOOTGUN note on why that's dangerous).
        game_.history.resize(game_.history_pos + 1);
        game_.board         = game_.history.back();
        game_.history_pos   = -1;
        pz_explore_anchor_  = std::min(pz_explore_anchor_, (int)game_.history.size());
    }

    if (game_.board.board[r][f] != 0) return;

    int is_black = (game_.board.turn_is_black == 1);

    // Legality first — an illegal move must change NOTHING (a rejected placement
    // previously still flipped the turn, silently swapping colors for the next click).
    if (GoRules::would_be_suicide(game_.board.board, r, f, is_black, game_.board_size)) {
        ko_flash_until_ = SDL_GetTicks() + 400;   // red cursor flash, like live play
        draw();
        return;
    }
    // Simple ko: simulated result must not recreate the position before the last move
    if (game_.history.size() >= 2) {
        char test[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
        memcpy(test, game_.board.board, sizeof(test));
        test[r][f] = is_black ? 1 : 2;
        int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
        int cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
        int cap_count = 0;
        GoRules::find_captured(test, is_black, r, f, cap_r, cap_f, cap_count, game_.board_size);
        for (int i = 0; i < cap_count; i++) test[cap_r[i]][cap_f[i]] = 0;
        if (memcmp(test, game_.history[game_.history.size() - 2].board, sizeof(test)) == 0) {
            ko_flash_until_ = SDL_GetTicks() + 400;
            draw();
            return;
        }
    }

    // Place the stone (captures apply) so the player sees their move either way
    game_.board.save_snapshot();
    game_.board.place_stone(r, f, is_black);
    game_.board.turn_is_black = is_black ? 0 : 1;
    game_.history.push_back(game_.board);
    last_move_r_ = r;
    last_move_f_ = f;

    if (pz_explore_ || pz_done_) {
        // Already exploring (or continuing past a verdict) — sandbox move
        if (!pz_explore_) {
            pz_explore_anchor_ = (int)game_.history.size() - 1;  // the verdict position
            pz_explore_ = true;
            pz_banner_.clear();   // drop the SOLVED!/WRONG banner once exploring
        }
        set_status("EXPLORING — " GLYPH_PS_CIRCLE " BACK TO SOLVING\nTRIGGERS: REVIEW");
        draw();
        return;
    }

    const PuzzleMoveNode* hit = nullptr;
    for (const auto& b : pz_node_->branches)
        if (b.x == f && b.y == r) { hit = &b; break; }

    if (!hit) {
        // Off the authored tree: not judged, just warned — free exploration from
        // here on (both sides played manually; there is no authored reply anyway).
        pz_explore_anchor_ = (int)game_.history.size() - 1;  // undo this off-tree move
        pz_explore_ = true;
        pz_comment_ = "OFF THE SOLUTION TREE — FREE PLAY, BOTH SIDES";
        set_status("OFF TREE — " GLYPH_PS_CIRCLE " BACK TO SOLVING\nTRIGGERS: REVIEW");
        draw();
        return;
    }
    pz_advance(hit, /*opponent_follows=*/true);
}

// Triangle: drop into the free sandbox right where you stand, without playing
// a move first. Only meaningful pre-verdict — once pz_done_, any placement
// already falls into the same sandbox (see pz_place).
void App::pz_enter_explore() {
    if (pz_explore_ || pz_done_ || pz_pending_reply_) return;
    pz_explore_anchor_ = (int)game_.history.size();
    pz_explore_ = true;
    pz_banner_.clear();
    pz_comment_ = "FREE PLAY — BOTH SIDES";
    set_status("EXPLORING — " GLYPH_PS_CIRCLE " BACK TO SOLVING\nTRIGGERS: REVIEW");
    draw();
}

// Circle, mid-exploration: discard the sandbox detour and resume at the judged
// position explore mode branched off from. pz_node_ never moves while
// pz_explore_ is set, so it's already sitting at the right spot — this just
// rewinds the board/history to match and restores the verdict banner, if any.
void App::pz_return_to_solving() {
    pz_pending_reply_ = nullptr;  // defensive — unreachable in practice, see pz_place()/pz_enter_explore() guards
    // Defense in depth: this resize must only ever shrink. pz_place() keeps the
    // anchor clamped to <= history size already, but a stale/out-of-range anchor
    // here would otherwise GROW game_.history, default-constructing GameState
    // elements (~12MB each — see the CRITICAL FOOTGUN note on why that's unsafe).
    int target = std::min((int)game_.history.size(), std::max(1, pz_explore_anchor_));
    game_.history.resize(target);
    game_.board       = game_.history.back();
    game_.history_pos = -1;
    pz_explore_        = false;
    pz_build_tree_render();
    if (pz_done_) {
        pz_banner_ = pz_solved_ ? "SOLVED!" : "WRONG";
        set_status(pz_solved_
            ? (pz_more_lines_ ? "MORE LINES REMAIN\n" GLYPH_PS_CIRCLE ": RETRY"
                               : "ALL LINES SEEN\nR3: NEXT   " GLYPH_PS_CIRCLE ": RETRY   " GLYPH_PS_SQUARE ": LIST")
            : "PRESS " GLYPH_PS_CIRCLE " TO RETRY");
    } else {
        pz_banner_.clear();
        pz_comment_ = (pz_node_ && !pz_node_->text.empty()) ? pz_node_->text : pz_.description;
        std::string rank = (pz_.rank > 0) ? "  (" + ogs_rank_str(pz_.rank) + ")" : "";
        set_status("YOU ARE " + std::string(pz_.solver_black ? "BLACK" : "WHITE") + rank);
    }
    draw();
}

// L3/R3: previous/next puzzle within the open collection.
void App::pz_step(int dir) {
    if (pz_list_.empty() || pz_list_pos_ < 0 || pz_loading_) return;
    int n    = (int)pz_list_.size();
    int next = (pz_list_pos_ + dir + n) % n;
    if (local_pz_kind_ != LocalPzKind::NONE && !drill_paths_.empty()) {
        local_pz_load_and_start(next);   // local files — no network fetch
        return;
    }
    pz_step_dir_    = dir;
    pz_step_target_ = next;
    pz_launch_fetch(2, pz_list_[next].first);
}

void App::draw_puzzle_browser() {
    // Progress coloring: collections turn yellow once any of their puzzles is
    // solved and green once all of them are; solved puzzles show green in the
    // list view. Alpha 0 = keep the list screen's normal white/accent colors.
    static constexpr SDL_Color PZ_NO_COLOR = {0,   0,   0,   0};
    static constexpr SDL_Color PZ_STARTED  = {255, 213, 74,  255};
    static constexpr SDL_Color PZ_DONE     = {105, 220, 130, 255};

    std::vector<std::string> lines;
    std::vector<SDL_Color>   colors;
    std::string title, footer;
    if (pz_view_ == PzView::COLLECTIONS) {
        int pages = std::max(1, (pz_col_total_ + PZ_PAGE_SIZE - 1) / PZ_PAGE_SIZE);
        title = "PUZZLES — PAGE " + std::to_string(pz_col_page_)
              + "/" + std::to_string(pages);
        std::map<int, int> solved_per_col = pz_solved_per_collection();
        for (const auto& c : pz_display_cols_) {
            std::string ln = c.name + "  —  " + std::to_string(c.puzzle_count) + " PUZZLES";
            if (c.min_rank > 0 || c.max_rank > 0)
                ln += "  " + ogs_rank_str(c.min_rank) + "-" + ogs_rank_str(c.max_rank);
            if (c.rating > 0.f) {
                char rb[16];
                snprintf(rb, sizeof(rb), "  R%.1f", c.rating);
                ln += rb;
            }
            ln += "  BY " + c.owner;
            lines.push_back(ln);
            // Book pins list under a routing sentinel, but their problems are
            // solved-tracked under the real collection id the importer stamped
            // in — resolve through local_books_ so they colour like any other.
            int col_id = (c.id == -4) ? GP_COLLECTION_ID : c.id;
            if (c.id <= -10)
                for (const auto& b : local_books_)
                    if (b.pin_id == c.id) { col_id = b.collection_id; break; }
            auto it = solved_per_col.find(col_id);
            int solved = (it != solved_per_col.end()) ? it->second : 0;
            colors.push_back(c.puzzle_count > 0 && solved >= c.puzzle_count ? PZ_DONE
                             : solved > 0                                   ? PZ_STARTED
                                                                            : PZ_NO_COLOR);
        }
        footer = GLYPH_PS_CROSS " OPEN   LEFT/RIGHT: PAGE   " GLYPH_PS_CIRCLE " LOBBY";
    } else if (local_pz_kind_ != LocalPzKind::NONE) {
        // Personal sets have no solved tracking (their ids are negative
        // sentinels); imported books do, so they get the same [X]/[ ] marks
        // and solved count an OGS collection list shows.
        bool tracked = (local_pz_kind_ == LocalPzKind::BOOK);
        int solved_here = 0;
        if (tracked)
            for (const auto& p : pz_list_)
                if (pz_solved_ids_.count(p.first)) solved_here++;
        title = pz_list_title_ + "  (" +
                (tracked ? std::to_string(solved_here) + "/" : "") +
                std::to_string((int)pz_list_.size()) + (tracked ? " SOLVED)" : ")");
        for (const auto& p : pz_list_) {
            bool solved = tracked && pz_solved_ids_.count(p.first) != 0;
            lines.push_back(tracked ? (solved ? "[X] " : "[ ] ") + p.second : p.second);
            colors.push_back(solved ? PZ_DONE : PZ_NO_COLOR);
        }
        footer = drill_rename_active_
                     ? "NEW NAME: " + drill_rename_buf_ + "_   ENTER=OK   ESC=CANCEL"
                     : GLYPH_PS_CROSS " OPEN   " GLYPH_PS_CIRCLE " COLLECTIONS";
        // Refresh the setup-position preview when the cursor moved to a new file
        if (pz_index_ != drill_thumb_idx_) {
            drill_thumb_idx_ = pz_index_;
            drill_thumb_bs_  = 0;
            if (pz_index_ >= 0 && pz_index_ < (int)drill_paths_.size()) {
                OgsPuzzle tmp;
                std::string err;
                if (load_drill_sgf(drill_paths_[pz_index_], tmp, err) &&
                    tmp.width <= BOARD_SIZE) {
                    memset(drill_thumb_, 0, sizeof(drill_thumb_));
                    auto apply = [&](const std::string& s, char stone) {
                        for (size_t i = 0; i + 1 < s.size(); i += 2) {
                            int f = s[i] - 'a', r = s[i + 1] - 'a';
                            if (r >= 0 && r < tmp.width && f >= 0 && f < tmp.width)
                                drill_thumb_[r][f] = stone;
                        }
                    };
                    apply(tmp.initial_black, 1);
                    apply(tmp.initial_white, 2);
                    drill_thumb_bs_ = tmp.width;
                }
            }
        }
    } else {
        int solved_here = 0;
        for (const auto& p : pz_list_)
            if (pz_solved_ids_.count(p.first)) solved_here++;
        title = "PUZZLES — " + pz_list_title_ + "  (" + std::to_string(solved_here)
              + "/" + std::to_string((int)pz_list_.size()) + " SOLVED)";
        for (const auto& p : pz_list_) {
            bool solved = pz_solved_ids_.count(p.first) != 0;
            lines.push_back((solved ? "[X] " : "[ ] ") + p.second);
            colors.push_back(solved ? PZ_DONE : PZ_NO_COLOR);
        }
        footer = gp_list_open()
                     ? GLYPH_PS_CROSS " SOLVE   LEFT/RIGHT: RANK   " GLYPH_PS_CIRCLE " COLLECTIONS"
                     : GLYPH_PS_CROSS " SOLVE   " GLYPH_PS_CIRCLE " COLLECTIONS";
    }
    if (pz_loading_) {
        lines.push_back("LOADING...");
        colors.push_back(PZ_NO_COLOR);
    }
    // The list screen presents itself unless something needs to layer on top
    // (the START popup, or the setup-position preview board).
    bool want_thumb = local_pz_kind_ != LocalPzKind::NONE && pz_view_ == PzView::PUZZLES && drill_thumb_bs_ > 0;
    bool self_present = !popup_active_ && !want_thumb;
    pz_lines_drawn_ = (int)lines.size();   // keep the hit-test in step with the draw
    renderer_->draw_list_screen(title.c_str(), lines, pz_index_, footer.c_str(),
                                self_present, colors.data(), pz_hover_);
    if (want_thumb) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer_->sdl, &w, &h);
        int size = std::min(w / 3, h / 2);
        renderer_->render_mini_board(w - size - 40, (h - size) / 2, size,
                                     drill_thumb_, drill_thumb_bs_);
    }
    draw_popup_overlay();
    if (!self_present)
        SDL_RenderPresent(renderer_->sdl);
}

// Input for both puzzle states (called from handle_controller_button).
int App::pz_item_count() const {
    return (pz_view_ == PzView::COLLECTIONS) ? (int)pz_display_cols_.size()
                                             : (int)pz_list_.size();
}

// Row under a screen point, using the same row count the last draw passed the list
// screen (which can include a "LOADING..." placeholder that isn't a real entry).
int App::pz_hit(int mx, int my) const {
    if (state_ != AppState::PUZZLE_BROWSE || pz_lines_drawn_ <= 0) return -1;
    return renderer_->list_screen_item_at(pz_lines_drawn_, pz_index_, mx, my);
}

void App::handle_puzzle_button(Uint8 btn) {
    // Pad/keyboard input means the mouse isn't what moved last — drop the hover
    // tint so two different rows are never lit at once.
    pz_hover_ = -1;
    if (state_ == AppState::PUZZLE_BROWSE) {
        int total = (pz_view_ == PzView::COLLECTIONS) ? (int)pz_display_cols_.size()
                                                      : (int)pz_list_.size();
        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (pz_index_ > 0) { pz_index_--; draw(); }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (pz_index_ + 1 < total) { pz_index_++; draw(); }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            // goproblems list: jump to the first problem of the previous/next rank
            if (gp_list_open() && pz_list_.size() == gp_index_.size() && total > 0) {
                int dir = (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 1 : -1;
                int i   = std::max(0, std::min(total - 1, pz_index_));
                int lv  = gp_index_[i].level;
                if (dir > 0) {
                    while (i < total && gp_index_[i].level == lv) i++;
                    if (i >= total) break;
                } else {
                    // start of this level; if already there, start of the one before
                    while (i > 0 && gp_index_[i - 1].level == lv) i--;
                    if (i == pz_index_ && i > 0) {
                        lv = gp_index_[--i].level;
                        while (i > 0 && gp_index_[i - 1].level == lv) i--;
                    }
                }
                pz_index_ = i;
                draw();
                break;
            }
            if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                if (pz_view_ == PzView::COLLECTIONS && !pz_loading_ &&
                    pz_col_page_ * PZ_PAGE_SIZE < pz_col_total_) {
                    pz_col_page_++;
                    pz_index_ = 0;
                    pz_launch_fetch(1, pz_col_page_);
                }
                break;
            }
            if (pz_view_ == PzView::COLLECTIONS && pz_col_page_ > 1 && !pz_loading_) {
                pz_col_page_--;
                pz_index_ = 0;
                pz_launch_fetch(1, pz_col_page_);
            }
            break;
        case SDL_CONTROLLER_BUTTON_A:
            if (pz_loading_ || total == 0 || pz_index_ >= total) break;
            if (pz_view_ == PzView::COLLECTIONS) {
                const auto& c = pz_display_cols_[pz_index_];
                if (c.id < 0) {   // local pin (personal set or imported book) — no fetch
                    if (c.id <= -10) {
                        local_book_ = LocalBook{};   // never fall back on the last one opened
                        for (const auto& b : local_books_)
                            if (b.pin_id == c.id) { local_book_ = b; break; }
                        if (local_book_.dir.empty()) break;   // row outlived its folder
                        open_local_pz_list(LocalPzKind::BOOK);
                        break;
                    }
                    if (c.id == -4) { open_gp_list(); break; }
                    LocalPzKind kind = c.id == -1 ? LocalPzKind::DRILLS
                                     : c.id == -2 ? LocalPzKind::MISSED
                                                  : LocalPzKind::FAVORITES;
                    open_local_pz_list(kind);
                    break;
                }
                if (c.starting_puzzle_id > 0) {
                    local_pz_kind_ = LocalPzKind::NONE;  // entering a real OGS collection
                    drill_paths_.clear();
                    pz_list_title_  = c.name;
                    pz_open_col_id_ = c.id;   // for the solved-mapping backfill
                    // Remember this collection (metadata refresh included) so the
                    // pinned MY SETS section can show it from any page, any session
                    pz_known_cols_[c.id] = c;
                    save_known_collections();
                    pz_launch_fetch(3, c.starting_puzzle_id);
                }
            } else if (local_pz_kind_ != LocalPzKind::NONE) {
                local_pz_load_and_start(pz_index_);
            } else {
                pz_step_dir_ = 0;   // picked by hand — a bad problem just reports
                pz_launch_fetch(2, pz_list_[pz_index_].first);
            }
            break;
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_X:
            if (pz_view_ == PzView::PUZZLES) {
                pz_view_  = PzView::COLLECTIONS;
                pz_index_ = 0;
                local_pz_kind_ = LocalPzKind::NONE;
                pz_rebuild_display();   // solves made in this collection may pin it
                draw();
            } else {
                local_pz_kind_ = LocalPzKind::NONE;
                state_ = AppState::LOBBY;
                set_status("");
                draw();
            }
            break;
        case SDL_CONTROLLER_BUTTON_START:
            open_popup_menu();
            break;
        default: break;
        }
        return;
    }

    // PUZZLE_PLAY
    int n = game_.board_size - 1;
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        game_.cursor_r = std::max(0, game_.cursor_r - 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        game_.cursor_r = std::min(n, game_.cursor_r + 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        game_.cursor_f = std::max(0, game_.cursor_f - 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        game_.cursor_f = std::min(n, game_.cursor_f + 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_A:
        pz_place(game_.cursor_r, game_.cursor_f);
        break;
    case SDL_CONTROLLER_BUTTON_B:
        if (pz_explore_) pz_return_to_solving();  // back to where exploring branched off
        else             pz_start();              // retry from the top
        break;
    case SDL_CONTROLLER_BUTTON_X:
        state_    = AppState::PUZZLE_BROWSE;
        pz_view_  = pz_list_.empty() ? PzView::COLLECTIONS : PzView::PUZZLES;
        pz_index_ = std::max(0, pz_list_pos_);
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        pz_enter_explore();
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        pz_step(-1);
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        pz_step(+1);
        break;
    case SDL_CONTROLLER_BUTTON_START:
        open_popup_menu();
        break;
    default: break;
    }
}

// L3/R3 while reviewing: jump straight to the previous/next SGF in the same
// directory as the open file — cycling marked positions or study puzzles without
// a round trip through the catalog. Files are ordered by name (case-insensitive);
// both the marked/ and puzzles/ naming schemes start with a date+time, so name
// order is chronological order.
std::string App::next_review_sibling(int dir, int* out_index, int* out_total) const {
    if (review_path_.empty()) return "";
    size_t sep = review_path_.find_last_of("/\\");
    if (sep == std::string::npos) return "";
    std::string dir_path = review_path_.substr(0, sep);
    std::string cur_name = review_path_.substr(sep + 1);

    std::vector<std::string> files;
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(
        Catalog::utf8_to_wide(Catalog::join_path(dir_path, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = Catalog::wide_to_utf8(fd.cFileName);
        if (name.size() < 4) continue;
        std::string ext = name.substr(name.size() - 4);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".sgf") files.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir_path.c_str());
    if (!d) return "";
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() < 4) continue;
        std::string ext = name.substr(name.size() - 4);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".sgf") continue;
        struct stat st;
        std::string full = Catalog::join_path(dir_path, name);
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
        files.push_back(name);
    }
    closedir(d);
#endif

    if (files.size() < 2) return "";
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        return str_icmp(a, b) < 0;
    });
    int idx = 0;
    for (int i = 0; i < (int)files.size(); i++)
        if (files[i] == cur_name) { idx = i; break; }
    int next = (idx + dir + (int)files.size()) % (int)files.size();
    if (out_index) *out_index = next + 1;
    if (out_total) *out_total = (int)files.size();
    return Catalog::join_path(dir_path, files[next]);
}

void App::review_cycle(int dir) {
    if (state_ != AppState::GAME_OVER || review_path_.empty()) return;
    int next_idx = 0, total = 0;
    std::string next_path = next_review_sibling(dir, &next_idx, &total);
    if (next_path.empty()) {
        flash_       = "NO OTHER FILES HERE";
        flash_until_ = SDL_GetTicks() + 1500;
        draw();
        return;
    }
    save_companion();  // persist this file's scores/marks before moving on
    load_sgf_for_review(next_path);
    flash_       = "FILE " + std::to_string(next_idx) + "/" + std::to_string(total);
    flash_until_ = SDL_GetTicks() + 1500;
    draw();
}

// Advance analysis_cur_ one ply along the active-child main line — shared by
// the L2/R2 step-forward button handler and the autoplay tick. No-op at a
// leaf (game end) or with no open analysis.
void App::analysis_step_forward() {
    if (!analysis_cur_ || analysis_cur_->children.empty()) return;
    analysis_cur_ = analysis_cur_->children[analysis_cur_->active_child].get();
    build_analysis_tree_render();
    kata_suggestion_count_ = 0;
    kata_score_lead_ = cached_analysis_score(analysis_cur_);
    kata_query_after_ = SDL_GetTicks() + 1000;
}

// ── Guess the move ────────────────────────────────────────────────────────────

void App::toggle_guess_mode() {
    if (!guess_mode_ && !guess_available()) {
        flash_       = "GUESS MODE NEEDS A GAME OPEN FOR REVIEW";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    guess_mode_ = !guess_mode_;
    if (guess_mode_) {
        // Fresh scoreboard per session — a score carried over from an earlier game
        // would be measured against a different number of guesses.
        guess_score_ = 0; guess_count_ = 0;
        guess_last_points_ = -1; guess_last_dist_ = -1;
        set_status(guess_status());
    } else {
        set_status("ANALYSIS — MOVE " + std::to_string(analysis_cur_ ? analysis_cur_->depth : 0));
    }
    close_popup_menu();
    draw();
}

// Score the cursor against the move actually played next, then step onto it so the
// answer is immediately visible on the board.
void App::guess_commit() {
    if (!analysis_cur_ || analysis_cur_->children.empty()) return;
    const AnalysisNode* next = analysis_cur_->children[analysis_cur_->active_child].get();
    // Passes carry no point to aim at, so they are stepped over rather than scored.
    if (next->move_row < 0 || next->move_col < 0) { analysis_step_forward(); draw(); return; }

    int dist = guess_ring_dist(game_.cursor_r, game_.cursor_f, next->move_row, next->move_col);
    guess_last_dist_   = dist;
    guess_last_points_ = guess_points(dist);
    guess_score_      += guess_last_points_;
    guess_count_++;

    analysis_step_forward();
    sound_.play_stone(/*mine=*/true);
    // Leaving the cursor on the guess (rather than snapping it to the answer) keeps
    // the heatmap where the eye already is, so the played stone landing inside or
    // outside those rings is the feedback.
    if (!guess_available()) guess_mode_ = false;   // ran out of moves to guess
    set_status(guess_status());
    draw();
}

// Broken into lines because the status line lays out embedded newlines itself; one
// long line would just shrink the whole block down to fit the gutter width.
std::string App::guess_status() const {
    std::string s = "GUESS THE MOVE";
    if (guess_count_ > 0) {
        int best = guess_count_ * GUESS_MAX_POINTS;
        s += "\n" + std::to_string(guess_score_) + " / " + std::to_string(best) +
             "  (" + std::to_string((guess_score_ * 100 + best / 2) / best) + "%)";
    }
    if (guess_last_points_ > 0)
        s += "\n+" + std::to_string(guess_last_points_) +
             (guess_last_dist_ == 0 ? "  BULLSEYE!" : "  CLOSE");
    else if (guess_last_points_ == 0)
        s += "\nMISS";
    s += guess_mode_ ? "\n" GLYPH_PS_CROSS "=GUESS   G=EXIT" : "\nEND OF GAME";
    return s;
}

void App::load_sgf_for_review(const std::string& path) {
    SgfGame g;
    if (!load_sgf(path, g)) return;

    // Reset game state — no live network, just a board position sequence
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = g.board_size;
    game_.board.board_size = g.board_size;
    game_.board.reset();
    game_.board.turn_is_black = 1;
    game_.cursor_r = game_.cursor_f = g.board_size / 2;
    game_.black_name  = g.black_name;
    game_.white_name  = g.white_name;
    game_.result      = g.result[0] ? g.result : "?";
    game_.my_color    = -1;  // review only — no "my side"
    game_.black_secs  = -1;
    game_.white_secs  = -1;

    // Apply AB[]/AW[] setup stones directly to the board as a single starting
    // position — they're facts about the position, not sequential moves, so no
    // capture logic runs and they collapse into one history entry, not one per stone.
    for (int i = 0; i < g.setup_count; i++) {
        int r, f;
        if (parse_sgf_move(g.moves[i], r, f))
            game_.board.board[r][f] = (g.colors[i] == 1) ? 1 : 2;
    }
    if (g.has_pl) {
        game_.board.turn_is_black = g.start_black;
    } else if (g.setup_count > 0) {
        // No explicit PL[]: fall back to the standard handicap convention —
        // whoever placed the last setup stone, the other color moves first.
        game_.board.turn_is_black = (g.colors[g.setup_count - 1] == 1) ? 0 : 1;
    } else {
        game_.board.turn_is_black = 1;
    }
    game_.history.push_back(game_.board);

    // Replay actual moves: trust the explicit color tags in the SGF
    for (int i = g.setup_count; i < g.move_count; i++) {
        int r, f;
        bool is_black = (g.colors[i] == 1);
        game_.board.turn_is_black = is_black ? 1 : 0;
        if (parse_sgf_move(g.moves[i], r, f)) {
            game_.board.save_snapshot();  // required for KO detection in place_stone
            game_.board.place_stone(r, f, is_black);
        }
        game_.board.turn_is_black = is_black ? 0 : 1;
        game_.history.push_back(game_.board);
    }

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;
    review_komi_ = g.komi;  // from KM[] (with the Fox-Go 375→3.5 guard applied at parse time)

    // Reset score graph / mark storage and derive companion path
    move_scores_.assign(game_.history.size(), FLT_MAX);
    move_marked_.assign(game_.history.size(), false);
    marked_paths_.assign(game_.history.size(), "");
    puzzle_eval_.clear();   // reviews never generate puzzles (my_color = -1),
    puzzle_saved_.clear();  // but don't let live-game leftovers linger either
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    companion_path_    = path.substr(0, path.rfind('.')) + ".katago";
    review_path_       = path;  // enables L3/R3 cycling through sibling files
    guess_mode_        = false; // a new game means a new quiz, never a carried score
    load_companion();   // pre-populate scores and marks if the file exists

    // Build analysis tree from the replayed history
    build_analysis_tree();

    // Enter GAME_OVER analysis mode
    state_ = AppState::GAME_OVER;
    set_status("REVIEW — " + std::string(g.result));
    kata_suggestion_count_ = 0;
    kata_score_lead_ = cached_analysis_score(analysis_cur_);  // instant if the companion file had it
    kata_analysis_enabled_ = false;
}

// ── Network message handler ───────────────────────────────────────────────────

// Is game_ still holding the live OGS game on this screen? Screens that merely
// overlay or replace the view (settings, lobby lists, the who-won quiz) leave it
// alone, so a live move can be applied underneath them; anything that replays a
// file, puzzle or joseki position does so into game_ itself, and a live move
// applied there would land on the wrong board. Default false: a screen added
// later degrades to a resync, never to a silently dropped move.
static bool state_keeps_live_board(AppState s) {
    switch (s) {
    case AppState::PLAYING:
    case AppState::MATCH_MENU:
    case AppState::LOBBY:
    case AppState::SEARCHING:
    case AppState::CORR_LIST:
    case AppState::CHALLENGES:
    case AppState::CHALLENGE_CREATE:
    case AppState::WHO_WON:
        return true;
    default:
        return false;
    }
}

void App::handle_net_msg(const NetMsg& msg) {
    switch (msg.type) {
    case NetMsgType::AUTH_OK:
        state_ = AppState::LOBBY;
        set_status("");
        // Persist a prompt login now that the server has accepted it, so the next
        // launch auto-connects. Only for prompt-entered creds (config logins are
        // already saved), and only after AUTH_OK so a wrong password is never stored.
        if (cred_from_prompt_) {
            save_credentials(cred_username_, cred_password_);
            cred_from_prompt_ = false;
            flash_       = "LOGIN SAVED";
            flash_until_ = SDL_GetTicks() + 3000;
        }
        load_demo_game();
        draw();
        break;

    case NetMsgType::AUTH_FAIL:
        // Reset to credential prompt so the user can re-enter
        state_    = AppState::CREDENTIAL_PROMPT;
        cred_step_ = 1;
        cred_username_.clear();
        cred_password_.clear();
        cred_buf_.clear();
        flash_       = "Login failed: " + msg.text;
        flash_until_ = SDL_GetTicks() + 4000;
        draw();
        break;

    case NetMsgType::MATCH_FOUND:
        is_corr_game_ = false;  // an automatch game is live, not from the corr list
        set_status("MATCH FOUND — CONNECTING...");
        draw();
        break;

    case NetMsgType::CORR_LIST_UPDATED:
        corr_loading_ = false;
        corr_games_   = msg.corr_games;
        if (state_ == AppState::CORR_LIST) {
            if (corr_index_ >= (int)corr_games_.size())
                corr_index_ = std::max(0, (int)corr_games_.size() - 1);
            // msg.text is set only on fetch/parse failure — surface it if present.
            set_status(msg.text.empty() ? "MY GAMES" : msg.text);
            draw();
        }
        break;

    case NetMsgType::CHALLENGES_UPDATED:
        chal_loading_ = false;
        challenges_   = msg.challenges;
        if (state_ == AppState::CHALLENGES) {
            if (chal_index_ >= (int)challenges_.size())
                chal_index_ = std::max(0, (int)challenges_.size() - 1);
            chal_accept_confirm_  = false;   // list changed — cancel any armed confirm
            chal_decline_confirm_ = false;
            set_status(msg.text.empty() ? "CHALLENGES" : msg.text);
            draw();
        }
        break;

    case NetMsgType::CHALLENGE_RESULT:
        // Accept/decline/send outcome — flash it; the follow-up CHALLENGES_UPDATED
        // (from the re-fetch) refreshes the list, and an accepted game arrives via
        // active_game.
        flash_       = msg.text;
        flash_until_ = SDL_GetTicks() + 3000;
        if (state_ == AppState::CHALLENGE_CREATE) set_status("NEW CHALLENGE");
        if (state_ == AppState::CHALLENGES || state_ == AppState::CHALLENGE_CREATE) draw();
        break;

    case NetMsgType::FRIENDS_UPDATED:
        friends_loading_ = false;
        friends_         = msg.friends;
        // A shorter list can strand the opponent cursor past the end.
        if (cc_opp_ > (int)friends_.size()) cc_opp_ = 0;
        if (state_ == AppState::CHALLENGE_CREATE) {
            if (!msg.text.empty()) set_status(msg.text);   // fetch failure
            draw();
        }
        break;

    case NetMsgType::GAME_CONNECTED: {
        game_.result.clear();
        game_.pending_col  = -2;
        game_.pending_row  = -2;
        game_.history.clear();
        game_.history_pos  = -1;
        memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
        memset(game_.ownership,   0, sizeof(game_.ownership));
        live_own_pending_     = false;
        live_own_query_after_ = 0;
        kata_score_lead_      = FLT_MAX;  // stale score from a previous game/analysis
        game_.game_id      = msg.game_id;
        game_.board_size   = msg.board_size;
        game_.my_color     = msg.my_color;
        game_.my_player_id = msg.my_player_id;
        game_.black_name = msg.black_name;
        game_.white_name = msg.white_name;
        game_.black_rank = msg.black_rank;
        game_.white_rank = msg.white_rank;
        // Real player names. The opponent's rank stays hidden while the game is
        // live — play the board, not the rating — and is revealed at game over.
        black_label_ = msg.black_name;
        white_label_ = msg.white_name;
        if (msg.my_color == 1 && !msg.black_rank.empty())
            black_label_ += " [" + msg.black_rank + "]";
        if (msg.my_color == 0 && !msg.white_rank.empty())
            white_label_ += " [" + msg.white_rank + "]";
        game_.black_secs        = msg.black_secs;
        game_.white_secs        = msg.white_secs;
        game_.black_periods     = msg.black_periods;
        game_.white_periods     = msg.white_periods;
        game_.black_period_secs = msg.black_period_secs;
        game_.white_period_secs = msg.white_period_secs;
        game_.black_in_byo      = msg.black_in_byo;
        game_.white_in_byo      = msg.white_in_byo;
        game_.clock_tick        = SDL_GetTicks();
        // Reconnect: the running player's countdown is genuinely mid-turn, but
        // the parked player's stored value is their last turn's leftover.
        reset_byo_countdowns(/*running_player_too=*/false);
        game_.cursor_r     = msg.board_size / 2;
        game_.cursor_f     = msg.board_size / 2;
        game_.board.reset();
        game_.board.board_size = msg.board_size;
        game_.handicap      = msg.handicap;
        game_.free_handicap = msg.free_handicap;

        // Snapshot empty board as history[0] (before any stones)
        game_.history.push_back(game_.board);

        // Pre-placed handicap stones (non-free handicap): always black, don't alter turn
        for (auto& [col, row] : msg.initial_handicap_stones)
            apply_move(col, row, 1);
        // Now set who plays the first real move
        game_.board.turn_is_black = msg.initial_player;

        // Apply moves from reconnect / ongoing game
        for (auto& [col, row] : msg.initial_moves)
            apply_move(col, row);

        // Parity anchor + resync bookkeeping for the live move handler: who opened,
        // how many moves the server has numbered so far, and a cleared resync timer.
        // This runs on every gamedata (including a resync rebuild), so it re-anchors
        // authoritatively each time.
        game_.initial_player     = msg.initial_player;
        game_.last_move_number   = (int)msg.initial_moves.size();
        game_.resync_req_at      = 0;
        game_.server_turn_player = msg.current_player;  // 0 if the gamedata had no clock
        game_.turn_mismatch_at   = 0;

        // Determine whose turn it is
        bool black_to_play = (game_.board.turn_is_black == 1);
        game_.my_turn = (black_to_play && game_.my_color == 1) ||
                        (!black_to_play && game_.my_color == 0);

        // Initialise score-graph / mark storage. assign(), not resize() — resize
        // never shrinks and keeps old values, which leaked the previous game's
        // scores into this one whenever the previous game was longer.
        move_scores_.assign(game_.history.size(), FLT_MAX);
        move_marked_.assign(game_.history.size(), false);
        marked_paths_.assign(game_.history.size(), "");
        puzzle_eval_.clear();
        puzzle_saved_.clear();
        review_path_.clear();  // this GAME_OVER will be a live game's, not a loaded file
        bg_analysis_next_  = 0;
        bg_analysis_depth_ = -1;
        bg_analysis_busy_  = false;
        review_komi_       = 7.5f;  // OGS standard komi; not read from server game data here

        state_ = AppState::PLAYING;
        close_popup_menu();  // a SEARCHING popup is stale once the game starts
        sound_.play_game_start();
        set_status(game_.my_turn ? "YOUR TURN" : "WAITING...");
        draw();
        break;
    }

    case NetMsgType::OPPONENT_MOVE:
        // Which screen is showing must never decide whether the board tracks the
        // server. This used to be `if (state_ != PLAYING) break;`, so a move that
        // arrived while the settings menu / a list screen / the quiz was up was
        // dropped outright — and nothing recovered it, because the sequence check
        // below only trips on a *later* move. A handicap game is where that bites:
        // white opens, so you are waiting at move 0 with an untouched board — the
        // moment you are most likely to be off the game screen — and the move lost
        // is the only one coming. The board then sits on "WAITING..." until you
        // lose on time (G0GOGO, 2026-07-27).
        // Trace every decision this handler makes: the log used to show only what
        // came off the wire, so a move that arrived and was then quietly discarded
        // looked exactly like one that landed on the board.
        net_.log_line("OPPONENT_MOVE mn=" + std::to_string(msg.move_number) +
                      " (" + std::to_string(msg.col) + "," + std::to_string(msg.row) + ")" +
                      " gid=" + std::to_string(msg.game_id) +
                      " state=" + std::to_string((int)state_) +
                      " last=" + std::to_string(game_.last_move_number) +
                      " pending=" + std::to_string(game_.pending_col));
        if (is_local_game_ || msg.game_id != game_.game_id) break;
        if (!state_keeps_live_board(state_)) {
            // Scoring, or this game's own post-game screen (review_path_ empty means
            // the review is the live game, not a file loaded over it): a move event
            // here is stale, not a gap. Otherwise game_ has been handed to a catalog
            // file while the game is genuinely still live — the move can't be applied
            // here, so pull the position back instead and let GAME_CONNECTED rebuild.
            bool post_game = (state_ == AppState::STONE_REMOVAL ||
                              (state_ == AppState::GAME_OVER && review_path_.empty()));
            if (!post_game) request_resync();
            break;
        }
        {
            // Update clocks regardless of whose move it was
            if (msg.black_secs >= 0) {
                game_.black_secs        = msg.black_secs;
                game_.black_periods     = msg.black_periods;
                game_.black_period_secs = msg.black_period_secs;
                game_.black_in_byo      = msg.black_in_byo;
                game_.clock_tick = SDL_GetTicks();
            }
            if (msg.white_secs >= 0) {
                game_.white_secs        = msg.white_secs;
                game_.white_periods     = msg.white_periods;
                game_.white_period_secs = msg.white_period_secs;
                game_.white_in_byo      = msg.white_in_byo;
            }

            // Authoritative colour of move `mn` (1-indexed) from the opening player
            // and handicap scheme — never the running toggle, so it can't drift.
            // Free-handicap placements are all black; afterwards colours alternate,
            // starting white for any handicap game and with initial_player otherwise.
            auto move_is_black = [&](int mn) -> bool {
                int h = game_.free_handicap ? game_.handicap : 0;
                if (mn <= h) return true;
                bool first_black = (h > 0) ? false : (game_.initial_player == 1);
                return ((mn - h) % 2 == 1) ? first_black : !first_black;
            };

            // Decide what this event is from its number and coordinates, not by
            // blindly trusting the optimistic pending flag. The old coordinate-blind
            // "a move arrived while pending is set, so it must be my echo" match
            // silently swallowed opponent moves and inverted every colour after that
            // point (the ded71 desync, 2026-07-20).
            bool is_my_echo = (game_.pending_col != -2) &&
                              msg.col == game_.pending_col && msg.row == game_.pending_row;

            // Duplicate / stale re-send of a move we already applied — ignore it.
            if (msg.move_number > 0 && msg.move_number <= game_.last_move_number) {
                net_.log_line("  -> ignored as duplicate");
                break;
            }

            // Anything that isn't the exact next move — a gap (a move we never got)
            // or our optimistic move the server never echoed (rejected/superseded, so
            // the pending coords don't match what arrived) — means the local board has
            // drifted. Re-request gamedata; GAME_CONNECTED rebuilds the whole position
            // authoritatively. Debounced so a flurry of out-of-sequence events can't
            // spam game/connect, and self-retrying so a dropped rebuild can't freeze us.
            bool sequential    = (msg.move_number == 0) ||
                                 (msg.move_number == game_.last_move_number + 1);
            bool stale_pending = (game_.pending_col != -2) && !is_my_echo;
            if (!sequential || stale_pending) {
                request_resync();
                break;
            }

            if (is_my_echo) {
                // Server echo of our own move — already applied optimistically, just clear.
                game_.pending_col = -2;
                game_.pending_row = -2;
            } else {
                // A move to place ourselves: the opponent's, or our own made on another
                // device. Anchor the toggle to the authoritative colour before applying,
                // so a prior drift is corrected here and the stone lands the right colour.
                if (msg.move_number > 0)
                    game_.board.turn_is_black = move_is_black(msg.move_number) ? 1 : 0;
                if (msg.col >= 0) {
                    play_move_sound(/*mine=*/false, apply_move(msg.col, msg.row));
                } else {
                    apply_pass();
                    flash_       = "OPPONENT PASSED";
                    flash_until_ = SDL_GetTicks() + 3000;
                }
                request_live_ownership();
            }
            if (msg.move_number > 0) game_.last_move_number = msg.move_number;

            // Whose turn now — read the toggle apply_move/apply_pass just advanced,
            // which we anchored from the authoritative move number above.
            {
                bool next_is_black = (game_.board.turn_is_black == 1);
                game_.my_turn = (next_is_black == (game_.my_color == 1));
            }
            pass_confirm_ = false;
            game_.turn_mismatch_at = 0;   // board and server just agreed again
            set_status(game_.my_turn ? "YOUR TURN" : "WAITING...");
            // A move was just played: both byo-yomi countdowns start a fresh
            // period, whatever leftover the event's clock snapshot carried.
            reset_byo_countdowns(/*running_player_too=*/true);
            draw();
        }
        break;

    case NetMsgType::CLOCK_UPDATE:
        if (msg.black_secs >= 0) {
            game_.black_secs        = msg.black_secs;
            game_.black_periods     = msg.black_periods;
            game_.black_period_secs = msg.black_period_secs;
            game_.black_in_byo      = msg.black_in_byo;
            game_.clock_tick = SDL_GetTicks();
        }
        if (msg.white_secs >= 0) {
            game_.white_secs        = msg.white_secs;
            game_.white_periods     = msg.white_periods;
            game_.white_period_secs = msg.white_period_secs;
            game_.white_in_byo      = msg.white_in_byo;
        }
        // Whose move the server thinks it is — the watchdog in the event loop
        // compares this against my_turn to notice a live move we never got.
        if (msg.current_player > 0) game_.server_turn_player = msg.current_player;
        // No move here — trust the running player's countdown (it's genuinely
        // in progress), but the parked player's value is stale leftover.
        reset_byo_countdowns(/*running_player_too=*/false);
        draw();
        break;

    case NetMsgType::STONE_REMOVAL: {
        state_ = AppState::STONE_REMOVAL;
        close_popup_menu();  // a GAME MENU popup is stale once scoring starts
        stone_removal_has_ogs_territory_ = false;  // reset; will be set once OGS sends territory
        game_.history_pos = -1;  // always show live board during stone removal
        // Decode dead stones from all_removed string (pairs of chars: col, row, 'a'=0)
        memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
        memset(game_.ownership,   0, sizeof(game_.ownership));
        const std::string& ar = msg.text;
        for (size_t i = 0; i + 1 < ar.size(); i += 2) {
            int col = ar[i]   - 'a';
            int row = ar[i+1] - 'a';
            if (row >= 0 && row < game_.board_size && col >= 0 && col < game_.board_size)
                game_.dead_stones[row][col] = true;
        }
        // Decode ownership 2D array from OGS — treat as authoritative once received
        if (!msg.ownership_json.empty()) {
            stone_removal_has_ogs_territory_ = true;
            try {
                auto ow = nlohmann::json::parse(msg.ownership_json);
                if (ow.is_array()) {
                    for (int r = 0; r < (int)ow.size() && r < game_.board_size; r++) {
                        auto& row_arr = ow[r];
                        if (!row_arr.is_array()) continue;
                        for (int f = 0; f < (int)row_arr.size() && f < game_.board_size; f++)
                            game_.ownership[r][f] = row_arr[f].get<int>();
                    }
                }
            } catch (...) {}
        }
        // If we already accepted this exact dead-stone set, re-send and keep ACCEPTING status.
        // Only reset if the stone set actually changed (opponent modified the markings).
        if (my_accept_sent_ && msg.text == stone_removal_all_removed_) {
            net_.cmd_accept_stones(game_.game_id);
            accept_resend_at_ = SDL_GetTicks() + 6000;
            set_status("ACCEPTING... WAITING FOR OPPONENT");
        } else {
            my_accept_sent_ = false;
            set_status(stone_removal_has_ogs_territory_
                           ? "PRESS " GLYPH_PS_CROSS " TO ACCEPT DEAD STONES"
                           : "WAITING FOR SERVER SCORE...");
        }
        stone_removal_all_removed_ = msg.text;
        draw();
        break;
    }

    case NetMsgType::GAME_OVER:
        state_ = AppState::GAME_OVER;
        close_popup_menu();  // popup items built for PLAYING/STONE_REMOVAL are stale now
        review_komi_ = 7.5f;  // OGS standard komi; not read from server game data here
        game_.result = msg.text;
        // Game's over — reveal the opponent's rank (hidden during live play)
        black_label_ = game_.black_name +
                       (game_.black_rank.empty() ? "" : " [" + game_.black_rank + "]");
        white_label_ = game_.white_name +
                       (game_.white_rank.empty() ? "" : " [" + game_.white_rank + "]");
        save_live_game();
        // Scan for auto study puzzles now that the game is over (positions the
        // background sweep hasn't reached yet get checked as their results arrive)
        for (int d = 0; d + 1 < (int)game_.history.size(); d++) check_puzzle(d);
        set_status("GAME OVER — " + msg.text);
        kata_suggestion_count_ = 0;
        build_analysis_tree();  // populates analysis_root_ and analysis_cur_
        kata_score_lead_ = cached_analysis_score(analysis_cur_);  // instant if already scored during play
        kata_for(game_.board_size).query_moves(
            analysis_cur_ ? analysis_cur_->board.board : game_.board.board,
            game_.board_size,
            analysis_cur_ ? analysis_cur_->board.turn_is_black == 1
                          : game_.board.turn_is_black == 1);
        draw();
        break;

    case NetMsgType::UNDO_REQUESTED:
        if (state_ == AppState::PLAYING) {
            undo_pending_     = true;
            undo_move_number_ = msg.undo_move_number;
            draw();
        }
        break;

    case NetMsgType::RESUME_PLAY:
        if (state_ == AppState::STONE_REMOVAL || state_ == AppState::PLAYING) {
            stone_removal_has_ogs_territory_ = false;
            stone_removal_fill_view_ = false;
            my_accept_sent_ = false;
            stone_removal_all_removed_.clear();
            state_ = AppState::PLAYING;
            memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
            memset(game_.ownership,   0, sizeof(game_.ownership));
            game_.history_pos = -1;
            undo_pending_     = false;
            pass_confirm_     = false;
            close_popup_menu();  // a SCORING popup is stale once play resumes
            // If we had a pending pass that the server rejected (phase reverted to play),
            // undo the optimistic turn flip and the history snapshot apply_pass() recorded
            // for it, so my_turn and history depth are computed from the correct state.
            if (game_.pending_col == -1 && game_.pending_row == -1) {
                game_.board.turn_is_black = !game_.board.turn_is_black;
                if (!game_.history.empty()) game_.history.pop_back();
                if (!move_scores_.empty()) {
                    move_scores_.pop_back();
                    move_marked_.pop_back();
                    marked_paths_.pop_back();
                }
            }
            game_.pending_col = -2;
            game_.pending_row = -2;
            bool btp = (game_.board.turn_is_black == 1);
            game_.my_turn = (btp && game_.my_color == 1) || (!btp && game_.my_color == 0);
            set_status(game_.my_turn ? "YOUR TURN" : "WAITING...");
            draw();
        }
        break;

    case NetMsgType::DISCONNECTED:
        // Neither a local game vs KataGo nor GAME_OVER analysis review (regardless of
        // whether the reviewed game was local, online, or loaded from the catalog) has
        // any live dependency on the OGS websocket — KataGo is a local subprocess and
        // review is just walking data already in memory/on disk. Don't yank the player
        // out of either just because the background lobby connection dropped; note it
        // and leave everything undisturbed.
        if (is_local_game_ || state_ == AppState::GAME_OVER) {
            flash_       = "OGS CONNECTION LOST (unaffected)";
            flash_until_ = SDL_GetTicks() + 3000;
            draw();
            break;
        }
        analysis_root_.reset();
        analysis_cur_ = nullptr;
        analysis_tree_render_.clear();
        kata_suggestion_count_ = 0;
        kata_score_lead_ = FLT_MAX;
        kata_query_after_ = 0;
        close_popup_menu();
        pass_confirm_ = false;
        mark_confirm_ = false;
        find_match_confirm_ = false;
        state_ = AppState::CONNECTING;
        // The network thread is already retrying with backoff, so say what is
        // actually happening. "DISCONNECTED" was a dead end: nothing reconnected,
        // and no key on this screen could.
        flash_       = msg.text;
        flash_until_ = SDL_GetTicks() + 4000;
        set_status("RECONNECTING...");
        draw();
        break;

    case NetMsgType::RECONNECTED:
        // Back up. A live game rebuilds itself: the network thread re-sent
        // game/connect on this same socket, so GAME_CONNECTED is on its way and
        // will restore the board, clocks, move number and PLAYING state
        // authoritatively — including anything that happened during the outage.
        // Don't touch state_ unless the disconnect parked us in CONNECTING, and
        // don't route through AUTH_OK's lobby entry, which would evict a live game.
        flash_       = "RECONNECTED";
        flash_until_ = SDL_GetTicks() + 3000;
        if (state_ == AppState::CONNECTING) {
            if (msg.game_id != 0) {
                set_status("RESYNCING...");   // GAME_CONNECTED lands us back in PLAYING
            } else {
                state_ = AppState::LOBBY;
                set_status("");
                load_demo_game();
            }
        }
        draw();
        break;
    }
}

// ── Drawing ───────────────────────────────────────────────────────────────────

Renderer::DrawState App::make_ds() {
    // Clocks: count down in real-time based on SDL_GetTicks() delta
    int b_secs    = game_.black_secs;
    int w_secs    = game_.white_secs;
    int b_periods = game_.black_periods;
    int w_periods = game_.white_periods;
    // "In byo-yomi" can't be derived from the displayed numbers alone (a period
    // countdown of 0:27 x5 looks identical to 27s of main time with 5 periods
    // banked) — use the flags the network layer determined at parse time, plus
    // the legacy secs<=0 heuristic as a safety net.
    bool b_in_byo = game_.black_in_byo || (game_.black_secs <= 0 && game_.black_periods > 0);
    bool w_in_byo = game_.white_in_byo || (game_.white_secs <= 0 && game_.white_periods > 0);
    // Byo-yomi periods reset the instant a move is played, so the player NOT to
    // move always has a full period waiting. Without this, the clock froze at
    // whatever the server last reported mid-period (e.g. stuck showing 0:24 after
    // using 6s of a 30s period) until it became that player's turn again.
    bool black_to_move = (game_.board.turn_is_black == 1);
    if (b_in_byo && !black_to_move && game_.black_period_secs > 0)
        b_secs = game_.black_period_secs;
    if (w_in_byo && black_to_move && game_.white_period_secs > 0)
        w_secs = game_.white_period_secs;
    if (state_ == AppState::PLAYING && game_.clock_tick > 0) {
        int elapsed = (int)((SDL_GetTicks() - game_.clock_tick) / 1000);
        auto tick = [](int secs, int periods, int period_secs, int elapsed,
                       int& out_secs, int& out_periods, bool& out_in_byo) {
            if (secs > 0 && elapsed < secs) {
                // Still within the period_time_left (or main time) window.
                out_secs    = secs - elapsed;
                out_periods = periods;
            } else if (periods > 0 && period_secs > 0) {
                // Byo-yomi counting. Two entry paths:
                //   a) secs == 0: server sent thinking_time=0 at period start.
                //   b) secs > 0: period_time_left just ran out; the current period
                //      (included in `periods`) is now consumed — use periods-1.
                int byo_elapsed = (secs > 0) ? elapsed - secs : elapsed;
                int byo_periods = (secs > 0) ? periods - 1   : periods;
                int lost        = byo_elapsed / period_secs;
                out_periods     = std::max(0, byo_periods - lost);
                out_secs        = out_periods > 0
                                    ? period_secs - byo_elapsed % period_secs
                                    : 0;
                out_in_byo      = true;
            } else {
                out_secs    = std::max(0, secs - elapsed);
                out_periods = periods;
            }
        };
        if (game_.board.turn_is_black == 1)
            tick(game_.black_secs, game_.black_periods, game_.black_period_secs, elapsed,
                 b_secs, b_periods, b_in_byo);
        else if (game_.board.turn_is_black == 0)
            tick(game_.white_secs, game_.white_periods, game_.white_period_secs, elapsed,
                 w_secs, w_periods, w_in_byo);
    }

    // GAME_OVER is navigated through the analysis tree; PLAYING/STONE_REMOVAL/
    // PUZZLE_PLAY (while exploring) use flat history.
    bool in_history = (state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL ||
                        state_ == AppState::PUZZLE_PLAY)
                      && (game_.history_pos >= 0 && !game_.history.empty());

    const char* status_cstr = status_.empty() ? nullptr : status_.c_str();
    if (pass_confirm_)   status_cstr = "PRESS " GLYPH_PS_CIRCLE " AGAIN TO PASS";
    if (undo_pending_)   status_cstr = "UNDO REQUEST: " GLYPH_PS_CROSS "=ACCEPT  " GLYPH_PS_CIRCLE "=DENY";
    if (in_history) {
        hist_status_ = "MOVE " + std::to_string(game_.history_pos) + "/" +
                       std::to_string((int)game_.history.size() - 1);
        status_cstr = hist_status_.c_str();
    }
    if (state_ == AppState::GAME_OVER && analysis_cur_ && !guess_mode_) {
        hist_status_ = "ANALYSIS - MOVE " + std::to_string(analysis_cur_->depth);
        if (analysis_cur_->children.size() > 1)
            hist_status_ += " [FORK]";
        status_cstr = hist_status_.c_str();
    }
    if (state_ == AppState::STONE_REMOVAL && !in_history) {
        hist_status_ = status_cstr ? status_cstr : "";
        if (!hist_status_.empty()) hist_status_ += "\n";
        hist_status_ += stone_removal_fill_view_
            ? GLYPH_PS_TRIANGLE "=SQUARE VIEW"
            : GLYPH_PS_TRIANGLE "=FILL VIEW";
        status_cstr = hist_status_.c_str();
    }
    // Theme abbreviated shoulder/trigger/menu labels (L1, R3, OPT...) in the status
    // to the connected pad. Face-button glyphs are themed later by draw_text. No-op
    // (a plain copy) for the PlayStation default.
    if (status_cstr) {
        themed_status_ = Renderer::themed_labels(status_cstr);
        status_cstr    = themed_status_.c_str();
    }

    bool playing = (state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL ||
                    state_ == AppState::GAME_OVER || state_ == AppState::PUZZLE_PLAY ||
                    state_ == AppState::JOSEKI);
    bool live    = (state_ != AppState::CREDENTIAL_PROMPT);

    // When reviewing history, display the historical board state;
    // in GAME_OVER display the current analysis node;
    // otherwise in idle states show the demo screensaver game if loaded.
    bool who_won   = (state_ == AppState::WHO_WON);
    bool show_demo = demo_active_ && !playing && !who_won &&
                     state_ != AppState::CREDENTIAL_PROMPT;
    const GameState& disp_board =
        who_won                                           ? wq_board_
        : (state_ == AppState::GAME_OVER && analysis_cur_) ? analysis_cur_->board
        : in_history                                      ? game_.history[game_.history_pos]
        : show_demo                                       ? demo_.board
        :                                                   game_.board;

    // References bound here must outlive this call — all are App member variables.
    // In the quiz the players stay hidden until the guess is in: seeing a famous
    // name next to the board is a hint, and the point is to read the position.
    const std::string& bname = who_won    ? (wq_answered_ ? wq_black_ : empty_str_)
                             : playing    ? black_label_
                             : show_demo  ? demo_.black_name
                             :              empty_str_;
    const std::string& wname = who_won    ? (wq_answered_ ? wq_white_ : empty_str_)
                             : playing    ? white_label_
                             : show_demo  ? demo_.white_name
                             :              empty_str_;

    int active_bs = who_won   ? wq_bs_
                  : playing   ? game_.board_size
                  : show_demo ? demo_.board_size
                  :             BOARD_SIZE;

    return Renderer::DrawState{
        .game                   = disp_board,
        .analysis               = nullptr,
        .analysis_mode          = false,
        .game_mode              = false,
        .guess_mode             = guess_mode_,
        .guess_score            = guess_score_,
        .guess_count            = guess_count_,
        .guess_last_points      = guess_last_points_,
        .guess_last_dist        = guess_last_dist_,
        .guess_heat_r           = guess_mode_ ? game_.cursor_r : -1,
        .guess_heat_f           = guess_mode_ ? game_.cursor_f : -1,
        .chain_mode             = chain_mode_,
        // The quiz owns the whole screen: a bare board with no HUD, and its own
        // overlay panel drawn beside it.
        .free_mode              = who_won,
        .active_board_size      = active_bs,
        .show_help              = show_help_,
        .catalog                = catalog_,
        .catalog_readonly       = catalog_readonly_,
        .draw_mode              = draw_mode_,
        .draw_dark              = chalk_dark_,
        .black_name             = bname,
        .white_name             = wname,
        .result_message         = who_won ? (wq_answered_ ? wq_result_ : empty_str_)
                                          : (state_ == AppState::GAME_OVER) ? game_.result
                                                                            : empty_str_,
        .game_date              = (who_won && wq_answered_) ? wq_date_ : empty_str_,
        .game_comment           = (state_ == AppState::PUZZLE_PLAY) ? pz_comment_
                                : (state_ == AppState::JOSEKI)      ? jk_comment_ : empty_str_,
        .move_delay_ms          = MOVE_DELAY_MS,
        .speed_message_until    = 0,
        .suppress_present       = false,
        .territory_drill        = false,
        .territory_board        = nullptr,
        .territory_b_score      = 0,
        .territory_w_score      = 0,
        .territory_answered     = false,
        .territory_correct      = false,
        .who_won_active         = who_won,
        .who_won_answered       = wq_answered_,
        .who_won_correct        = wq_correct_,
        .who_won_right          = wq_right_,
        .who_won_total          = wq_total_,
        .stone_filter           = 0,
        // Drawing mode replaces the OS pointer with a chalk-tinted crosshair, so the
        // cursor itself shows that a click draws and which colour is armed (it used
        // to be a text label, which collided with the status line). Off in every
        // other state, where the OS cursor is the pointer.
        .cursor_x               = draw_mode_ ? mouse_x_ : -1,
        .cursor_y               = draw_mode_ ? mouse_y_ : -1,
        .cursor_type            = draw_mode_ ? 1 : 0,
        .cursor_color           = chalk_dark_ ? Palette::CHALK_DARK : Palette::CHALK,
        .show_move_numbers      = false,
        .sgf_moves              = nullptr,
        .sgf_colors             = nullptr,
        .sgf_game_index         = 0,
        .analysis_num_grid      = nullptr,
        .analysis_col_grid      = nullptr,
        .quit_confirm           = quit_confirm_,
        .box_sel_pts            = nullptr,
        .box_sel_count          = 0,
        .box_drag_active        = false,
        .box_drag_r1            = 0,
        .box_drag_f1            = 0,
        .box_drag_r2            = 0,
        .box_drag_f2            = 0,
        .catalog_thumb_valid    = thumb_valid_,
        .catalog_thumb_single   = thumb_single_,
        .catalog_thumb_open     = thumb_valid_ ? thumb_open_  : nullptr,
        .catalog_thumb_final    = thumb_valid_ ? thumb_final_ : nullptr,
        .catalog_thumb_board_size = thumb_board_size_,
        .flash_message          = flash_,
        .flash_message_until    = flash_until_,
        .save_input_step        = 0,
        .save_input_buf         = empty_str_,

        // Live fields
        .live_mode       = live,
        .live_cursor_r   = (state_ == AppState::PLAYING && !in_history) ? game_.cursor_r :
                            (state_ == AppState::GAME_OVER)              ? game_.cursor_r :
                            (state_ == AppState::PUZZLE_PLAY)            ? game_.cursor_r :
                            (state_ == AppState::JOSEKI)                 ? game_.cursor_r : -1,
        .live_cursor_f   = (state_ == AppState::PLAYING && !in_history) ? game_.cursor_f :
                            (state_ == AppState::GAME_OVER)              ? game_.cursor_f :
                            (state_ == AppState::PUZZLE_PLAY)            ? game_.cursor_f :
                            (state_ == AppState::JOSEKI)                 ? game_.cursor_f : -1,
        .live_my_color   = game_.my_color,
        .live_my_turn    = game_.my_turn,
        .live_black_secs        = playing ? b_secs : -1,
        .live_white_secs        = playing ? w_secs : -1,
        .live_black_periods     = playing ? b_periods : -1,
        .live_white_periods     = playing ? w_periods : -1,
        .live_black_period_secs = playing ? game_.black_period_secs : -1,
        .live_white_period_secs = playing ? game_.white_period_secs : -1,
        .live_black_in_byo      = playing && b_in_byo,
        .live_white_in_byo      = playing && w_in_byo,
        .live_status     = live ? status_cstr : nullptr,
        // The screensaver's end-of-game fill reuses the stone-removal fill view
        // wholesale — same ownership map, same "dead stones count as the other
        // side's territory" rule, so the final position of a catalog game reads
        // exactly like the scored board of one of the user's own.
        .live_dead_stones     = (state_ == AppState::STONE_REMOVAL) ? game_.dead_stones
                              : (show_demo && demo_.fill_ready)     ? demo_.dead
                              :                                       nullptr,
        .live_ownership       = (state_ == AppState::STONE_REMOVAL ||
                                  (state_ == AppState::PLAYING && show_live_ownership_))
                                                                    ? game_.ownership
                              : (show_demo && demo_.fill_ready)     ? demo_.ownership
                              :                                       nullptr,
        .live_ownership_fill  = ((state_ == AppState::STONE_REMOVAL) && stone_removal_fill_view_) ||
                                (show_demo && demo_.fill_ready),
        .live_area_black      = (show_demo && demo_.fill_ready) ? demo_.area_black : -1,
        .live_area_white      = (show_demo && demo_.fill_ready) ? demo_.area_white : -1,
        .live_suggestions     = (state_ == AppState::GAME_OVER && kata_suggestion_count_ > 0)
                                     ? kata_suggestions_ : nullptr,
        .live_suggestion_count = (state_ == AppState::GAME_OVER) ? kata_suggestion_count_ : 0,
        .live_hovered_suggestion = [&]() -> int {
            if (state_ != AppState::GAME_OVER) return -1;
            for (int i = 0; i < std::min(kata_suggestion_count_, 3); i++)
                if (kata_suggestions_[i].row == game_.cursor_r &&
                    kata_suggestions_[i].col == game_.cursor_f) return i;
            return -1;
        }(),
        .live_cursor_ko       = (ko_flash_until_ > SDL_GetTicks()),
        .live_kata_score_lead    = (state_ == AppState::GAME_OVER ||
                                     (state_ == AppState::PLAYING && show_live_ownership_))
                                        ? kata_score_lead_ : FLT_MAX,
        // Both the yellow "actual move" marker and the tree panel show the answer
        // to what's being guessed, so guess mode blanks them out same as it would
        // for a player who shouldn't get to peek. The marker is additionally off
        // unless SHOW NEXT MOVE is ticked — same spoiler, just always rather than
        // only in guess mode.
        .live_actual_move_r = [&]() -> int {
            if (state_ != AppState::GAME_OVER || guess_mode_ || !show_next_move_ ||
                !analysis_cur_ || analysis_cur_->children.empty())
                return -1;
            return analysis_cur_->children[0]->move_row;
        }(),
        .live_actual_move_f = [&]() -> int {
            if (state_ != AppState::GAME_OVER || guess_mode_ || !show_next_move_ ||
                !analysis_cur_ || analysis_cur_->children.empty())
                return -1;
            return analysis_cur_->children[0]->move_col;
        }(),
        .live_actual_move_score = [&]() -> float {
            if (state_ != AppState::GAME_OVER || guess_mode_ || !show_next_move_ ||
                !analysis_cur_ || analysis_cur_->children.empty())
                return FLT_MAX;
            int ar = analysis_cur_->children[0]->move_row;
            int af = analysis_cur_->children[0]->move_col;
            if (ar < 0 || af < 0) return FLT_MAX;
            for (int i = 0; i < kata_suggestion_count_; i++)
                if (kata_suggestions_[i].row == ar && kata_suggestions_[i].col == af)
                    return kata_suggestions_[i].score_lead;
            return FLT_MAX;
        }(),
        .live_analysis_tree       = (state_ == AppState::GAME_OVER && !guess_mode_ &&
                                      !analysis_tree_render_.empty())
                                         ? analysis_tree_render_.data()
                                  : (state_ == AppState::PUZZLE_PLAY && !pz_tree_render_.empty())
                                         ? pz_tree_render_.data() : nullptr,
        .live_analysis_tree_count = (state_ == AppState::GAME_OVER && !guess_mode_)
                                         ? (int)analysis_tree_render_.size()
                                  : (state_ == AppState::PUZZLE_PLAY) ? (int)pz_tree_render_.size() : 0,
        .live_analysis_tree_cur_depth = (state_ == AppState::PUZZLE_PLAY) ? pz_cur_depth_
                                      : analysis_cur_ ? analysis_cur_->depth : 0,

        // Score graph
        .live_score_graph     = (state_ == AppState::GAME_OVER && !move_scores_.empty())
                                     ? move_scores_.data() : nullptr,
        .live_score_graph_len = (state_ == AppState::GAME_OVER) ? (int)move_scores_.size() : 0,
        .live_score_graph_cur = analysis_cur_ ? analysis_cur_->depth : 0,
        // Last-played-stone indicator: only shown while RT (or the right mouse button) is
        // held (previously always-on row/column lines were too distracting; now an opt-in
        // teal circle on demand).
        .live_last_move_r = [&]() -> int {
            if (!rt_down_ && !rclick_down_) return -1;
            if (state_ == AppState::GAME_OVER && analysis_cur_ && analysis_cur_->parent)
                return analysis_cur_->move_row;
            if ((state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL)
                && game_.history_pos < 0)
                return last_move_r_;
            return -1;
        }(),
        .live_last_move_f = [&]() -> int {
            if (!rt_down_ && !rclick_down_) return -1;
            if (state_ == AppState::GAME_OVER && analysis_cur_ && analysis_cur_->parent)
                return analysis_cur_->move_col;
            if ((state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL)
                && game_.history_pos < 0)
                return last_move_f_;
            return -1;
        }(),
        .live_show_coords = show_coords_,
        .live_labels      = (state_ == AppState::GAME_OVER && analysis_cur_ &&
                             !analysis_cur_->labels.empty())
                                ? analysis_cur_->labels.data()
                          : (state_ == AppState::PUZZLE_PLAY && !pz_marks_.empty())
                                ? pz_marks_.data() : nullptr,
        .live_label_count = (state_ == AppState::GAME_OVER && analysis_cur_)
                                ? (int)analysis_cur_->labels.size()
                          : (state_ == AppState::PUZZLE_PLAY)
                                ? (int)pz_marks_.size() : 0,
        .live_result_banner = (state_ == AppState::STONE_REMOVAL && is_local_game_ &&
                               !local_game_score_.empty())
                                  ? local_game_score_.c_str()
                            : (state_ == AppState::PUZZLE_PLAY && pz_done_ &&
                               !pz_banner_.empty())
                                  ? pz_banner_.c_str() : nullptr,
        .square_stones      = square_stones_,
        .octagon_stones     = octagon_stones_,
        .square_grid        = square_grid_,
        .tetris_captures    = tetris_captures_,
        // JOSEKI reuses the puzzle layout: comment box in the right gutter,
        // player labels and clocks suppressed
        .puzzle_mode        = (state_ == AppState::PUZZLE_PLAY ||
                               state_ == AppState::JOSEKI),
        // START popup menu (PUZZLE_BROWSE draws its own copy over the list screen)
        .popup_items        = popup_active_ ? popup_labels_.data() : nullptr,
        .popup_count        = popup_active_ ? (int)popup_labels_.size() : 0,
        .popup_index        = popup_index_,
        .popup_title        = popup_active_ ? popup_title_.c_str() : nullptr,
        .popup_styles       = (popup_active_ && !popup_styles_.empty())
                                  ? popup_styles_.data() : nullptr,
        // Joseki continuation dots
        .live_markers       = (state_ == AppState::JOSEKI && !jk_markers_.empty())
                                  ? jk_markers_.data() : nullptr,
        .live_marker_count  = (state_ == AppState::JOSEKI) ? (int)jk_markers_.size() : 0,
    };
}

void App::draw() {
    if (state_ == AppState::CREDENTIAL_PROMPT) {
        draw_credential_prompt();
        return;
    }
    if (state_ == AppState::MATCH_MENU) {
        renderer_->draw_match_menu(match_menu_);
        return;
    }
    if (state_ == AppState::PUZZLE_BROWSE) {
        draw_puzzle_browser();
        return;
    }
    if (state_ == AppState::CORR_LIST) {
        draw_corr_list();
        return;
    }
    if (state_ == AppState::CHALLENGES) {
        draw_challenges();
        return;
    }
    if (state_ == AppState::CHALLENGE_CREATE) {
        draw_challenge_create();
        return;
    }
    if (catalog_.active) {
        // Parse names for the entire listing, not just the visible window. The first
        // frame after entering a directory pays one pass of 4KB header reads; every
        // frame after that is a no-op flag scan (ensure_names_loaded skips entries
        // already loaded). Covers subdirectory navigation, which doesn't go through
        // open_game_catalog()'s own full-parse call.
        if (!catalog_.search_mode && !catalog_.virtual_year_mode && !catalog_.virtual_player_mode)
            catalog_.ensure_names_loaded(0, (int)catalog_.entries.size());
        // Refresh the [BY YEAR] virtual list once the background index finishes —
        // never called anywhere else in this file, so without it that screen was
        // stuck on "Building index..." forever if opened before indexing completed.
        catalog_.tick();
    }
    auto ds = make_ds();
    renderer_->draw_board(ds);
}

// ── Credential prompt ─────────────────────────────────────────────────────────

// Returns true when credentials are complete and accepted.
// Control keys only (Enter advances/submits, Backspace deletes). Character entry
// comes through SDL_TEXTINPUT — raw keycodes here can't represent shifted input
// (Shift+a reports 'a', Shift+1 reports '1'), which corrupted passwords.
static bool handle_cred_key(SDL_Keycode key, int& step,
                             std::string& buf,
                             std::string& username,
                             std::string& password)
{
    if (step == 0) return false;
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (step == 1) { username = buf; buf.clear(); step = 2; }
        else           { password = buf; buf.clear(); step = 0; return true; }
        return false;
    }
    if (key == SDLK_BACKSPACE) {
        if (!buf.empty()) buf.pop_back();
    }
    return false;
}

// ── Main event loop ───────────────────────────────────────────────────────────

void App::event_loop() {
    bool quit = false;

    // D-pad + trigger key-repeat state
    static const Uint8 DPAD_BTNS[] = {
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        0xFD, 0xFE  // virtual: LT (history back), RT (history forward)
    };
    Uint8  repeat_btn      = 0xFF;   // 0xFF = none held
    Uint32 repeat_next_ms  = 0;
    const int REPEAT_DELAY = 400;    // ms before first repeat
    const int REPEAT_RATE  = 80;     // ms between subsequent repeats
    // Joystick cursor repeat — snappier than the dpad's, since the whole point of
    // using the stick (per Boris) is moving the cursor fast.
    const int JS_REPEAT_DELAY = 130;
    const int JS_REPEAT_RATE  = 30;
    bool lt_down = false;  // trigger axis state (rt_down_ is a member — read by make_draw_state)
    int    js_dir_x = 0, js_dir_y = 0;      // last committed joystick cursor direction
    Uint32 js_move_next_ms = 0;             // next time that direction is allowed to step again

    while (!quit) {
        Uint32 now = SDL_GetTicks();

        // Recompute wait time: wake every second to tick the clock display
        int wait_ms = 1000;
        if (flash_until_ > now)
            wait_ms = std::min(wait_ms, (int)(flash_until_ - now));
        if (ko_flash_until_ > now)
            wait_ms = std::min(wait_ms, (int)(ko_flash_until_ - now));
        if (kata_query_after_ > 0 && kata_query_after_ > now)
            wait_ms = std::min(wait_ms, (int)(kata_query_after_ - now));
        if (live_own_query_after_ > 0 && live_own_query_after_ > now)
            wait_ms = std::min(wait_ms, (int)(live_own_query_after_ - now));
        if (pz_pending_reply_ && pz_reply_at_ > now)
            wait_ms = std::min(wait_ms, (int)(pz_reply_at_ - now));
        if (repeat_btn != 0xFF && repeat_next_ms > now)
            wait_ms = std::min(wait_ms, (int)(repeat_next_ms - now));
        if (demo_active_ && demo_.next_tick > now)
            wait_ms = std::min(wait_ms, (int)(demo_.next_tick - now));
        // Tetris capture blink: redraw every frame while it plays (the draw() at
        // the bottom of the loop paints it; vsync paces the loop)
        if (int anim_ms = renderer_->capture_anim_wait_ms(); anim_ms >= 0)
            wait_ms = std::min(wait_ms, anim_ms);
        // Wake up in time for the joystick cursor's next repeat step (same states
        // as the js_cursor_ok gate below)
        if ((state_ == AppState::PLAYING || state_ == AppState::GAME_OVER ||
             state_ == AppState::PUZZLE_PLAY || state_ == AppState::JOSEKI) &&
            (js_dir_x != 0 || js_dir_y != 0) && js_move_next_ms > now)
            wait_ms = std::min(wait_ms, (int)(js_move_next_ms - now));

        SDL_Event e;
        // Mouse motion arrives in bursts, and every draw() ends in a vsync-blocked
        // SDL_RenderPresent (~16ms). Redrawing inline per event meant a fast sweep
        // stalled for a frame per row crossed while more motion queued up behind it.
        // Motion handlers just mark the screen dirty; one redraw happens once the
        // whole burst has been drained.
        bool motion_redraw = false;
        if (SDL_WaitEventTimeout(&e, wait_ms)) {
            do {
                // Reveal the OS cursor while the mouse is in use, hide it for pad or
                // keyboard input so the pointer doesn't sit parked over the board
                // during a controller session. Only a firm stick/trigger push (past a
                // generous deadzone) counts, so idle-stick drift can't hide it after
                // the mouse just moved.
                switch (e.type) {
                case SDL_MOUSEMOTION:
                    mouse_x_ = e.motion.x;
                    mouse_y_ = e.motion.y;
                    // fall through — motion also counts as "the mouse is in use"
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEWHEEL:
                    // Drawing mode draws its own chalk-tinted crosshair, so the OS
                    // pointer stays hidden there — two cursors would be one too many.
                    if (!cursor_shown_ && !draw_mode_) { SDL_ShowCursor(SDL_ENABLE);  cursor_shown_ = true;  }
                    break;
                case SDL_KEYDOWN:
                case SDL_CONTROLLERBUTTONDOWN:
                    if (cursor_shown_)  { SDL_ShowCursor(SDL_DISABLE); cursor_shown_ = false; }
                    break;
                case SDL_CONTROLLERAXISMOTION:
                    if (cursor_shown_ && (e.caxis.value > 16000 || e.caxis.value < -16000)) {
                        SDL_ShowCursor(SDL_DISABLE); cursor_shown_ = false;
                    }
                    break;
                default: break;
                }

                if (e.type == SDL_QUIT) {
                    quit = true;

                } else if (e.type == g_net_event_type) {
                    // Inbound OGS messages are NOT drained here — see the unconditional
                    // drain below, which runs every pass whether or not this wake event
                    // ever showed up. This branch only carries the pollers whose worker
                    // threads use the same wake.
                    // OGS puzzle fetch results (worker threads wake us with this event)
                    poll_puzzle_fetch();
                    // OJE joseki node fetches, same worker/wake pattern
                    poll_joseki_fetch();
                    // KataGo GTP move (local game)
                    if (is_local_game_ && state_ == AppState::PLAYING) {
                        int gtp_row, gtp_col;
                        if (kata_gtp_.poll_genmove(gtp_row, gtp_col))
                            handle_katago_gtp_move(gtp_row, gtp_col);
                    }
                    // GTP final_score + final_status_list dead (local stone removal)
                    if (is_local_game_ && state_ == AppState::STONE_REMOVAL) {
                        std::string fscore;
                        int dead_rows[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                        int dead_cols[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                        int dead_count = 0;
                        if (kata_gtp_.poll_final_status(fscore, dead_rows, dead_cols, dead_count)) {
                            memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
                            for (int i = 0; i < dead_count; i++)
                                game_.dead_stones[dead_rows[i]][dead_cols[i]] = true;
                            local_game_score_ = fscore;
                            // Now query territory with dead stones excluded so that
                            // the colored squares inside dead groups are correct.
                            if (kata_for(game_.board_size).running())
                                kata_for(game_.board_size).query_ownership(
                                    game_.board.board, game_.board_size,
                                    game_.dead_stones, 7.5f, 100);
                            // Status updates to "PRESS A" after ownership arrives (below).
                            draw();
                        }
                    }
                    // KataGo analysis results — poll both processes
                    for (KatagoProc* kp : { &kata_, &kata_9_ }) {
                        // Territory result (stone removal phase) — ownership fills color squares
                        if (state_ == AppState::STONE_REMOVAL) {
                            int kata_own[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
                            int bs = 0;
                            if (kp->poll_ownership(kata_own, bs) && !stone_removal_has_ogs_territory_) {
                                for (int r = 0; r < bs; r++)
                                    for (int f = 0; f < bs; f++)
                                        game_.ownership[r][f] = kata_own[r][f];
                                // Territory is now ready; show the score + prompt
                                // Score itself is shown as the big banner (make_ds
                                // passes local_game_score_ as live_result_banner)
                                if (is_local_game_ && !local_game_score_.empty())
                                    set_status("PRESS " GLYPH_PS_CROSS " FOR ANALYSIS");
                                draw();
                            }
                        }
                        // Live territory overlay (PLAYING) — teaching/demo toggle, separate
                        // from the stone-removal territory query above
                        if (state_ == AppState::PLAYING && live_own_pending_) {
                            int kata_own[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
                            int bs = 0;
                            if (kp->poll_ownership(kata_own, bs)) {
                                live_own_pending_ = false;
                                if (show_live_ownership_ && bs == game_.board_size) {
                                    for (int r = 0; r < bs; r++)
                                        for (int f = 0; f < bs; f++)
                                            game_.ownership[r][f] = kata_own[r][f];
                                }
                                draw();
                            }
                        }
                        // End-of-game fill for the lobby screensaver — the answer to
                        // demo_begin_end_fill()'s query. Stones the map hands to the
                        // other side are marked dead so the fill replaces them too,
                        // leaving a board covered edge to edge in the owners' colours.
                        if (demo_active_ && demo_.end_phase && !demo_.fill_ready &&
                            (state_ == AppState::LOBBY || state_ == AppState::SEARCHING ||
                             state_ == AppState::CONNECTING)) {
                            int kata_own[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
                            int bs = 0;
                            if (kp->poll_ownership(kata_own, bs) && bs == demo_.board_size) {
                                int area_b = 0, area_w = 0;
                                for (int r = 0; r < bs; r++)
                                    for (int f = 0; f < bs; f++) {
                                        int ow = kata_own[r][f];
                                        char cell = demo_.board.board[r][f];
                                        demo_.ownership[r][f] = ow;
                                        bool dead = (cell == 1 && ow < 0) ||
                                                    (cell == 2 && ow > 0);
                                        demo_.dead[r][f] = dead;
                                        // Same rule the renderer's fill uses, so the
                                        // count matches the stones actually drawn:
                                        // a living stone keeps its colour, everything
                                        // else becomes the owner's (or stays empty
                                        // where KataGo calls the point neutral).
                                        char filled = (cell != 0 && !dead) ? cell
                                                    : (ow > 0) ? 1 : (ow < 0) ? 2 : 0;
                                        if      (filled == 1) area_b++;
                                        else if (filled == 2) area_w++;
                                    }
                                demo_.area_black = area_b;
                                demo_.area_white = area_w;
                                demo_.fill_ready = true;
                                demo_.next_tick  = SDL_GetTicks() + DEMO_FILL_HOLD_MS;
                                draw();
                            }
                        }
                        // Move suggestions / background scoring
                        {
                            int count = 0;
                            float sl = FLT_MAX;
                            if (kp->poll_moves(kata_suggestions_, count, sl)) {
                                if (bg_analysis_busy_) {
                                    // Background scoring result — store in the graph array
                                    if (sl != FLT_MAX && bg_analysis_depth_ >= 0 &&
                                        bg_analysis_depth_ < (int)move_scores_.size())
                                        move_scores_[bg_analysis_depth_] = sl;
                                    // Keep the top-2 suggestions for study-puzzle detection
                                    if (count > 0 && bg_analysis_depth_ >= 0) {
                                        PuzzleEval pe;
                                        pe.best_sl = kata_suggestions_[0].score_lead;
                                        pe.best_r  = kata_suggestions_[0].row;
                                        pe.best_f  = kata_suggestions_[0].col;
                                        if (count > 1) pe.second_sl = kata_suggestions_[1].score_lead;
                                        puzzle_eval_[bg_analysis_depth_] = pe;
                                        // Evaluate only after the game — a fresh score at depth D
                                        // can also complete the check for D-1 (needs score[D]).
                                        if (state_ == AppState::GAME_OVER) {
                                            check_puzzle(bg_analysis_depth_);
                                            check_puzzle(bg_analysis_depth_ - 1);
                                        }
                                    }
                                    bg_analysis_busy_ = false;
                                    if (state_ == AppState::GAME_OVER) draw();
                                } else {
                                    fg_kata_pending_ = false;
                                    kata_score_lead_ = sl;
                                    // Persist score on the node always; only feed the
                                    // depth-indexed graph array when this node is genuinely
                                    // on the main line — a branch node at the same depth as
                                    // a main-line move is a different position and would
                                    // otherwise clobber that move's real graph value.
                                    if (analysis_cur_ && sl != FLT_MAX) {
                                        analysis_cur_->score_lead = sl;
                                        int d = analysis_cur_->depth;
                                        if (analysis_cur_->is_main_line && d < (int)move_scores_.size())
                                            move_scores_[d] = sl;
                                    }
                                    // Filter out illegal suggestions (ko, suicide, occupied)
                                    kata_suggestion_count_ = 0;
                                    for (int i = 0; i < count; i++) {
                                        int sr = kata_suggestions_[i].row, sf = kata_suggestions_[i].col;
                                        if (sr < 0 || sf < 0 ||
                                            is_legal_analysis_move(sf, sr))
                                            kata_suggestions_[kata_suggestion_count_++] = kata_suggestions_[i];
                                    }
                                    if (state_ == AppState::GAME_OVER) draw();
                                }
                            }
                        }
                    }

                } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                    Uint8 btn = e.cbutton.button;
                    handle_controller_button(btn);
                    // Start key-repeat tracking for dpad buttons
                    for (Uint8 db : DPAD_BTNS) {
                        if (btn == db) {
                            repeat_btn     = btn;
                            repeat_next_ms = SDL_GetTicks() + REPEAT_DELAY;
                            break;
                        }
                    }

                } else if (e.type == SDL_CONTROLLERBUTTONUP) {
                    if (e.cbutton.button == repeat_btn)
                        repeat_btn = 0xFF;

                } else if (e.type == SDL_CONTROLLERAXISMOTION) {
                    // Treat LT/RT triggers as virtual buttons 0xFD/0xFE with key-repeat
                    auto on_trigger = [&](Uint8 vbtn, bool& down, Sint16 val) {
                        bool pressed = (val >= 8192);
                        if (pressed == down) return;
                        down = pressed;
                        if (pressed) {
                            handle_controller_button(vbtn);
                            repeat_btn     = vbtn;
                            repeat_next_ms = SDL_GetTicks() + REPEAT_DELAY;
                        } else {
                            if (repeat_btn == vbtn) repeat_btn = 0xFF;
                            draw();  // e.g. RT release should hide the last-move indicator promptly
                        }
                    };
                    if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
                        on_trigger(0xFD, lt_down, e.caxis.value);
                    else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                        on_trigger(0xFE, rt_down_, e.caxis.value);
                    else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
                        js_left_x_ = e.caxis.value;
                    else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
                        js_left_y_ = e.caxis.value;

                } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                    if (!pad_) {
                        pad_ = SDL_GameControllerOpen(e.cdevice.which);
                        Renderer::set_pad_style(detect_pad_style(pad_));
                        draw();  // re-theme hints to the newly connected pad
                    }

                } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                    if (pad_ && SDL_GameControllerGetJoystick(pad_) ==
                            SDL_JoystickFromInstanceID(e.cdevice.which)) {
                        SDL_GameControllerClose(pad_);
                        pad_ = nullptr;
                        Renderer::set_pad_style(detect_pad_style(pad_));
                        draw();
                    }

                } else if (e.type == SDL_TEXTINPUT) {
                    // Real composed characters (respects Shift, caps lock, layout).
                    // Only the credential prompt consumes them; elsewhere they're
                    // ignored and controls still arrive via SDL_KEYDOWN.
                    if (state_ == AppState::CREDENTIAL_PROMPT && cred_step_ >= 1) {
                        cred_buf_ += e.text.text;
                        draw();
                    }

                } else if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode k = e.key.keysym.sym;

                    if (state_ == AppState::CREDENTIAL_PROMPT && k == SDLK_ESCAPE) {
                        continue_offline();
                    } else if (state_ == AppState::CREDENTIAL_PROMPT) {
                        if (handle_cred_key(k, cred_step_, cred_buf_,
                                            cred_username_, cred_password_)) {
                            // Both credentials entered — start connecting. Mark them
                            // as prompt-entered so AUTH_OK can persist them.
                            cred_from_prompt_ = true;
                            state_ = AppState::CONNECTING;
                            set_status("CONNECTING...");
                            net_.start(cred_username_, cred_password_);
                        }
                        draw();  // re-renders the prompt from the current cred state
                    } else if (drill_rename_active_ && state_ == AppState::PUZZLE_BROWSE) {
                        // Typing a new drill name — capture everything (same
                        // plain-keycode entry as the credential prompt)
                        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                            drill_commit_rename();
                        } else if (k == SDLK_ESCAPE) {
                            drill_rename_active_ = false;
                            drill_rename_buf_.clear();
                            draw();
                        } else if (k == SDLK_BACKSPACE) {
                            if (!drill_rename_buf_.empty()) drill_rename_buf_.pop_back();
                            draw();
                        } else if (k >= SDLK_SPACE && k <= SDLK_z && k < 127) {
                            // Only accept characters that survive filename
                            // sanitizing — typing something the file can't
                            // keep (e.g. brackets) would silently vanish.
                            char c = (char)toupper((int)k);
                            if (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') {
                                drill_rename_buf_ += c;
                                draw();
                            }
                        }
                    } else if (k == SDLK_q) {
                        if (quit_confirm_) { quit = true; }
                        else { quit_confirm_ = true; draw(); }
                    } else if (k == SDLK_ESCAPE) {
                        // ESC mirrors the controller's OPTIONS/START button: open the
                        // context menu, or close it if already open. The menu is
                        // arrow-key navigable and self-describing (including QUIT), so
                        // there's no hotkey list to memorize.
                        if (quit_confirm_) { quit_confirm_ = false; draw(); }
                        else { handle_controller_button(SDL_CONTROLLER_BUTTON_START); }
                    }
                    // Any other key cancels quit confirm without acting
                    else if (quit_confirm_) {
                        quit_confirm_ = false;
                        draw();
                    }
                    // SPACE toggles freehand drawing — a big, unmissable key you can
                    // hit without looking away from the tablet. RETURN keeps the
                    // "press A" duty it used to share with SPACE.
                    else if (k == SDLK_SPACE) {
                        toggle_draw_mode();
                    }
                    // G: guess-the-move quiz over the pro game open for review.
                    else if (k == SDLK_g && e.key.repeat == 0) {
                        toggle_guess_mode();
                    }
                    // SHIFT flips the chalk between white and black. Only while
                    // drawing, so it can't quietly change state you can't see, and
                    // guarded on repeat==0 or holding the key would strobe it.
                    else if ((k == SDLK_LSHIFT || k == SDLK_RSHIFT) &&
                             draw_mode_ && e.key.repeat == 0) {
                        chalk_dark_  = !chalk_dark_;
                        flash_       = chalk_dark_ ? "BLACK CHALK" : "WHITE CHALK";
                        flash_until_ = SDL_GetTicks() + 1200;
                        draw();
                    }
                    // Keyboard shortcut mirrors for controller buttons — a full
                    // substitute for when the pad is unplugged or out of battery.
                    else {
                        Uint8 mapped = 0xFF;
                        switch (k) {
                        case SDLK_RETURN:     mapped = SDL_CONTROLLER_BUTTON_A;             break;
                        case SDLK_b: case SDLK_p:
                                              mapped = SDL_CONTROLLER_BUTTON_B;             break;
                        case SDLK_c:          mapped = SDL_CONTROLLER_BUTTON_X;             break;
                        case SDLK_m:          mapped = SDL_CONTROLLER_BUTTON_Y;             break;
                        case SDLK_s:          mapped = SDL_CONTROLLER_BUTTON_BACK;          break;
                        case SDLK_r: case SDLK_f:
                                              mapped = SDL_CONTROLLER_BUTTON_START;         break;
                        case SDLK_UP:         mapped = SDL_CONTROLLER_BUTTON_DPAD_UP;       break;
                        case SDLK_DOWN:       mapped = SDL_CONTROLLER_BUTTON_DPAD_DOWN;     break;
                        case SDLK_LEFT:       mapped = SDL_CONTROLLER_BUTTON_DPAD_LEFT;     break;
                        case SDLK_RIGHT:      mapped = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;    break;
                        // MINUS/EQUALS duplicate TAB/bracket on purpose — the more
                        // familiar +/- pairing for "adjust a number" (BOARD HUE/SAT/VAL
                        // in the DISPLAY settings), so it's discoverable without
                        // knowing L1/R1 live on these keys at all.
                        case SDLK_TAB: case SDLK_LEFTBRACKET: case SDLK_MINUS: case SDLK_KP_MINUS:
                                              mapped = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;  break;
                        case SDLK_RIGHTBRACKET: case SDLK_EQUALS: case SDLK_KP_PLUS:
                                              mapped = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
                        case SDLK_COMMA:      mapped = 0xFD; break;  // LT — step back
                        case SDLK_PERIOD:     mapped = 0xFE; break;  // RT — step forward
                        case SDLK_PAGEUP:     mapped = SDL_CONTROLLER_BUTTON_LEFTSTICK;     break;
                        case SDLK_PAGEDOWN:   mapped = SDL_CONTROLLER_BUTTON_RIGHTSTICK;    break;
                        default: break;
                        }
                        if (mapped != 0xFF) handle_controller_button(mapped);
                    }

                } else if (e.type == SDL_MOUSEMOTION) {
                    // The popup floats above everything, so it gets the mouse first:
                    // hovering a row moves the highlight, keeping the pointer and the
                    // d-pad cursor from ever disagreeing about what's selected.
                    // Drawing: stamp a segment from the last point to this one. Every
                    // point in a burst is stamped into the layer, but the frame is
                    // coalesced like any other motion — no ink is ever dropped, it
                    // just appears one present later.
                    if (draw_mode_ && !popup_active_) {
                        if (pen_down_) {
                            renderer_->annot_segment(pen_x_, pen_y_,
                                                     e.motion.x, e.motion.y, chalk_dark_);
                            pen_x_ = e.motion.x;
                            pen_y_ = e.motion.y;
                            motion_redraw = true;
                        }
                    }
                    else if (popup_active_) {
                        int hit = popup_hit(e.motion.x, e.motion.y);
                        if (hit >= 0 && hit != popup_index_) {
                            popup_index_ = hit;
                            popup_armed_ = -1;  // same disarm-on-move rule the d-pad uses
                            popup_sync_labels();
                            motion_redraw = true;
                        }
                    }
                    // Settings menu: hover moves the focus outright — unlike the
                    // lists, focus here costs nothing (no file is read), so the
                    // pointer and the d-pad can share one cursor.
                    else if (state_ == AppState::MATCH_MENU) {
                        int c, r;
                        if (match_menu_hit(e.motion.x, e.motion.y, c, r) &&
                            (c != match_menu_.focus_col || r != match_menu_.focus_row)) {
                            match_menu_.focus_col = c;
                            match_menu_.focus_row = r;
                            motion_redraw = true;
                        }
                    }
                    // The catalog deliberately has NO hover feedback. Any hover tint
                    // means a redraw per row crossed, and a catalog redraw is the most
                    // expensive frame in the app (a width pass over every entry, plus
                    // thumbnails) — with vsync that pinned a sustained 60fps re-render
                    // of a 2000-entry list just to tint a row. Clicking still works;
                    // motion here is free.
                    //
                    // Puzzle browser: hover only tints the row — it deliberately does
                    // not move the selection, so nothing is fetched by sweeping past.
                    // Cheap here: the list layout is O(1) and these lists are short.
                    else if (state_ == AppState::PUZZLE_BROWSE) {
                        int h = pz_hit(e.motion.x, e.motion.y);
                        if (h != pz_hover_) { pz_hover_ = h; motion_redraw = true; }
                    }
                    // MY GAMES list: same cheap hover tint as the puzzle browser.
                    else if (state_ == AppState::CORR_LIST) {
                        int h = corr_hit(e.motion.x, e.motion.y);
                        if (h != corr_hover_) { corr_hover_ = h; motion_redraw = true; }
                    }
                    else if (state_ == AppState::CHALLENGES) {
                        int h = chal_hit(e.motion.x, e.motion.y);
                        if (h != chal_hover_) { chal_hover_ = h; motion_redraw = true; }
                    }
                    else if (state_ == AppState::CHALLENGE_CREATE) {
                        int h = cc_hit(e.motion.x, e.motion.y);
                        if (h != cc_hover_) { cc_hover_ = h; motion_redraw = true; }
                    }
                    // Snap the board cursor to the hovered grid point (mouse and pad
                    // coexist — the cursor just follows whichever moved last)
                    else if (!catalog_.active && state_ != AppState::MATCH_MENU &&
                        ((state_ == AppState::PLAYING && game_.history_pos < 0) ||
                         state_ == AppState::GAME_OVER ||
                         state_ == AppState::PUZZLE_PLAY)) {
                        BoardView view;
                        renderer_->get_board_view(view, game_.board_size);
                        int mr, mf;
                        if (renderer_->screen_to_board(view, e.motion.x, e.motion.y, mr, mf) &&
                            (mr != game_.cursor_r || mf != game_.cursor_f)) {
                            game_.cursor_r = mr;
                            game_.cursor_f = mf;
                            motion_redraw = true;
                        }
                    }

                } else if (e.type == SDL_MOUSEBUTTONUP) {
                    if (e.button.button == SDL_BUTTON_LEFT) pen_down_ = false;
                    else if (e.button.button == SDL_BUTTON_RIGHT && rclick_down_) {
                        rclick_down_ = false;
                        draw();
                    }

                } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                    // Pen down in drawing mode starts a stroke and stamps a dot, so a
                    // tap leaves a mark. Nothing else sees the click — a stylus press
                    // IS a left click, so without this every stroke would also be
                    // trying to place a stone.
                    if (draw_mode_ && !popup_active_ && e.button.button == SDL_BUTTON_LEFT) {
                        pen_down_ = true;
                        pen_x_    = e.button.x;
                        pen_y_    = e.button.y;
                        renderer_->annot_segment(pen_x_, pen_y_, pen_x_, pen_y_, chalk_dark_);
                        draw();
                    }
                    // The popup owns the click while it's open. Without this the click
                    // fell through to the board underneath the dimmed overlay and fired
                    // whichever popup row happened to be highlighted.
                    else if (popup_active_) {
                        if (e.button.button == SDL_BUTTON_LEFT) {
                            bool inside = false;
                            int  hit    = popup_hit(e.button.x, e.button.y, &inside);
                            if (hit >= 0) {
                                // Route through the normal select path so confirm-armed
                                // items ("REALLY RESIGN?") still take two clicks.
                                popup_index_ = hit;
                                handle_popup_button(SDL_CONTROLLER_BUTTON_A);
                            } else if (!inside) {
                                close_popup_menu();  // clicking off the panel dismisses
                                draw();
                            }
                        } else if (e.button.button == SDL_BUTTON_RIGHT) {
                            handle_popup_button(SDL_CONTROLLER_BUTTON_B);
                        }
                    } else if (e.button.button == SDL_BUTTON_LEFT) {
                        bool board_state = !catalog_.active && state_ != AppState::MATCH_MENU &&
                            (state_ == AppState::PLAYING || state_ == AppState::GAME_OVER ||
                             state_ == AppState::PUZZLE_PLAY);
                        if (board_state) {
                            // Only place when the click actually lands on a grid point —
                            // clicking panels/dead space must not drop a stone at the
                            // last cursor position. (STONE_REMOVAL deliberately excluded:
                            // accepting a score shouldn't happen from a stray click.)
                            BoardView view;
                            renderer_->get_board_view(view, game_.board_size);
                            int mr, mf;
                            if (renderer_->screen_to_board(view, e.button.x, e.button.y, mr, mf)) {
                                game_.cursor_r = mr;
                                game_.cursor_f = mf;
                                handle_controller_button(SDL_CONTROLLER_BUTTON_A);
                            }
                        } else if (catalog_.active) {
                            // Click commits the selection (loading its thumbnail);
                            // clicking the already-selected row opens it. Two steps
                            // on purpose — a stray click shouldn't launch a game.
                            int h = catalog_hit(e.button.x, e.button.y);
                            if (h >= 0) {
                                if (h == catalog_.index) {
                                    handle_controller_button(SDL_CONTROLLER_BUTTON_A);
                                } else {
                                    set_catalog_index(h);
                                    draw();
                                }
                            }
                        } else if (state_ == AppState::MATCH_MENU) {
                            // Clicking a control focuses it and activates it in one
                            // go — toggles are cheap and instantly reversible, so
                            // there's nothing here worth a two-step confirm. BOARD
                            // HUE/SAT/VAL additionally have their own [-]/[+] hit
                            // targets, checked first since they sit inside the same
                            // row match_menu_hit already resolves the click to.
                            int c, r;
                            if (match_menu_hit(e.button.x, e.button.y, c, r)) {
                                match_menu_.focus_col = c;
                                match_menu_.focus_row = r;
                                int dcol = match_menu_.ingame ? 0 : (match_menu_.katago_mode ? -1 : 2);
                                int zone = (c == dcol && r >= 9)
                                               ? renderer_->match_menu_stepper_zone(
                                                     match_menu_, r, e.button.x, e.button.y)
                                               : -1;
                                if (zone == 0)      { adjust_board_hsv(r, -5); renderer_->draw_match_menu(match_menu_); }
                                else if (zone == 1) { adjust_board_hsv(r, 5);  renderer_->draw_match_menu(match_menu_); }
                                else                 handle_controller_button(SDL_CONTROLLER_BUTTON_A);
                            }
                        } else if (state_ == AppState::PUZZLE_BROWSE) {
                            // Click commits the selection; clicking the row that's
                            // already selected opens it. Two steps on purpose, so a
                            // stray click can never fire a fetch you didn't aim at.
                            int h = pz_hit(e.button.x, e.button.y);
                            if (h >= 0 && h < pz_item_count()) {
                                if (h == pz_index_) {
                                    handle_puzzle_button(SDL_CONTROLLER_BUTTON_A);
                                } else {
                                    pz_index_ = h;
                                    draw();
                                }
                            }
                        } else if (state_ == AppState::CORR_LIST) {
                            // Same two-step: click selects, clicking the selected row
                            // opens the game (guarding the LOADING/empty placeholder row).
                            int h = corr_hit(e.button.x, e.button.y);
                            if (h >= 0 && h < (int)corr_games_.size()) {
                                if (h == corr_index_) {
                                    handle_corr_button(SDL_CONTROLLER_BUTTON_A);
                                } else {
                                    corr_index_ = h;
                                    draw();
                                }
                            }
                        } else if (state_ == AppState::CHALLENGES) {
                            // Click selects; clicking the selected row arms/confirms
                            // accept (the same confirm-gated A path). Decline is pad Y.
                            int h = chal_hit(e.button.x, e.button.y);
                            if (h >= 0 && h < (int)challenges_.size()) {
                                if (h == chal_index_) {
                                    handle_chal_button(SDL_CONTROLLER_BUTTON_A);
                                } else {
                                    chal_index_ = h;
                                    chal_accept_confirm_  = false;
                                    chal_decline_confirm_ = false;
                                    draw();
                                }
                            }
                        } else if (state_ == AppState::CHALLENGE_CREATE) {
                            // Click focuses a row; clicking the focused row advances
                            // its value (or arms/confirms SEND, via the same A path).
                            int h = cc_hit(e.button.x, e.button.y);
                            if (h >= 0 && h < CC_ROWS) {
                                if (h == cc_row_) {
                                    handle_cc_button(SDL_CONTROLLER_BUTTON_A);
                                } else {
                                    cc_row_ = h;
                                    cc_send_confirm_ = false;
                                    draw();
                                }
                            }
                        }
                    } else if (e.button.button == SDL_BUTTON_RIGHT) {
                        // Right-click is a mouse gesture, not an alias for circle.
                        // Blanket-forwarding it to B fired whatever hotkey circle
                        // happened to carry in that state — in the LOBBY that's
                        // "open the puzzle browser", which is why right-clicking
                        // kept launching puzzles; mid-game it armed a pass.
                        // On the screens whose own hint reads "=CLOSE" it backs
                        // out. On a game board, holding it shows the last-played
                        // stone (like holding RT) — ESC opens the menu there.
                        // Everywhere else it opens the context menu, which
                        // already lists PUZZLES, CATALOG, SETTINGS and the rest.
                        if (catalog_.active || state_ == AppState::MATCH_MENU) {
                            handle_controller_button(SDL_CONTROLLER_BUTTON_B);
                        } else if (state_ == AppState::PLAYING ||
                                   state_ == AppState::STONE_REMOVAL ||
                                   state_ == AppState::GAME_OVER) {
                            rclick_down_ = true;
                            draw();
                        } else if (state_ != AppState::CREDENTIAL_PROMPT &&
                                   state_ != AppState::CONNECTING) {
                            open_popup_menu();
                            draw();
                        }
                    }

                } else if (e.type == SDL_MOUSEWHEEL) {
                    // Wheel: scroll lists in the catalog/menu/browser/popup, step moves on a board
                    bool list_ctx = popup_active_ || catalog_.active ||
                                    state_ == AppState::MATCH_MENU ||
                                    state_ == AppState::PUZZLE_BROWSE ||
                                    state_ == AppState::CORR_LIST ||
                                    state_ == AppState::CHALLENGES ||
                                    state_ == AppState::CHALLENGE_CREATE;
                    Uint8 up_btn   = list_ctx ? (Uint8)SDL_CONTROLLER_BUTTON_DPAD_UP   : (Uint8)0xFD;
                    Uint8 down_btn = list_ctx ? (Uint8)SDL_CONTROLLER_BUTTON_DPAD_DOWN : (Uint8)0xFE;
                    for (int s = e.wheel.y; s > 0; s--) handle_controller_button(up_btn);
                    for (int s = e.wheel.y; s < 0; s++) handle_controller_button(down_btn);

                } else if (e.type == SDL_WINDOWEVENT) {
                    if (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                        e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                        draw();
                }

            } while (!quit && SDL_PollEvent(&e));

            // One frame for the whole drained burst. Anything else that called
            // draw() directly has already painted; this only covers the motion
            // handlers above, which deliberately defer to here.
            if (motion_redraw && !quit) draw();
        }

        if (quit_requested_) quit = true;  // QUIT chosen from a context menu

        // Fire key-repeat for held dpad button
        now = SDL_GetTicks();
        if (repeat_btn != 0xFF && now >= repeat_next_ms) {
            handle_controller_button(repeat_btn);
            repeat_next_ms = now + REPEAT_RATE;
        }

        // Left-stick joystick cursor — live play (at the live edge) and GAME_OVER
        // analysis (placing branch stones / labels), but not while the catalog or
        // the result freeze-screen is up.
        // Snap the stick to one of 8 directions (45-degree sectors) so a tilt
        // near a diagonal commits firmly to that diagonal instead of racing
        // between axes, then step the cursor at a fixed repeat rate (same
        // delay/rate as the dpad buttons) instead of accelerating continuously.
        bool js_cursor_ok =
            !catalog_.active && !popup_active_ &&
            ((state_ == AppState::PLAYING && game_.history_pos < 0) ||
             state_ == AppState::GAME_OVER ||
             state_ == AppState::PUZZLE_PLAY ||
             state_ == AppState::JOSEKI);
        if (js_cursor_ok) {
            const float DEAD    = 8192.f;
            const float TAN22_5 = 0.41421356f;  // tan(22.5 deg)
            float x = (float)js_left_x_, y = (float)js_left_y_;
            float ax = std::fabs(x), ay = std::fabs(y);
            int dx = 0, dy = 0;
            if (std::sqrt(x * x + y * y) > DEAD) {
                dx = (x > 0.f) ? 1 : (x < 0.f ? -1 : 0);
                dy = (y > 0.f) ? 1 : (y < 0.f ? -1 : 0);
                if (ax > 0.f && ay / ax < TAN22_5) dy = 0;       // within 22.5 deg of horizontal
                else if (ay > 0.f && ax / ay < TAN22_5) dx = 0;  // within 22.5 deg of vertical
            }
            if (dx != 0 || dy != 0) {
                bool changed = (dx != js_dir_x || dy != js_dir_y);
                if (changed || now >= js_move_next_ms) {
                    int n = game_.board_size - 1;
                    game_.cursor_f = std::max(0, std::min(n, game_.cursor_f + dx));
                    game_.cursor_r = std::max(0, std::min(n, game_.cursor_r + dy));
                    js_move_next_ms = now + (changed ? JS_REPEAT_DELAY : JS_REPEAT_RATE);
                }
            }
            js_dir_x = dx;
            js_dir_y = dy;
        } else {
            js_dir_x = js_dir_y = 0;
        }

        // Advance demo screensaver when idle (not in a live game)
        bool idle = (state_ == AppState::LOBBY || state_ == AppState::SEARCHING ||
                     state_ == AppState::CONNECTING);
        if (demo_active_ && idle && now >= demo_.next_tick) {
            if (demo_.pos < (int)demo_.rows.size()) {
                demo_.board.place_stone(demo_.rows[demo_.pos], demo_.cols[demo_.pos],
                                        demo_.colors[demo_.pos]);
                demo_.pos++;
                demo_.next_tick = now + 1000;
            } else if (!demo_.end_phase) {
                demo_begin_end_fill();  // ask KataGo who owns what, then hold on it
            } else {
                demo_next_game();       // hold over — on to the next game
            }
        }

        // Drain everything the network thread has queued. Deliberately unconditional
        // and not inside the `e.type == g_net_event_type` branch above: that made
        // delivery depend on an SDL wake event surviving the trip from the network
        // thread, and a single lost one stranded the message indefinitely — nothing
        // else drains the queue, and a game waiting on the opponent produces no
        // further events to wake us (only latency/pong, which queue nothing). The
        // loop already wakes at least once a second to tick the clock, so this costs
        // an uncontended mutex and an empty check, and no latency: an SDL wake still
        // breaks the wait, and this runs in that same pass.
        {
            NetMsg msg;
            while (net_.poll_msg(msg))
                handle_net_msg(msg);
        }

        // Live play: every clock event carries the server's own answer to "whose move
        // is it". Once that has disagreed with our board for ten seconds, something in
        // the move stream went missing — a move of theirs we never applied, or one of
        // ours the server never took — and the board is dead: nothing further will
        // arrive to notice it, because each side is waiting on the other. Pull the
        // position back rather than sit there losing on time (which is exactly how the
        // G0GOGO handicap game was lost, 2026-07-27). Ten seconds is far longer than a
        // move round-trip, so a move genuinely in flight can't trip it.
        if (state_ == AppState::PLAYING && !is_local_game_ && game_.game_id != 0 &&
            game_.server_turn_player > 0) {
            bool server_says_mine = (game_.server_turn_player == game_.my_player_id);
            if (server_says_mine != game_.my_turn) {
                if (game_.turn_mismatch_at == 0)               game_.turn_mismatch_at = now;
                else if (now - game_.turn_mismatch_at > 10000) request_resync();
            } else {
                game_.turn_mismatch_at = 0;
            }
        }

        // Stone removal: our accept may not have reached the server (seen in practice —
        // no protocol error, the send just doesn't always take effect first try). Don't
        // wait solely on the server's own removed_stones re-broadcast (can take 20-30s);
        // proactively resend a few seconds after our own accept if nothing's changed yet.
        if (state_ == AppState::STONE_REMOVAL && my_accept_sent_ &&
            now >= accept_resend_at_) {
            net_.cmd_accept_stones(game_.game_id);
            accept_resend_at_ = now + 6000;
        }

        // Fire deferred KataGo query once the user has settled on a position
        if (kata_query_after_ > 0 && now >= kata_query_after_) {
            kata_query_after_ = 0;
            if (state_ == AppState::GAME_OVER && analysis_cur_ && kata_analysis_enabled_) {
                bg_analysis_busy_ = false;  // cancel any in-flight background query
                fg_kata_pending_  = true;
                kata_for(game_.board_size).query_moves(
                    analysis_cur_->board.board, game_.board_size,
                    analysis_cur_->board.turn_is_black == 1, review_komi_);
            }
        }

        // Live territory overlay: same lost-response protection as bg_analysis_busy_
        if (live_own_pending_ && now - live_own_started_at_ > 15000) {
            live_own_pending_ = false;
        }

        // Fire deferred live-ownership query once the board has settled
        if (live_own_query_after_ > 0 && now >= live_own_query_after_) {
            live_own_query_after_ = 0;
            if (show_live_ownership_ && state_ == AppState::PLAYING &&
                kata_for(game_.board_size).running()) {
                bg_analysis_busy_    = false;  // release the shared single-query slot
                live_own_pending_    = true;
                live_own_started_at_ = now;
                bool  no_dead[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
                float km = is_local_game_ ? local_game_komi_ : 7.5f;
                kata_for(game_.board_size).query_ownership(
                    game_.board.board, game_.board_size, no_dead, km, 40);
            }
        }

        // Fire the puzzle opponent's delayed reply once it's had its moment
        if (pz_pending_reply_ && now >= pz_reply_at_ && state_ == AppState::PUZZLE_PLAY) {
            pz_fire_pending_reply();
            draw();
        }

        // A list screen showed flash_ in its footer and the message has now lapsed.
        // Those screens repaint only on input, so repaint once to clear it (the wait
        // timeout above already wakes us at flash_until_).
        if (flash_on_list_ && (flash_.empty() || now >= flash_until_)) {
            flash_on_list_ = false;
            if (state_ == AppState::CORR_LIST || state_ == AppState::CHALLENGES ||
                state_ == AppState::CHALLENGE_CREATE)
                draw();
        }

        // If a background query's response never arrives (lost/corrupted on the way
        // back from KataGo — has happened; see katago.cpp's stderr-pipe fix), bg_analysis_busy_
        // would otherwise stay stuck true forever, silently blocking every subsequent
        // depth for the rest of the session. Give up on it after a generous timeout so
        // the sweep can move on — that one depth just stays unscored instead of
        // everything after it.
        if (bg_analysis_busy_ && now - bg_analysis_started_at_ > 15000) {
            bg_analysis_busy_ = false;
        }

        // Background scoring — fill move_scores_[] with low-visit KataGo queries.
        // Runs during PLAYING (so the graph is mostly ready at game-over) and during
        // GAME_OVER (fills in positions the user hasn't explicitly navigated to yet).
        // Guards: no foreground query in-flight, no deferred query pending.
        if ((state_ == AppState::PLAYING || state_ == AppState::GAME_OVER) &&
            !bg_analysis_busy_ && !fg_kata_pending_ && kata_query_after_ == 0 &&
            !live_own_pending_ && kata_for(game_.board_size).running()) {
            // Skip depths that already have a score
            while (bg_analysis_next_ < (int)move_scores_.size() &&
                   move_scores_[bg_analysis_next_] != FLT_MAX)
                bg_analysis_next_++;
            if (bg_analysis_next_ < (int)move_scores_.size() &&
                bg_analysis_next_ < (int)game_.history.size()) {
                const GameState& hs = game_.history[bg_analysis_next_];
                kata_for(game_.board_size).query_moves(
                    hs.board, game_.board_size, hs.turn_is_black == 1, review_komi_, 50);
                bg_analysis_depth_      = bg_analysis_next_++;
                bg_analysis_busy_       = true;
                bg_analysis_started_at_ = now;
            }
        }

        // Tick clock display every second even with no events
        if (!quit) draw();
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────

int App::run() {
    if (!init()) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Failed to initialise SDL:\n%s", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OGS Client", buf, nullptr);
        return 1;
    }

    g_net_event_type = SDL_RegisterEvents(1);
    if (g_net_event_type == (Uint32)-1) {
        cleanup();
        return 1;
    }

    std::string username, password;
    std::string kata_exe, kata_model, kata_cfg, kata_model_9x9, kata_human_model;
    load_config(username, password, kata_exe, kata_model, kata_cfg,
                kata_model_9x9, kata_human_model);
    my_username_       = username;
    kata_exe_          = kata_exe;
    kata_model_        = kata_model;
    kata_human_model_  = kata_human_model;
    load_adaptive();        // restore the adaptive KataGo strength from previous sessions
    load_solved_puzzles();     // restore the solved-puzzle checklist
    load_known_collections();  // restore metadata for the pinned MY SETS section
    load_settings();           // restore match/display settings from previous sessions
    if (!kata_exe.empty() && !kata_model.empty() && !kata_cfg.empty())
        kata_.start(kata_exe, kata_model, kata_cfg);
    if (!kata_exe.empty() && !kata_model_9x9.empty() && !kata_cfg.empty())
        kata_9_.start(kata_exe, kata_model_9x9, kata_cfg);
    if (!username.empty()) {
        set_status("CONNECTING...");
        draw();
        net_.start(username, password);
    } else {
        state_     = AppState::CREDENTIAL_PROMPT;
        cred_step_ = 1;
        draw();
    }

    event_loop();
    cleanup();
    return 0;
}

int main(int /*argc*/, char* /*argv*/[]) {
    setvbuf(stderr, nullptr, _IONBF, 0);  // unbuffered even when piped
#ifdef _WIN32
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        fprintf(stderr, "[CRASH] SEH 0x%08lX at %p thread=%lu\n",
                (unsigned long)ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress,
                (unsigned long)GetCurrentThreadId());
        fflush(stderr);
        // stderr alone is lost when the app is launched without an attached console
        // (e.g. double-clicked) — persist the same line to disk so a crash leaves a
        // forensic trail regardless of how it was started. Opened/closed immediately
        // (rather than an fd kept open for the app's lifetime) since this only ever
        // fires once, right before the process goes down.
        time_t t = time(nullptr);
        struct tm* tm_info = localtime(&t);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
        FILE* cf = fopen("crash.log", "a");
        if (cf) {
            fprintf(cf, "[%s] SEH 0x%08lX at %p thread=%lu\n",
                    ts,
                    (unsigned long)ep->ExceptionRecord->ExceptionCode,
                    ep->ExceptionRecord->ExceptionAddress,
                    (unsigned long)GetCurrentThreadId());
            fclose(cf);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });
#endif
    App* app = new App();
    int r = app->run();
    delete app;
    return r;
}
