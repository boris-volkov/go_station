#include "katago.hpp"
#include "json.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <string>
#include <vector>

using json = nlohmann::json;
extern Uint32 g_net_event_type;

// ── Logging ───────────────────────────────────────────────────────────────────
static FILE* g_kata_log = nullptr;
static void kata_log(const char* msg) {
    if (!g_kata_log) g_kata_log = fopen("kata.log", "w");
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    char ts[24];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm_info);
    if (g_kata_log) { fprintf(g_kata_log, "[%s] %s\n", ts, msg); fflush(g_kata_log); }
    fprintf(stderr, "[kata] [%s] %s\n", ts, msg);
}
static void kata_logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kata_log(buf);
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

// Our (row, col) → KataGo GTP string.
// Our row=0 is the TOP of the board; KataGo GTP row 1 is the BOTTOM.
// Column letters skip 'I': A=0, B=1, ..., H=7, J=8, K=9, ...
static std::string to_gtp(int row, int col, int board_size) {
    char col_c = (col < 8) ? char('A' + col) : char('A' + col + 1);
    return std::string(1, col_c) + std::to_string(board_size - row);
}

// KataGo GTP string → our (row, col). Returns false for "pass" or invalid input.
static bool from_gtp(const std::string& gtp, int board_size, int& row_out, int& col_out) {
    if (gtp.size() < 2) return false;
    char letter = (char)toupper((unsigned char)gtp[0]);
    if (letter == 'I') return false;
    int col = (letter >= 'J') ? (letter - 'A' - 1) : (letter - 'A');
    if (col < 0 || col >= board_size) return false;
    // "pass" (and anything else that isn't a coordinate) has a non-numeric remainder,
    // which would make stoi() below throw. Reject up front rather than relying on an
    // exception to unwind — a thrown exception here previously escaped all the way out
    // of reader_loop's per-line try/catch and discarded the *entire* response (not just
    // the pass entry), silently losing analysis for any position where KataGo suggested
    // a pass among its top moves or principal variations (common in the endgame).
    for (size_t i = 1; i < gtp.size(); i++)
        if (!isdigit((unsigned char)gtp[i])) return false;
    int gtp_row = std::stoi(gtp.substr(1));
    int row = board_size - gtp_row;
    if (row < 0 || row >= board_size) return false;
    row_out = row;
    col_out = col;
    return true;
}

// KataGo ownership array indexing: ownership[y * boardXSize + x], row-major
// "starting at the top-left of the board (e.g. A19) and going to the bottom
// right (e.g. T1)" (KataGo docs/Analysis_Engine.md) — so y=0 is the TOP row,
// same as our row convention (unlike GTP coordinates, where row 1 is the
// bottom — that difference is what to_gtp/from_gtp handle, and misreading
// ownership as if it shared GTP's orientation vertically mirrors the map).
static inline int kata_idx(int r, int c, int bs) {
    return r * bs + c;
}

// ── Process management ────────────────────────────────────────────────────────

