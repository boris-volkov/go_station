#pragma once
#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>

// Central colour palette for go_viewer.
// All hard-coded colours in the renderer should be pulled from here so that
// tweaking the look of the program means editing one file.

namespace Palette {

// ---------------------------------------------------------------------------
// Screen / board surfaces

constexpr SDL_Color BACKGROUND      = {52,  60,  72,  255};  // letterbox area behind the board
constexpr SDL_Color GRID            = {45,  55,  70,  255};  // grid lines (dark blue-gray)

// Board surface colour — the one entry in this file that's runtime-adjustable
// (DISPLAY settings screen, HUE/SAT/VAL rows), so it's a plain mutable `inline`
// variable rather than `constexpr` like everything else here. Never assign to
// BOARD / BOARD_ANALYSIS or BOARD_HUE/SAT/VAL directly — go through
// set_board_hue/sat/val below, or they'll drift out of sync with each other
// (and with settings.txt).
//
// 220/35/60 is (the nearest multiple of 5 to) the HSV equivalent of the
// original hard-coded board colour (95,115,150), so a fresh install renders
// close to identical to before this control existed.
inline int BOARD_HUE = 220;   // 0-359, wraps, always a multiple of 5 (see round5)
inline int BOARD_SAT = 35;    // 0-100, clamped, always a multiple of 5
inline int BOARD_VAL = 60;    // 0-100, clamped, always a multiple of 5

inline SDL_Color hsv_to_rgb(int h, int s, int v) {
    float H = (float)(((h % 360) + 360) % 360);
    float S = (float)std::max(0, std::min(100, s)) / 100.0f;
    float V = (float)std::max(0, std::min(100, v)) / 100.0f;
    float C = V * S;
    float X = C * (1.0f - std::fabs(std::fmod(H / 60.0f, 2.0f) - 1.0f));
    float m = V - C;
    float r = 0, g = 0, b = 0;
    if      (H <  60) { r = C; g = X; b = 0; }
    else if (H < 120) { r = X; g = C; b = 0; }
    else if (H < 180) { r = 0; g = C; b = X; }
    else if (H < 240) { r = 0; g = X; b = C; }
    else if (H < 300) { r = X; g = 0; b = C; }
    else              { r = C; g = 0; b = X; }
    return SDL_Color{
        (Uint8)std::lround((r + m) * 255.0f),
        (Uint8)std::lround((g + m) * 255.0f),
        (Uint8)std::lround((b + m) * 255.0f),
        255
    };
}

// BOARD_ANALYSIS keeps the same hue and saturation as BOARD, just brighter —
// the same "clearly related, but distinguishable" relationship the original
// hard-coded pair had. Computed straight from the HSV defaults above, so
// there's no separate RGB literal to keep in sync by hand.
inline SDL_Color BOARD           = hsv_to_rgb(BOARD_HUE, BOARD_SAT, BOARD_VAL);
inline SDL_Color BOARD_ANALYSIS  = hsv_to_rgb(BOARD_HUE, BOARD_SAT, std::min(100, BOARD_VAL + 10));

inline void apply_board_hsv() {
    BOARD          = hsv_to_rgb(BOARD_HUE, BOARD_SAT, BOARD_VAL);
    BOARD_ANALYSIS = hsv_to_rgb(BOARD_HUE, BOARD_SAT, std::min(100, BOARD_VAL + 10));
}

// Nearest multiple of 5, for v >= 0 (both callers below clamp/wrap to a
// non-negative range first).
inline int round5(int v) { return ((v + 2) / 5) * 5; }

// Locked to multiples of 5 — the DISPLAY settings rows step by 5, and snapping
// here (rather than just relying on 5-at-a-time stepping from a multiple-of-5
// default) also catches a hand-edited settings.txt landing off the grid.
inline void set_board_hue(int h) {
    h = ((h % 360) + 360) % 360;
    BOARD_HUE = round5(h) % 360;   // round5(359) rolls over to 360 -> wrap to 0
    apply_board_hsv();
}
inline void set_board_sat(int s) { BOARD_SAT = round5(std::max(0, std::min(100, s))); apply_board_hsv(); }
inline void set_board_val(int v) { BOARD_VAL = round5(std::max(0, std::min(100, v))); apply_board_hsv(); }

// ---------------------------------------------------------------------------
// Stones

constexpr SDL_Color STONE_BLACK     = {30,  30,  30,  255};
constexpr SDL_Color STONE_WHITE     = {240, 240, 240, 255};
constexpr SDL_Color STONE_OUTLINE   = {80,  80,  80,  255};  // subtle ring on white stones

// ---------------------------------------------------------------------------
// UI accent

// Primary accent — mode-status labels, cursor ring, crosshair arms.
// A soft warm yellow that reads clearly on the blue-gray board without
// being as harsh as a fully-saturated yellow.
constexpr SDL_Color ACCENT          = {255, 255, 180, 255};

// ---------------------------------------------------------------------------
// Text

constexpr SDL_Color TEXT_PRIMARY    = {230, 230, 230, 255};  // player names
constexpr SDL_Color TEXT_SECONDARY  = {200, 200, 200, 255};  // secondary labels / help descriptions
constexpr SDL_Color TEXT_DIM        = {120, 120, 120, 255};  // prisoner counts, game year
constexpr SDL_Color TEXT_WHITE      = {255, 255, 255, 255};  // brightest white (help overlay title, catalog)

// ---------------------------------------------------------------------------
// Overlays / panels

constexpr SDL_Color OVERLAY_DARK    = {30,  30,  30,  210};  // help overlay background
constexpr SDL_Color OVERLAY_MID     = {80,  80,  80,  190};  // catalog overlay background
constexpr SDL_Color OVERLAY_SPEED   = {80,  80,  80,  180};  // speed-change flash

// ---------------------------------------------------------------------------
// Interactive / game elements

// Freehand chalk annotation. Warm off-white rather than pure white so it reads as
// chalk on a slate board, and bright enough to stay legible over stones of either
// colour. A single solid stroke — no halo.
constexpr SDL_Color CHALK           = {250, 247, 232, 255};
// Dark chalk (SHIFT toggles to it) — a soft near-black rather than pure #000, so it
// still reads as a drawn stroke over the board rather than a hole punched in it.
constexpr SDL_Color CHALK_DARK      = {18,  16,  14,  255};

constexpr SDL_Color CATALOG_SELECT  = {40,  120, 255, 190};  // catalog highlighted row
// Mouse-hover row tint: same hue as the selection but far weaker, so "the pointer
// is here" never reads as "this is selected" (hover does not commit a selection).
constexpr SDL_Color CATALOG_HOVER   = {40,  120, 255, 60};
constexpr SDL_Color LIBERTY_DOT     = {220, 50,  50,  200};  // liberty indicator dots
constexpr SDL_Color BOX_SELECT      = {255, 255, 180, 200};  // box-selection highlight (matches ACCENT)

// ---------------------------------------------------------------------------
// Popup menu sections
//
// The lobby menu grew long enough that a flat list of a dozen commands is hard
// to scan, so its rows are grouped under headings. Each section gets its own
// hue: the heading wears it at full strength and its commands wear a muted
// version, so a group reads as one block at a glance. The tints stay well
// short of ACCENT's brightness on purpose — the highlighted row has to remain
// the loudest thing on the panel no matter which section it sits in.

constexpr SDL_Color MENU_PLAY       = {120, 195, 255, 255};  // start or resume a game
constexpr SDL_Color MENU_TRAIN      = {140, 225, 155, 255};  // puzzles, quiz, joseki
constexpr SDL_Color MENU_REVIEW     = {235, 190, 120, 255};  // saved games and analysis
constexpr SDL_Color MENU_SETTINGS   = {200, 170, 235, 255};  // settings screens
constexpr SDL_Color MENU_SCREEN     = {175, 175, 185, 255};  // drawing and quit

// A section's command rows: the heading's hue pulled two thirds of the way back
// to ordinary text, so the tint groups the rows without making any of them
// compete with the selection.
constexpr Uint8 mix_channel(Uint8 a, Uint8 b, int num, int den) {
    return (Uint8)((a * num + b * (den - num)) / den);
}
constexpr SDL_Color menu_item_tint(SDL_Color s) {
    return {mix_channel(s.r, TEXT_SECONDARY.r, 1, 3),
            mix_channel(s.g, TEXT_SECONDARY.g, 1, 3),
            mix_channel(s.b, TEXT_SECONDARY.b, 1, 3), 255};
}

// ---------------------------------------------------------------------------
// Territory drill

constexpr SDL_Color TERRITORY_BLACK = {100, 100, 100, 180};  // black territory shading
constexpr SDL_Color TERRITORY_WHITE = {200, 200, 200, 180};  // white territory shading
constexpr SDL_Color SCORE_CORRECT   = {100, 220, 100, 255};  // correct-answer feedback (green)
constexpr SDL_Color SCORE_TEXT      = {230, 230, 230, 255};  // score label
constexpr SDL_Color PROJECTED       = { 10, 210, 150, 255};  // KataGo projected score (matches suggestion circles)

} // namespace Palette
