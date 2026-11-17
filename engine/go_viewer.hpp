#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <SDL2/SDL.h>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cstdio>

static constexpr int BOARD_SIZE         = 19;   // standard game size
static constexpr int MAX_BOARD_SIZE     = 30;   // largest board free mode can use
static constexpr int SCREEN_SIZE        = 800;
static constexpr int MAX_MOVES          = 1000;
static constexpr int MOVE_TEXT_LEN      = 8;
static constexpr int MOVE_DELAY_MS      = 4000;
static constexpr int MOVE_DELAY_MIN_MS  = 200;
static constexpr int MOVE_DELAY_MAX_MS  = 3600000;  // 1 hour — effectively no cap
static constexpr int MOVE_DELAY_STEP_MS = 1000;
static constexpr int GAME_OVER_PAUSE_MS = 20000;
static constexpr int SPEED_MESSAGE_MS   = 1500;
static constexpr int NAME_LEN           = 128;
static constexpr int RESULT_LEN         = 16;

#ifdef _WIN32
static constexpr char        PATH_SEP     = '\\';
static constexpr const char* PATH_SEP_STR = "\\";
#else
static constexpr char        PATH_SEP     = '/';
static constexpr const char* PATH_SEP_STR = "/";
#endif

static const char DEFAULT_GAMES_DIR[] = "games/";

struct Stone {
    int r = 0, f = 0, is_black = 0;
};

static constexpr int MAX_PV = 8;

struct MoveSuggestion {
    int   row        = -1;
    int   col        = -1;
    float winrate    = 0.f;
    float score_lead = 0.f;
    int   order      = 0;
    int   pv_count = 0;
    int   pv_row[MAX_PV] = {};
    int   pv_col[MAX_PV] = {};
};
static constexpr int MAX_SUGGESTIONS = 10;

struct MoveNumCell {
    int num      = 0;  // 1-based move number, 0 = no move played here
    int is_black = 0;
};

// Complete snapshot of board + stone-tracking arrays (used for history / analysis init)
struct GameSnapshot {
    char  board[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    Stone stones[MAX_MOVES]                    = {};
    int   stone_count                          = 0;
    int   black_prisoners                      = 0;
    int   white_prisoners                      = 0;
    int   turn_is_black                        = 1;
};

struct BoardView {
    int active_size = BOARD_SIZE;  // actual grid size being used (19 for games, variable in free mode)
    int square   = 0;
    int margin   = 0;  // padding between board background edge and first grid line
    int offset_x = 0, offset_y = 0;  // pixel position of the top-left grid intersection
    int screen_w = 0, screen_h = 0;
    int board_px = 0;  // grid area in pixels (square * active_size)
    int orient   = 0;  // display orientation: ORIENT_* bits (see below)
};

// Board display orientation. A board point (r, f) is shown at display cell
// (dr, df) obtained by: transpose (swap r/f), then mirror columns, then mirror
// rows — each step only when its bit is set. The eight combinations cover every
// rotation and reflection of the square. Only rendering and hit-testing see it;
// all game state stays in true board coordinates.
enum : int { ORIENT_TRANSPOSE = 1, ORIENT_FLIP_H = 2, ORIENT_FLIP_V = 4 };

// Compose a further transform on top of an existing orientation.
inline int orient_flip_h(int o) { return o ^ ORIENT_FLIP_H; }
inline int orient_flip_v(int o) { return o ^ ORIENT_FLIP_V; }
inline int orient_rotate_cw(int o) {
    // Rotating 90° clockwise = transpose then mirror columns, applied after o.
    // Pushing it through: T'=!T, H'=!V, V'=H.
    int t = o & ORIENT_TRANSPOSE, h = (o & ORIENT_FLIP_H) != 0, v = (o & ORIENT_FLIP_V) != 0;
    return (t ^ ORIENT_TRANSPOSE) | (v ? 0 : ORIENT_FLIP_H) | (h ? ORIENT_FLIP_V : 0);
}

// Board point -> display cell
inline void orient_to_display(const BoardView& v, int r, int f, int& dr, int& df) {
    int n = v.active_size;
    dr = r; df = f;
    if (v.orient & ORIENT_TRANSPOSE) { dr = f; df = r; }
    if (v.orient & ORIENT_FLIP_H)    df = n - 1 - df;
    if (v.orient & ORIENT_FLIP_V)    dr = n - 1 - dr;
}

// Display cell -> board point (inverse of orient_to_display)
inline void display_to_board(const BoardView& v, int dr, int df, int& r, int& f) {
    int n = v.active_size;
    if (v.orient & ORIENT_FLIP_V)    dr = n - 1 - dr;
    if (v.orient & ORIENT_FLIP_H)    df = n - 1 - df;
    r = dr; f = df;
    if (v.orient & ORIENT_TRANSPOSE) { r = df; f = dr; }
}

// Pixel centre of board point (r, f), honouring the view's orientation
inline void cell_center(const BoardView& v, int r, int f, int& cx, int& cy) {
    int dr, df;
    orient_to_display(v, r, f, dr, df);
    cx = v.offset_x + df * v.square + v.square / 2;
    cy = v.offset_y + dr * v.square + v.square / 2;
}

struct Overlay {
    bool  active   = false;
    int   is_black = 0;
    float x = 0.f, y = 0.f;
    int   skip_r1 = -1, skip_f1 = -1;
};