bool KatagoProc::start(const std::string& exe, const std::string& model, const std::string& cfg) {
    if (running_.load()) return true;

#ifndef _WIN32
    kata_log("KataGo subprocess support is not implemented on this platform — skipping");
    return false;
#else
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_stdin_r = NULL, h_stdout_w = NULL, h_stderr_w = NULL;
    if (!CreatePipe(&h_stdin_r, &h_write_, &sa, 0))        return false;
    if (!CreatePipe(&h_read_,   &h_stdout_w, &sa, 0)) {
        CloseHandle(h_stdin_r); CloseHandle(h_write_); h_write_ = NULL;
        return false;
    }
    // Separate pipe for stderr — sharing one pipe between stdout and stderr lets KataGo's
    // own writes to each stream interleave at the byte level (observed in practice:
    // warning text and JSON response bytes getting spliced together), corrupting the
    // JSON-lines protocol reader_loop() depends on. A corrupted/lost response never sets
    // moves_ready_/result_ready_, which permanently wedges bg_analysis_busy_ or
    // fg_kata_pending_ in main.cpp — analysis silently stops advancing for the rest of
    // the session. Keeping stderr on its own pipe (read by stderr_reader_loop(), which
    // just logs raw text) keeps stdout's stream pure JSON.
    if (!CreatePipe(&h_stderr_read_, &h_stderr_w, &sa, 0)) {
        CloseHandle(h_stdin_r); CloseHandle(h_write_); CloseHandle(h_read_); CloseHandle(h_stdout_w);
        h_write_ = h_read_ = NULL;
        return false;
    }
    // Make our (parent) ends non-inheritable
    SetHandleInformation(h_write_,       HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_read_,        HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_stderr_read_, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + exe + "\" analysis"
                    + " -model \"" + model + "\""
                    + " -config \"" + cfg + "\"";
    kata_logf("starting: %s", cmd.c_str());
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    // Run KataGo from its own directory so logDir and other relative paths resolve correctly.
    std::string kata_dir = exe;
    auto sep = kata_dir.find_last_of("/\\");
    if (sep != std::string::npos) kata_dir.resize(sep);
    const char* kata_cwd = kata_dir.empty() ? nullptr : kata_dir.c_str();
    kata_logf("working directory: %s", kata_cwd ? kata_cwd : "(inherited)");

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = h_stdin_r;
    si.hStdOutput = h_stdout_w;
    si.hStdError  = h_stderr_w;   // own pipe — see comment above on why not merged with stdout

    BOOL ok = CreateProcessA(nullptr, cmd_buf.data(),
                             nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, kata_cwd,
                             &si, &proc_);
    CloseHandle(h_stdin_r);
    CloseHandle(h_stdout_w);
    CloseHandle(h_stderr_w);
    if (!ok) {
        kata_logf("CreateProcess FAILED (error %lu)", GetLastError());
        CloseHandle(h_write_); CloseHandle(h_read_); CloseHandle(h_stderr_read_);
        h_write_ = h_read_ = h_stderr_read_ = NULL;
        return false;
    }
#endif

    kata_log("process created — reader threads starting");
    running_.store(true);
    reader_        = std::thread([this]() { reader_loop(); });
    stderr_reader_ = std::thread([this]() { stderr_reader_loop(); });

    // Send a minimal 1-visit ping so we can measure how long init takes.
    // The ping response will be logged as "moves response id=0 ...".
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_query_id_    = 0;
        pending_response_id_ = 0;
        pending_query_bs_    = 1;
    }
    json ping = {
        {"id",          "0"},
        {"moves",       json::array()},
        {"initialStones", json::array()},
        {"rules",       "chinese"},
        {"komi",        7.5},
        {"boardXSize",  2},
        {"boardYSize",  2},
        {"maxVisits",   1},
        {"analyzeTurns", {0}}
    };
    kata_log("startup ping sent — waiting for first response to confirm KataGo is ready");
    write_line(ping.dump());
    return true;
}

void KatagoProc::stop() {
    if (!running_.load() && !reader_.joinable()) return;
    running_.store(false);
#ifdef _WIN32
    if (h_write_) { CloseHandle(h_write_); h_write_ = NULL; }  // EOF → katago exits
    if (reader_.joinable()) reader_.join();
    if (stderr_reader_.joinable()) stderr_reader_.join();
    if (h_read_)         { CloseHandle(h_read_);         h_read_         = NULL; }
    if (h_stderr_read_)  { CloseHandle(h_stderr_read_);  h_stderr_read_  = NULL; }
    if (proc_.hProcess) { TerminateProcess(proc_.hProcess, 0);
                          CloseHandle(proc_.hProcess); proc_.hProcess = NULL; }
    if (proc_.hThread)  { CloseHandle(proc_.hThread);  proc_.hThread  = NULL; }
#endif
}

void KatagoProc::write_line(const std::string& s) {
#ifdef _WIN32
    if (!h_write_) return;
    std::string line = s + "\n";
    DWORD written = 0;
    WriteFile(h_write_, line.c_str(), DWORD(line.size()), &written, nullptr);
#endif
}

// ── Query ─────────────────────────────────────────────────────────────────────

