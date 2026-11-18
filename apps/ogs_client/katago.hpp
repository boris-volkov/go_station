#pragma once
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <cfloat>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include "go_viewer.hpp"

// Manages a KataGo analysis subprocess and queries it for territory ownership.
// Thread-safe: query_ownership() and poll_ownership() are called from the main
// thread; a background reader thread parses KataGo's JSON output.
class KatagoProc {
public:
    KatagoProc()  = default;
    ~KatagoProc() { stop(); }

    // Spawn "katago analysis -model <model> -config <cfg>".
    // Returns false if the process could not be created.
    bool start(const std::string& exe, const std::string& model, const std::string& cfg);
    void stop();
    bool running() const { return running_.load(); }

    // Send an ownership query for the current board position.
    // dead_stones[r][f] = true means that stone should be treated as removed.
    void query_ownership(const char  board[][MAX_BOARD_SIZE], int board_size,
                         const bool  dead[][MAX_BOARD_SIZE],  float komi = 7.5f,
                         int max_visits = 1);

    // Returns true if a fresh result is available, and copies it to out[][].
    // Values: +1 = black territory, -1 = white territory, 0 = neutral.
    bool poll_ownership(int out[][MAX_BOARD_SIZE], int& board_size_out);

    // Send a move-suggestion query for the given board position.
    // black_to_play: true if it is Black's turn; max_visits controls analysis depth.
    // prev_board (optional): the position one move earlier. When board differs from it
    // by exactly one placed stone, that move is sent as history so KataGo knows the
    // ko ban — without it, KataGo assumes a just-taken ko can be retaken immediately.
    void query_moves(const char board[][MAX_BOARD_SIZE], int board_size,
                     bool black_to_play, float komi = 7.5f, int max_visits = 200,
                     const char prev_board[][MAX_BOARD_SIZE] = nullptr);

    // Returns true if fresh move suggestions are ready; copies up to MAX_SUGGESTIONS
    // entries into out[] and sets count_out. score_lead_out receives KataGo's
    // expected score margin from Black's perspective (positive = Black winning),
    // or FLT_MAX if rootInfo was absent from the response.
    bool poll_moves(MoveSuggestion out[], int& count_out, float& score_lead_out);

private:
#ifdef _WIN32
    HANDLE h_write_        = NULL;   // write end of katago's stdin pipe
    HANDLE h_read_         = NULL;   // read end of katago's stdout pipe (JSON responses only)
    HANDLE h_stderr_read_  = NULL;   // read end of katago's stderr pipe (diagnostics only)
    PROCESS_INFORMATION proc_{};
#endif
    std::atomic<bool> running_{ false };
    std::thread       reader_;
    std::thread       stderr_reader_;  // drains stderr separately so it can never
                                        // interleave with and corrupt the stdout JSON stream

    std::mutex mu_;
    // Ownership result (query_ownership / poll_ownership)
    bool result_ready_                               = false;
    int  result_bs_                                  = 0;
    int  result_own_[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    // Move-suggestion result (query_moves / poll_moves)
    bool           moves_ready_                      = false;
    int            moves_count_                      = 0;
    MoveSuggestion moves_result_[MAX_SUGGESTIONS]    = {};
    // Move-suggestion: expected score lead from Black's perspective (FLT_MAX = absent)
    float moves_score_lead_                          = FLT_MAX;
    // Shared query tracking
    int  pending_query_id_                           = 0;
    int  pending_response_id_                        = 0;
    int  pending_query_bs_                           = 19;  // board size of last query

    void write_line(const std::string& s);
    void reader_loop();
    void stderr_reader_loop();
};

// ── GTP subprocess for local play vs the human SL model ──────────────────────

// Manages a KataGo GTP subprocess for local games.
// poll_genmove returns: row/col >= 0 = normal move; -1/-1 = PASS; -2/-2 = resign.
class KataGoGtp {
public:
    KataGoGtp()  = default;
    ~KataGoGtp() { stop(); }

    // Write a temp config then spawn:
    //   katago.exe gtp -model <model> -human-model <human_model> -config <temp_cfg>
    // profile: e.g. "preaz_10k". board_size and komi are sent immediately after spawn.
    bool start(const std::string& exe, const std::string& model,
               const std::string& human_model, const std::string& profile,
               int board_size, float komi = 7.5f);
    void stop();
    bool running() const { return running_.load(); }

    // Send "play B/W <coord>" (row=-1/col=-1 → "pass").  color: 1=Black, 0=White.
    void send_play(int color, int row, int col, int board_size);

    // Send GTP "undo" — undoes exactly one ply on KataGo's own internal board.
    // Call once per ply popped from our own history to keep KataGo in sync.
    void send_undo();

    // Send "genmove B/W".
    void request_genmove(int color);

    // Returns true if a move is ready.  row/col: >=0 = move; -1/-1 = pass; -2/-2 = resign.
    bool poll_genmove(int& row, int& col);

    // Send "final_score" then "final_status_list dead" to the GTP process.
    // Call once after the game ends; poll_final_status() returns when both reply.
    void request_final_status();

    // Returns true when both replies have arrived.
    // score_out: KataGo score string, e.g. "B+5.5".
    // dead_rows[]/dead_cols[]: up to MAX_BOARD_SIZE*MAX_BOARD_SIZE dead stone positions.
    bool poll_final_status(std::string& score_out,
                           int dead_rows[], int dead_cols[], int& dead_count_out);

private:
#ifdef _WIN32
    HANDLE h_write_       = NULL;
    HANDLE h_read_        = NULL;   // stdout only — GTP responses
    HANDLE h_stderr_read_ = NULL;   // stderr only — diagnostics, kept off the response stream
    PROCESS_INFORMATION proc_{};
#endif
    std::atomic<bool> running_{ false };
    std::thread reader_;
    std::thread stderr_reader_;
    std::mutex mu_;
    bool move_ready_ = false;
    int  move_row_   = 0;
    int  move_col_   = 0;
    int  board_size_ = 19;
    std::string temp_cfg_;
    // final_score / final_status_list dead state
    int         final_pending_    = 0;  // 0=idle, 1=awaiting score, 2=awaiting dead list
    bool        final_ready_      = false;
    std::string final_score_str_;
    int         final_dead_count_ = 0;
    int         final_dead_rows_[MAX_BOARD_SIZE * MAX_BOARD_SIZE] = {};
    int         final_dead_cols_[MAX_BOARD_SIZE * MAX_BOARD_SIZE] = {};

    static std::string make_temp_cfg(const std::string& profile);
    void write_line(const std::string& s);
    void reader_loop();
    void stderr_reader_loop();
};