void KatagoProc::query_ownership(const char  board[][MAX_BOARD_SIZE], int board_size,
                                  const bool  dead[][MAX_BOARD_SIZE],  float komi,
                                  int max_visits) {
    if (!running_.load()) { kata_log("query_ownership: process not running — skipped"); return; }

    // Build initialStones: all living stones on the board
    json stones = json::array();
    for (int r = 0; r < board_size; r++) {
        for (int f = 0; f < board_size; f++) {
            if (board[r][f] == 0) continue;
            if (dead && dead[r][f])  continue;   // dead stone — exclude
            stones.push_back({ (board[r][f] == 1) ? "B" : "W", to_gtp(r, f, board_size) });
        }
    }

    int qid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        result_ready_        = false;
        moves_ready_         = false;
        qid                  = ++pending_query_id_;
        pending_response_id_ = qid;
        pending_query_bs_    = board_size;
    }

    json q = {
        {"id",             std::to_string(qid)},
        {"moves",          json::array()},
        {"initialStones",  stones},
        {"rules",          "chinese"},
        {"komi",           komi},
        {"boardXSize",     board_size},
        {"boardYSize",     board_size},
        {"includeOwnership", true},
        {"maxVisits",      max_visits},
        {"analyzeTurns",   {0}}
    };
    kata_logf("ownership query id=%d bs=%d stones=%d", qid, board_size, (int)stones.size());
    write_line(q.dump());
}

// ── Move-suggestion query ─────────────────────────────────────────────────────

void KatagoProc::query_moves(const char board[][MAX_BOARD_SIZE], int board_size,
                              bool black_to_play, float komi, int max_visits,
                              const char prev_board[][MAX_BOARD_SIZE]) {
    if (!running_.load()) { kata_log("query_moves: process not running — skipped"); return; }

    // Find the move that led here: the only point empty before and occupied now
    // (captures only ever empty points). It must be the opponent's stone; anything
    // else (a pass, setup edits) falls back to a bare position with no ko ban.
    int mv_r = -1, mv_c = -1;
    if (prev_board) {
        int placed = 0;
        for (int r = 0; r < board_size; r++)
            for (int c = 0; c < board_size; c++)
                if (prev_board[r][c] == 0 && board[r][c] != 0) { mv_r = r; mv_c = c; placed++; }
        if (placed != 1 || board[mv_r][mv_c] != (black_to_play ? 2 : 1)) mv_r = mv_c = -1;
    }
    auto base = (mv_r >= 0) ? prev_board : board;

    json stones = json::array();
    for (int r = 0; r < board_size; r++)
        for (int c = 0; c < board_size; c++) {
            if (base[r][c] == 0) continue;
            stones.push_back({ base[r][c] == 1 ? "B" : "W", to_gtp(r, c, board_size) });
        }
    json moves = json::array();
    if (mv_r >= 0) moves.push_back({ black_to_play ? "W" : "B", to_gtp(mv_r, mv_c, board_size) });

    int qid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        result_ready_        = false;
        moves_ready_         = false;
        qid                  = ++pending_query_id_;
        pending_response_id_ = qid;
        pending_query_bs_    = board_size;
    }

    json q = {
        {"id",             std::to_string(qid)},
        {"moves",          moves},
        {"initialStones",  stones},
        // initialPlayer is whoever plays moves[0] — the opponent when there's a last move
        {"initialPlayer",  (black_to_play == moves.empty()) ? "B" : "W"},
        {"rules",          "chinese"},
        {"komi",           komi},
        {"boardXSize",     board_size},
        {"boardYSize",     board_size},
        {"maxVisits",      max_visits},
        {"analyzeTurns",   {(int)moves.size()}}
        // NOTE: no "includePV" field — KataGo's analysis engine already includes a
        // "pv" array in every moveInfo by default; this key isn't a real query field
        // and previously just triggered a "do you have a typo?" warning on every query.
    };
    kata_logf("moves query id=%d bs=%d %s stones=%d visits=%d",
              qid, board_size, black_to_play ? "B" : "W", (int)stones.size(), max_visits);
    write_line(q.dump());
}

// ── Result polling ────────────────────────────────────────────────────────────

bool KatagoProc::poll_ownership(int out[][MAX_BOARD_SIZE], int& board_size_out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!result_ready_) return false;
    result_ready_ = false;
    board_size_out = result_bs_;
    memcpy(out, result_own_, sizeof(result_own_));
    return true;
}

bool KatagoProc::poll_moves(MoveSuggestion out[], int& count_out, float& score_lead_out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!moves_ready_) return false;
    moves_ready_ = false;
    count_out = moves_count_;
    score_lead_out = moves_score_lead_;
    memcpy(out, moves_result_, moves_count_ * sizeof(MoveSuggestion));
    return true;
}

// ── Reader thread ─────────────────────────────────────────────────────────────

void KatagoProc::reader_loop() {
    std::string buf;
    char tmp[8192];

    kata_log("reader thread running");
    while (running_.load()) {
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe(h_read_, nullptr, 0, nullptr, &avail, nullptr)) {
            DWORD peek_err = GetLastError();
            DWORD exit_code = 0;
            GetExitCodeProcess(proc_.hProcess, &exit_code);
            kata_logf("reader: PeekNamedPipe failed (error %lu) exit_code=%lu — process crashed",
                      peek_err, exit_code);
            break;
        }
        if (avail == 0) { Sleep(10); continue; }
        DWORD nread = 0;
        if (!ReadFile(h_read_, tmp, sizeof(tmp) - 1, &nread, nullptr) || nread == 0) {
            kata_log("reader: ReadFile returned 0 — pipe closed");
            break;
        }
        tmp[nread] = '\0';
        buf.append(tmp, nread);
#endif

        // Process all complete newline-terminated lines
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty()) continue;
            if (line[0] != '{') {
                // KataGo startup / error output (not JSON) — log it verbatim
                kata_logf("katago: %s", line.c_str());
                continue;
            }
            try {
                auto d = json::parse(line);

                // Match query id to avoid stale results
                int resp_id = 0;
                if (d.contains("id") && d["id"].is_string())
                    resp_id = std::stoi(d["id"].get<std::string>());

                bool do_push = false;

                // ── Ownership response (stone removal phase) ──────────────
                if (d.contains("ownership") && d["ownership"].is_array()) {
                    auto& ow = d["ownership"];
                    int n = int(ow.size());
                    int bs = 0;
                    for (int s = 2; s <= MAX_BOARD_SIZE; s++) if (s * s == n) { bs = s; break; }
                    if (bs > 0) {
                        std::lock_guard<std::mutex> lk(mu_);
                        if (resp_id == pending_response_id_) {
                            result_bs_ = bs;
                            memset(result_own_, 0, sizeof(result_own_));
                            for (int r = 0; r < bs; r++)
                                for (int c = 0; c < bs; c++) {
                                    float v = ow[kata_idx(r, c, bs)].get<float>();
                                    result_own_[r][c] = (v > 0.5f) ? 1 : (v < -0.5f) ? -1 : 0;
                                }
                            result_ready_ = true;
                            do_push = true;
                            kata_logf("ownership response id=%d bs=%d OK", resp_id, bs);
                        } else {
                            kata_logf("ownership response id=%d stale (want %d) — discarded",
                                      resp_id, pending_response_id_);
                        }
                    } else {
                        kata_logf("ownership response id=%d: unrecognised array size %d", resp_id, n);
                    }
                }

                // ── Move-suggestion response (history review) ─────────────
                if (d.contains("moveInfos") && d["moveInfos"].is_array()) {
                    auto& mi = d["moveInfos"];
                    int bs = 0;
                    MoveSuggestion tmp[MAX_SUGGESTIONS];
                    int count = 0;
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        if (resp_id == pending_response_id_) {
                            bs = pending_query_bs_;
                            // rootInfo.scoreLead is from Black's absolute perspective
                            // (positive = Black leading, negative = White leading).
                            float sl = FLT_MAX;
                            if (d.contains("rootInfo") && d["rootInfo"].is_object()) {
                                auto& ri = d["rootInfo"];
                                if (ri.contains("scoreLead") && ri["scoreLead"].is_number())
                                    sl = ri["scoreLead"].get<float>();
                            }
                            moves_score_lead_ = sl;
                            for (auto& info : mi) {
                                if (count >= MAX_SUGGESTIONS) break;
                                if (!info.contains("move") || !info["move"].is_string()) continue;
                                int row, col;
                                if (!from_gtp(info["move"].get<std::string>(), bs, row, col)) continue;
                                MoveSuggestion s{};
                                s.row     = row;
                                s.col     = col;
                                s.winrate    = info.value("winrate",    0.5f);
                                s.score_lead = info.value("scoreLead",  0.f);
                                s.order      = info.value("order",      count);
                                // PV: KataGo includes the move itself as pv[0]; skip it, store continuations
                                if (info.contains("pv") && info["pv"].is_array()) {
                                    auto& pv = info["pv"];
                                    int pc = 0;
                                    bool skip_first = true;
                                    for (auto& mv : pv) {
                                        if (skip_first) { skip_first = false; continue; }
                                        if (pc >= MAX_PV || !mv.is_string()) break;
                                        int pr, pf;
                                        if (from_gtp(mv.get<std::string>(), bs, pr, pf)) {
                                            s.pv_row[pc] = pr;
                                            s.pv_col[pc] = pf;
                                            pc++;
                                        }
                                    }
                                    s.pv_count = pc;
                                }
                                tmp[count++] = s;
                            }
                            memcpy(moves_result_, tmp, count * sizeof(MoveSuggestion));
                            moves_count_ = count;
                            moves_ready_ = true;
                            do_push = true;
                            kata_logf("moves response id=%d bs=%d count=%d (raw=%d)",
                                      resp_id, bs, count, (int)mi.size());
                        } else {
                            kata_logf("moves response id=%d stale (want %d) — discarded",
                                      resp_id, pending_response_id_);
                        }
                    }
                }

                // KataGo sends {"id":..., "error":"..."} for bad queries
                if (d.contains("error") && d["error"].is_string())
                    kata_logf("katago ERROR id=%d: %s", resp_id,
                              d["error"].get<std::string>().c_str());

                if (do_push) {
                    SDL_Event ev{};
                    ev.type = g_net_event_type;
                    SDL_PushEvent(&ev);
                }

            } catch (const std::exception& e) {
                kata_logf("JSON parse error: %s  line=%.80s", e.what(), line.c_str());
            } catch (...) {
                kata_logf("unknown exception parsing JSON line=%.80s", line.c_str());
            }
        }
    }
    running_.store(false);   // process died or stop() was called — no more queries
    kata_log("reader thread exited");
}

// Drains katago's stderr on its own pipe/thread — plain text logging only, never
// touches the JSON response stream. Exits quietly once the pipe closes (normal on stop()).
void KatagoProc::stderr_reader_loop() {
#ifdef _WIN32
    std::string buf;
    char tmp[8192];
    while (running_.load()) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h_stderr_read_, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) { Sleep(20); continue; }
        DWORD nread = 0;
        if (!ReadFile(h_stderr_read_, tmp, sizeof(tmp) - 1, &nread, nullptr) || nread == 0) break;
        tmp[nread] = '\0';
        buf.append(tmp, nread);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty()) kata_logf("katago(stderr): %s", line.c_str());
        }
    }
#endif
}

// ── KataGoGtp ─────────────────────────────────────────────────────────────────

static FILE* g_gtp_log = nullptr;
static void gtp_log(const char* msg) {
    if (!g_gtp_log) g_gtp_log = fopen("kata_gtp.log", "w");
    time_t t = time(nullptr);
    struct tm* ti = localtime(&t);
    char ts[24];
    strftime(ts, sizeof(ts), "%H:%M:%S", ti);
    if (g_gtp_log) { fprintf(g_gtp_log, "[%s] %s\n", ts, msg); fflush(g_gtp_log); }
    fprintf(stderr, "[gtp] [%s] %s\n", ts, msg);
}
static void gtp_logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    gtp_log(buf);
}

// GTP column letter for col index (0-based), skipping 'I'.
static char gtp_col_letter(int col) {
    return (col < 8) ? char('A' + col) : char('A' + col + 1);
}

// Convert (row, col) in our coords to GTP string.  row=-1/col=-1 → "pass".
static std::string gtp_coord(int row, int col, int board_size) {
    if (row < 0) return "pass";
    return std::string(1, gtp_col_letter(col)) + std::to_string(board_size - row);
}

// Parse GTP response token into our (row, col).
// Returns false if unrecognised.  Sets row=-1/col=-1 for "pass", -2/-2 for "resign".
static bool parse_gtp_move(const std::string& tok, int board_size, int& row, int& col) {
    if (tok.empty()) return false;
    std::string up; for (char c : tok) up += char(toupper((unsigned char)c));
    if (up == "PASS") { row = col = -1; return true; }
    if (up == "RESIGN") { row = col = -2; return true; }
    if (up.size() < 2) return false;
    char letter = up[0];
    if (letter == 'I') return false;
    int c = (letter >= 'J') ? (letter - 'A' - 1) : (letter - 'A');
    if (c < 0 || c >= board_size) return false;
    int gtp_row;
    try { gtp_row = std::stoi(up.substr(1)); } catch (...) { return false; }
    int r = board_size - gtp_row;
    if (r < 0 || r >= board_size) return false;
    row = r; col = c;
    return true;
}

std::string KataGoGtp::make_temp_cfg(const std::string& profile) {
#ifdef _WIN32
    char tmp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_dir);
    std::string path = std::string(tmp_dir) + "kata_gtp.cfg";
#else
    std::string path = "/tmp/kata_gtp.cfg";
#endif

    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return "";

    fprintf(fp,
        "# Auto-generated by ogs_client\n"
        "logAllGTPCommunication = false\n"
        "logSearchInfo = false\n"
        "logToStderr = false\n"
        "rules = chinese\n"
        // Resignation matches KataGo's official gtp_human5k_example.cfg: a weaker
        // human keeps fighting far longer than a strong bot would, and an early-
        // resigning bot hands out free wins that make it feel weaker than its rank.
        "allowResignation = true\n"
        "resignThreshold = -0.99\n"
        "resignConsecTurns = 20\n"
        "resignMinScoreDifference = 40\n"
        "resignMinMovesPerBoardArea = 0.4\n"
        "maxVisits = 40\n"
        "numSearchThreads = 1\n"
        "lagBuffer = 1.0\n"
        "humanSLProfile = %s\n"
        "humanSLChosenMoveProp = 1.0\n"
        "humanSLChosenMoveIgnorePass = true\n"
        "humanSLChosenMovePiklLambda = 100000000\n"
        "humanSLRootExploreProbWeightless = 0.0\n"
        "humanSLRootExploreProbWeightful = 0.0\n"
        "humanSLPlaExploreProbWeightless = 0.0\n"
        "humanSLPlaExploreProbWeightful = 0.0\n"
        "humanSLOppExploreProbWeightless = 0.0\n"
        "humanSLOppExploreProbWeightful = 0.0\n"
        "humanSLCpuctExploration = 0.50\n"
        "humanSLCpuctPermanent = 0.2\n"
        // Sharper than the official example's 0.85/0.70-below-1%-only sampling.
        // Per Analysis_Engine.md "How to get stronger human-style play": setting
        // OnlyBelowProb to 1.0 and lowering the temperatures shifts the bot from
        // "realistic individual of this rank, blunders included" toward "majority
        // vote of players at this rank" — same style, but it stops gifting the
        // rank's typical blunders (Boris found the realistic setting too easy for
        // the claimed rank). Raise temps back toward 0.85/0.70 for more blunders.
        "chosenMoveTemperatureEarly = 0.65\n"
        "chosenMoveTemperature = 0.50\n"
        "chosenMoveTemperatureHalflife = 80\n"
        "chosenMoveTemperatureOnlyBelowProb = 1.0\n"
        "chosenMoveSubtract = 0\n"
        "chosenMovePrune = 0\n"
        "nnCacheSizePowerOfTwo = 17\n"
        "nnMutexPoolSizePowerOfTwo = 14\n"
        "ignorePreRootHistory = false\n"
        "analysisIgnorePreRootHistory = false\n"
        "rootNumSymmetriesToSample = 2\n"
        "useLcbForSelection = false\n"
        "winLossUtilityFactor = 1.0\n"
        "staticScoreUtilityFactor = 0.30\n"
        "dynamicScoreUtilityFactor = 0.00\n"
        "useUncertainty = false\n"
        "subtreeValueBiasFactor = 0.0\n"
        "useNoisePruning = false\n",
        profile.c_str());

    fclose(fp);
    return path;
}

bool KataGoGtp::start(const std::string& exe, const std::string& model,
                      const std::string& human_model, const std::string& profile,
                      int board_size, float komi) {
    if (running_.load()) return true;
    board_size_ = board_size;

    temp_cfg_ = make_temp_cfg(profile);
    if (temp_cfg_.empty()) { gtp_log("failed to write temp config"); return false; }

#ifndef _WIN32
    gtp_log("KataGo subprocess support is not implemented on this platform — skipping");
    return false;
#else
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_stdin_r = NULL, h_stdout_w = NULL, h_stderr_w = NULL;
    if (!CreatePipe(&h_stdin_r, &h_write_, &sa, 0))        return false;
    if (!CreatePipe(&h_read_, &h_stdout_w, &sa, 0)) {
        CloseHandle(h_stdin_r); CloseHandle(h_write_); h_write_ = NULL;
        return false;
    }
    // Separate pipe for stderr — see the matching comment in KatagoProc::start() for why
    // merging it with stdout risks corrupting the response stream.
    if (!CreatePipe(&h_stderr_read_, &h_stderr_w, &sa, 0)) {
        CloseHandle(h_stdin_r); CloseHandle(h_write_); CloseHandle(h_read_); CloseHandle(h_stdout_w);
        h_write_ = h_read_ = NULL;
        return false;
    }
    SetHandleInformation(h_write_,       HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_read_,        HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_stderr_read_, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + exe + "\" gtp"
                    + " -model \"" + model + "\""
                    + " -human-model \"" + human_model + "\""
                    + " -config \"" + temp_cfg_ + "\"";
    gtp_logf("starting: %s", cmd.c_str());
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    std::string kata_dir = exe;
    auto sep = kata_dir.find_last_of("/\\");
    if (sep != std::string::npos) kata_dir.resize(sep);
    const char* kata_cwd = kata_dir.empty() ? nullptr : kata_dir.c_str();

    STARTUPINFOA si{};
    si.cb        = sizeof(si);
    si.dwFlags   = STARTF_USESTDHANDLES;
    si.hStdInput  = h_stdin_r;
    si.hStdOutput = h_stdout_w;
    si.hStdError  = h_stderr_w;   // own pipe — see comment above

    BOOL ok = CreateProcessA(nullptr, cmd_buf.data(),
                             nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, kata_cwd,
                             &si, &proc_);
    CloseHandle(h_stdin_r);
    CloseHandle(h_stdout_w);
    CloseHandle(h_stderr_w);
    if (!ok) {
        gtp_logf("CreateProcess FAILED (error %lu)", GetLastError());
        CloseHandle(h_write_); CloseHandle(h_read_); CloseHandle(h_stderr_read_);
        h_write_ = h_read_ = h_stderr_read_ = NULL;
        return false;
    }
#endif

    running_.store(true);
    reader_        = std::thread([this]() { reader_loop(); });
    stderr_reader_ = std::thread([this]() { stderr_reader_loop(); });

    // Send initial board setup — KataGo queues these and processes after loading.
    char komi_buf[16];
    snprintf(komi_buf, sizeof(komi_buf), "%.1f", komi);
    write_line("boardsize " + std::to_string(board_size));
    write_line("komi " + std::string(komi_buf));
    write_line("clear_board");
    gtp_logf("GTP process started, board_size=%d komi=%s profile=%s",
             board_size, komi_buf, profile.c_str());
    return true;
}

void KataGoGtp::stop() {
    if (!running_.load() && !reader_.joinable()) return;
    running_.store(false);
#ifdef _WIN32
    if (h_write_) { CloseHandle(h_write_); h_write_ = NULL; }
    if (reader_.joinable()) reader_.join();
    if (stderr_reader_.joinable()) stderr_reader_.join();
    if (h_read_)        { CloseHandle(h_read_);        h_read_        = NULL; }
    if (h_stderr_read_) { CloseHandle(h_stderr_read_); h_stderr_read_ = NULL; }
    if (proc_.hProcess) { TerminateProcess(proc_.hProcess, 0);
                          CloseHandle(proc_.hProcess); proc_.hProcess = NULL; }
    if (proc_.hThread)  { CloseHandle(proc_.hThread);  proc_.hThread  = NULL; }
#endif
}

void KataGoGtp::write_line(const std::string& s) {
#ifdef _WIN32
    if (!h_write_) return;
    std::string line = s + "\n";
    DWORD written = 0;
    WriteFile(h_write_, line.c_str(), DWORD(line.size()), &written, nullptr);
    gtp_logf(">>> %s", s.c_str());
#endif
}

void KataGoGtp::send_play(int color, int row, int col, int board_size) {
    if (!running_.load()) return;
    const char* clr = (color == 1) ? "B" : "W";
    write_line(std::string("play ") + clr + " " + gtp_coord(row, col, board_size));
}

void KataGoGtp::send_undo() {
    if (!running_.load()) return;
    write_line("undo");
}

void KataGoGtp::request_genmove(int color) {
    if (!running_.load()) return;
    write_line(std::string("genmove ") + ((color == 1) ? "B" : "W"));
}

bool KataGoGtp::poll_genmove(int& row, int& col) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!move_ready_) return false;
    move_ready_ = false;
    row = move_row_;
    col = move_col_;
    return true;
}

void KataGoGtp::request_final_status() {
    if (!running_.load()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        final_pending_ = 1;
        final_ready_   = false;
    }
    write_line("final_score");
    write_line("final_status_list dead");
}

bool KataGoGtp::poll_final_status(std::string& score_out,
                                   int dead_rows[], int dead_cols[], int& dead_count_out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!final_ready_) return false;
    final_ready_   = false;
    score_out      = final_score_str_;
    dead_count_out = final_dead_count_;
    if (final_dead_count_ > 0) {
        memcpy(dead_rows, final_dead_rows_, final_dead_count_ * sizeof(int));
        memcpy(dead_cols, final_dead_cols_, final_dead_count_ * sizeof(int));
    }
    return true;
}

void KataGoGtp::reader_loop() {
    std::string buf;
    char tmp[4096];
    gtp_log("GTP reader thread running");

    while (running_.load()) {
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe(h_read_, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) { Sleep(10); continue; }
        DWORD nread = 0;
        if (!ReadFile(h_read_, tmp, sizeof(tmp) - 1, &nread, nullptr) || nread == 0) break;
        tmp[nread] = '\0';
        buf.append(tmp, nread);
#endif
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r') line.pop_back();

            gtp_logf("<<< %s", line.c_str());

            // GTP success response: "= <result>" (result may be empty)
            if (line.size() >= 2 && line[0] == '=' && line[1] == ' ') {
                std::string tok = line.substr(2);
                while (!tok.empty() && isspace((unsigned char)tok.back())) tok.pop_back();

                int fp;
                { std::lock_guard<std::mutex> lk(mu_); fp = final_pending_; }

                if (fp == 1) {
                    // "final_score" reply: e.g. "B+5.5"
                    gtp_logf("final_score response: %s", tok.c_str());
                    std::lock_guard<std::mutex> lk(mu_);
                    final_score_str_ = tok;
                    final_pending_   = 2;
                } else if (fp == 2) {
                    // "final_status_list dead" reply: space-separated GTP coords
                    int tmp_rows[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                    int tmp_cols[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                    int tmp_count = 0;
                    size_t p = 0;
                    while (p < tok.size()) {
                        while (p < tok.size() && isspace((unsigned char)tok[p])) p++;
                        size_t e = p;
                        while (e < tok.size() && !isspace((unsigned char)tok[e])) e++;
                        if (p == e) break;
                        int r = 0, c = 0;
                        if (parse_gtp_move(tok.substr(p, e - p), board_size_, r, c)
                                && r >= 0 && tmp_count < MAX_BOARD_SIZE * MAX_BOARD_SIZE) {
                            tmp_rows[tmp_count] = r;
                            tmp_cols[tmp_count] = c;
                            tmp_count++;
                        }
                        p = e;
                    }
                    gtp_logf("final_status_list dead: %d stones", tmp_count);
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        final_dead_count_ = tmp_count;
                        if (tmp_count > 0) {
                            memcpy(final_dead_rows_, tmp_rows, tmp_count * sizeof(int));
                            memcpy(final_dead_cols_, tmp_cols, tmp_count * sizeof(int));
                        }
                        final_pending_ = 0;
                        final_ready_   = true;
                    }
                    SDL_Event ev{};
                    ev.type = g_net_event_type;
                    SDL_PushEvent(&ev);
                } else if (!tok.empty()) {
                    // Normal genmove response
                    int r, c;
                    if (parse_gtp_move(tok, board_size_, r, c)) {
                        gtp_logf("genmove response: %s → (%d,%d)", tok.c_str(), r, c);
                        {
                            std::lock_guard<std::mutex> lk(mu_);
                            move_row_   = r;
                            move_col_   = c;
                            move_ready_ = true;
                        }
                        SDL_Event ev{};
                        ev.type = g_net_event_type;
                        SDL_PushEvent(&ev);
                    }
                }
            } else if (!line.empty() && line[0] == '?') {
                gtp_logf("GTP error: %s", line.c_str());
            }
        }
    }
    running_.store(false);
    gtp_log("GTP reader thread exited");
}

// Drains katago's stderr on its own pipe/thread — see KatagoProc::stderr_reader_loop().
void KataGoGtp::stderr_reader_loop() {
#ifdef _WIN32
    std::string buf;
    char tmp[8192];
    while (running_.load()) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h_stderr_read_, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) { Sleep(20); continue; }
        DWORD nread = 0;
        if (!ReadFile(h_stderr_read_, tmp, sizeof(tmp) - 1, &nread, nullptr) || nread == 0) break;
        tmp[nread] = '\0';
        buf.append(tmp, nread);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty()) gtp_logf("katago(stderr): %s", line.c_str());
        }
    }
#endif
}
