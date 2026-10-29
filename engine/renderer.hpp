#pragma once
#include "go_viewer.hpp"
#include "game_state.hpp"
#include "analysis_state.hpp"
#include "catalog.hpp"
#include <cfloat>
#include <string>
#include <vector>

// Face-button glyphs: draw_text() renders these placeholder characters as the
// connected controller's button symbols (see font_glyphs / face-button rendering
// in renderer.cpp). Use in control-hint strings via literal concatenation,
// e.g.  GLYPH_PS_CROSS "=SELECT". The chars encode the button *position* (SDL's
// A/B/X/Y positional layout), not a fixed shape — the actual symbol drawn depends
// on Renderer::pad_style():
//   ~  = bottom face button  (PS cross,   Xbox A, Switch B)  — confirm
//   @  = right  face button  (PS circle,  Xbox B, Switch A)  — cancel/back
//   #  = left   face button  (PS square,  Xbox X, Switch Y)
//   ^  = top    face button  (PS triangle,Xbox Y, Switch X)
// The GLYPH_PS_* names are kept for source readability; despite the "PS" they
// theme to whatever controller is connected.
#define GLYPH_PS_CROSS    "~"
#define GLYPH_PS_CIRCLE   "@"
#define GLYPH_PS_SQUARE   "#"
#define GLYPH_PS_TRIANGLE "^"

// Which controller's button symbols/labels the hints render as. Set from the
// connected pad's SDL type (see Renderer::set_pad_style); defaults to PlayStation.
// Keyboard is used when no pad is connected or the keyboard was the last input:
// face glyphs render as their mirrored key names (ENTER, B, C, M) and the
// shoulder/menu labels as keys ([ ], ", .", ESC...).
enum class PadStyle { PlayStation, Xbox, Nintendo, Keyboard };

// A letter mark ("A", "B", …) placed on a board point during analysis.
struct BoardLabel { int r = 0; int f = 0; char ch = 'A'; };

// A colored dot drawn on an empty intersection (joseki continuation markers).
struct PointMarker { int r = 0; int f = 0; SDL_Color color = {}; };

// One row's styling in a sectioned popup menu. `heading` makes the row a
// section label rather than a command: it is drawn in `color` at full strength,
// gets extra space above it, and cannot be selected by mouse or pad. Command
// rows use `color` as their resting tint and still turn ACCENT when highlighted,
// so the selection stays the loudest thing on the panel.
struct PopupStyle { SDL_Color color = {}; bool heading = false; };

// ---------------------------------------------------------------------------
// Bullseye guess scoring (go_viewer's GUESS MODE)
//
// Naming the pro's exact point is rare enough that an all-or-nothing guess is
// mostly misses, which teaches nothing. Instead a guess scores by ring distance
// from the move actually played, so "I knew it was that corner" earns credit:
//
//   ring 0 (the point itself)     10
//   ring 1 (the 8 neighbours)      5
//   ring 2 (the 16 points beyond)  1
//   anything further               0
//
// Rings are Chebyshev (square) rather than Manhattan (diamond) distance, so each
// ring is the square shell the heatmap cursor draws — what you see is what scores.
constexpr int GUESS_RING_POINTS[3] = {10, 5, 1};
constexpr int GUESS_MAX_POINTS     = GUESS_RING_POINTS[0];

inline int guess_ring_dist(int r1, int f1, int r2, int f2) {
    int dr = r1 - r2; if (dr < 0) dr = -dr;
    int df = f1 - f2; if (df < 0) df = -df;
    return dr > df ? dr : df;
}
inline int guess_points(int dist) {
    return (dist >= 0 && dist < 3) ? GUESS_RING_POINTS[dist] : 0;
}

// Describes one node in the analysis tree for rendering (produced by ogs_client,
// consumed by render_analysis_tree — a plain POD so renderer.hpp stays dependency-free).
struct AnalysisTreeRenderNode {
    int  depth        = 0;   // row in tree (0 = root)
    int  col          = 0;   // column (0 = main trunk, 1+ = branches)
    bool current      = false;
    int  parent_depth = -1;  // -1 for root
    int  parent_col   = 0;
    int  move_color   = -1;  // 1=black stone, 0=white stone, -1=root (no move)
    bool marked       = false; // flagged for special analysis attention (full-row highlight)
    bool goal         = false; // solution endpoint (puzzle trees) — green halo on the node
};

class Renderer {
public:
    SDL_Renderer* sdl = nullptr;

    explicit Renderer(SDL_Renderer* r) : sdl(r) {}
    ~Renderer() {
        if (board_cache_) SDL_DestroyTexture(board_cache_);
        if (annot_layer_) SDL_DestroyTexture(annot_layer_);
    }

    // All draw state needed for one frame
    struct DrawState {
        const GameState&      game;
        const AnalysisState*  analysis;      // null when not in analysis mode
        bool  analysis_mode  = false;
        bool  game_mode      = false;
        bool  guess_mode     = false;
        int   guess_score    = 0;
        // Bullseye guessing: a guess scores by how close it lands to the move the pro
        // actually played, so being in the right area is worth something even when the
        // exact point is missed. See guess_points() for the rings.
        int   guess_count       = 0;   // guesses made this session
        int   guess_last_points = -1;  // points from the most recent guess, -1 = none yet
        int   guess_last_dist   = -1;  // ring distance of that guess, -1 = none yet
        // Bullseye heatmap: the point its rings are centred on, -1 = don't draw.
        // Sourced from whatever that app uses to aim — go_viewer snaps it from the
        // mouse, ogs_client passes its D-pad board cursor — so the engine draws the
        // rings the same way for both without knowing which is which.
        int   guess_heat_r      = -1;
        int   guess_heat_f      = -1;
        // Snap the software stone cursor onto guess_heat_r/f instead of letting it
        // float at the raw pointer (go_viewer, where the mouse is what aims).
        bool  cursor_snap_heat  = false;
        bool  chain_mode         = false;
        bool  free_mode          = false;   // demo mode: board only, no HUD labels
        int   active_board_size  = BOARD_SIZE;  // passed to get_board_view
        bool  show_help      = false;
        const Catalog&        catalog;
        // True while `catalog` is the read-only pro game library (ogs_client) —
        // render_catalog_overlay shows this in the title and disables the
        // "delete" hint; go_viewer's own catalog never sets this.
        bool                  catalog_readonly = false;
        // Freehand chalk mode is armed — shows the on-screen reminder, since in this
        // mode a click draws instead of placing a stone.
        bool                  draw_mode        = false;
        bool                  draw_dark        = false;  // chalk colour: dark, not white
        const std::string&    black_name;
        const std::string&    white_name;
        const std::string&    result_message;
        const std::string&    game_date;
        const std::string&    game_comment;
        int   move_delay_ms          = MOVE_DELAY_MS;
        Uint32 speed_message_until   = 0;
        bool  suppress_present       = false;
        // Territory estimation drill
        bool  territory_drill    = false;
        const char (*territory_board)[MAX_BOARD_SIZE] = nullptr;
        int   territory_b_score  = 0;
        int   territory_w_score  = 0;
        bool  territory_answered = false;
        bool  territory_correct  = false;
        // "Who won?" quiz (ogs_client): a finished professional position with the
        // result hidden until the player guesses black or white. Drawn over a
        // free_mode board (no HUD), so the overlay owns the whole presentation —
        // it reads the players/result/date from black_name, white_name,
        // result_message and game_date, which stay blank until answered.
        bool  who_won_active   = false;
        bool  who_won_answered = false;
        bool  who_won_correct  = false;
        int   who_won_right    = 0;   // correct answers so far this session
        int   who_won_total    = 0;   // questions answered so far
        // Stone visibility filter for playback (hold B or W key)
        int   stone_filter = 0;   // 0=all, 1=black only, 2=white only
        // Software cursor (drawn by renderer, bypasses OS DPI scaling entirely)
        int   cursor_x    = -1;
        int   cursor_y    = -1;
        int   cursor_type = 0;  // 0=hidden, 1=crosshair, 2=white stone, 3=black stone
        // Crosshair tint. Alpha 0 means "unset" — the crosshair keeps its default
        // accent colour. ogs_client sets it to the armed chalk colour in drawing
        // mode, so the cursor itself says which colour you're about to draw with.
        SDL_Color cursor_color = {0, 0, 0, 0};
        bool  show_move_numbers = false;  // overlay move-order numbers on stones
        // Playback: raw SGF arrays (renderer builds grid, independent of captures)
        const char (*sgf_moves)[MOVE_TEXT_LEN] = nullptr;
        const int*  sgf_colors     = nullptr;
        int         sgf_game_index = 0;
        // Analysis: persistent grids maintained by App, never modified on capture
        const int (*analysis_num_grid)[MAX_BOARD_SIZE] = nullptr;
        const int (*analysis_col_grid)[MAX_BOARD_SIZE] = nullptr;
        bool  quit_confirm   = false;
        // Box selection (shift+drag, additive)
        const bool (*box_sel_pts)[MAX_BOARD_SIZE] = nullptr;
        int   box_sel_count  = 0;
        bool  box_drag_active = false;
        int   box_drag_r1 = 0, box_drag_f1 = 0;  // drag start
        int   box_drag_r2 = 0, box_drag_f2 = 0;  // drag end (current mouse)
        // Catalog thumbnails: opening (first N moves) and final position
        bool        catalog_thumb_valid       = false;
        bool        catalog_thumb_single      = false;  // opening == final (e.g. a
                                                        // saved position) — draw one
                                                        // larger board instead of two
        const char (*catalog_thumb_open) [BOARD_SIZE] = nullptr;
        const char (*catalog_thumb_final)[BOARD_SIZE] = nullptr;
        int         catalog_thumb_board_size  = BOARD_SIZE;
        // Transient flash notification (e.g. save confirmation)
        const std::string& flash_message;
        Uint32 flash_message_until = 0;
        // Save-position text input (0=off, 1=name, 2=note)
        int                save_input_step = 0;
        const std::string& save_input_buf;

        // Live game (ogs_client) — all default to off; go_viewer never sets these.
        bool        live_mode       = false;
        int         live_cursor_r   = -1;   // board cursor row; -1 = not shown
        int         live_cursor_f   = -1;   // board cursor col
        int         live_my_color   = 1;    // 1=black, 0=white (for cursor tint)
        bool        live_my_turn    = false;
        int         live_black_secs        = -1;  // -1 = no clock displayed
        int         live_white_secs        = -1;
        int         live_black_periods     = -1;  // -1 = not byo-yomi
        int         live_white_periods     = -1;
        int         live_black_period_secs = -1;
        int         live_white_period_secs = -1;
        bool        live_black_in_byo      = false;  // main time gone, byo-yomi period counting
        bool        live_white_in_byo      = false;
        const char* live_status            = nullptr;
        // Stone removal overlay (STONE_REMOVAL phase only)
        const bool (*live_dead_stones)[MAX_BOARD_SIZE] = nullptr;  // greyed-out stones
        const int  (*live_ownership)[MAX_BOARD_SIZE]   = nullptr;  // territory: 1=black, -1=white
        // Chinese-style fill view: territory (and cells vacated by dead stones) drawn
        // as full stones of the owning color instead of small marker squares.
        bool        live_ownership_fill    = false;
        // Chinese-style area score for a filled board: how many points each colour
        // holds once every territory point has become a stone. Drawn in the player
        // panel in place of the clock. -1 = no count, panel unchanged.
        int         live_area_black        = -1;
        int         live_area_white        = -1;
        // KataGo move suggestions (GAME_OVER history review)
        const MoveSuggestion* live_suggestions        = nullptr;
        int                   live_suggestion_count   = 0;
        int                   live_hovered_suggestion = -1;  // index into live_suggestions, -1 = none
        // Ko violation: cursor flashes red when the move would recreate a prior board state
        bool                  live_cursor_ko          = false;
        // KataGo expected score lead from Black's perspective; FLT_MAX = not available
        float                         live_kata_score_lead         = FLT_MAX;
        // Actual game move from current analysis position (children[0]); -1 = none/pass
        int   live_actual_move_r     = -1;
        int   live_actual_move_f     = -1;
        float live_actual_move_score = FLT_MAX;  // KataGo score_lead for that move; FLT_MAX = unknown
        // Post-game analysis tree (GAME_OVER) — null when not in analysis
        const AnalysisTreeRenderNode* live_analysis_tree          = nullptr;
        int                           live_analysis_tree_count     = 0;
        int                           live_analysis_tree_cur_depth = 0;
        // Score graph — KataGo score_lead (Black's perspective) per main-line depth
        const float* live_score_graph     = nullptr;   // FLT_MAX = no data for that depth
        int          live_score_graph_len = 0;
        int          live_score_graph_cur = 0;          // current depth → yellow scan line
        // Last-played stone — row/col highlight crosshair (-1 = none)
        int          live_last_move_r = -1;
        int          live_last_move_f = -1;
        // Board-edge coordinate labels (toggled by RT during live play)
        bool         live_show_coords = false;
        // Letter labels placed with circle during analysis (per-position, live client)
        const BoardLabel* live_labels      = nullptr;
        int               live_label_count = 0;
        // Big result banner (e.g. "W+42.5") drawn above the status line in double
        // scale — local-game score screen ("result big, prompt normal")
        const char*       live_result_banner = nullptr;
        // Square stone mode: board stones render as beveled tiles instead of
        // shaded circles (settings menu DISPLAY toggle; off = classic round)
        bool              square_stones = false;
        // Octagon stone mode: beveled octagonal tiles sized to the full cell, so
        // orthogonal neighbours meet flat edge to flat edge and diagonal ones leave
        // a small square of board showing — the truncated-square tiling (settings
        // menu DISPLAY toggle). Takes precedence over square_stones if both are on.
        bool              octagon_stones = false;
        // Square grid mode: points sit at the centre of a checkerboard cell
        // instead of at a crossing of grid lines (settings menu DISPLAY toggle).
        // Purely how render_board_content draws the board underneath — every
        // stone/marker/cursor already renders at the same point-centre pixel
        // regardless of this flag, so nothing else needs to know about it.
        bool              square_grid = false;
        // Tetris capture mode: stones removed by a capture blink a few times and
        // vanish, like a completed line in the early Tetris games (settings menu
        // DISPLAY toggle). The renderer spots captures itself by diffing the
        // board between frames, so no caller has to report them; the animation is
        // drawn over the cached board and never touches the cache.
        bool              tetris_captures = false;
        // OGS puzzle solving: solution tree occupies the left panel (via the
        // live_analysis_tree fields), so the comment box moves to the RIGHT
        // gutter and the player labels/clocks are suppressed.
        bool              puzzle_mode = false;
        // START popup action menu (live client): a small centered command list
        // drawn over the dimmed screen, outside the board cache. popup_items
        // points at popup_count display labels; popup_index is the highlighted row.
        const std::string* popup_items = nullptr;
        int                popup_count = 0;
        int                popup_index = 0;
        const char*        popup_title = nullptr;
        // Optional per-row section styling; nullptr draws the plain flat list.
        const PopupStyle*  popup_styles = nullptr;
        // Colored continuation dots (joseki explorer) — drawn on empty points,
        // outside the board cache like the cursor
        const PointMarker* live_markers      = nullptr;
        int                live_marker_count = 0;
    };

    // Match search settings menu (live client only) — also doubles as a general
    // settings menu accessible mid-game (see `ingame`), for display toggles that
    // don't belong in the network-facing MatchPrefs payload.
    struct MatchMenu {
        int  focus_col    = 0;     // 0=board size, 1=time control or strength, 2=display
        int  focus_row    = 0;     // row within focused column
        bool size_sel[3]  = {};    // 9x9, 13x13, 19x19
        bool speed_sel[3] = {};    // fast/blitz, medium/rapid, slow/live  (OGS mode)
        bool katago_mode  = false; // true = this is the KATAGO SETTINGS screen
        int  katago_str   = 2;     // strength index 0-7 fixed ranks, 8 = adaptive (default 10k)
        int  katago_size  = 2;     // local-play board size: 0=9x9, 1=13x13, 2=19x19
        std::string adaptive_label; // display text for the adaptive row, e.g. "ADAPTIVE (8 KYU)"
        bool show_coords_sel = false; // mirrors App::show_coords_ while the menu is open
        bool analysis_sel    = false; // mirrors App::kata_analysis_enabled_ while the menu is open
        bool analysis_available = true; // false = no KataGo process running at all —
                                         // ENGINE ANALYSIS row renders greyed out, inert
        bool chain_sel       = false; // mirrors App::chain_mode_ (visual links between chained stones)
        bool square_sel      = false; // mirrors App::square_stones_ (tile-style stones)
        bool octagon_sel     = false; // mirrors App::octagon_stones_ (tessellating octagon tiles)
        bool square_grid_sel = false; // mirrors App::square_grid_ (checkerboard cell layout)
        bool territory_sel      = false; // mirrors App::show_live_ownership_ (live territory overlay)
        bool territory_available = true; // false = no KataGo process running — greyed out, inert
        bool next_move_sel   = false; // mirrors App::show_next_move_ (yellow actual-move ring in review)
        bool tetris_sel      = false; // mirrors App::tetris_captures_ (blink-and-vanish capture animation)
        bool ingame       = false; // opened mid-game — hide search/mode controls, board
                                   // size/speed shown read-only for reference only
    };
    void draw_match_menu(const MatchMenu& menu);

    // Full-screen scrollable list with a highlighted row and a footer hint line —
    // generic (used by the OGS puzzle browser). Presents the frame itself unless
    // the caller wants to layer more on top first (present = false).
    // line_colors: optional per-row text-color overrides, parallel to `lines`;
    // alpha 0 = no override (row keeps the normal white/accent scheme).
    // `hover` is the mouse-hovered row (-1 = none): tinted more faintly than the
    // selected row, because hovering deliberately does not commit a selection.
    void draw_list_screen(const char* title, const std::vector<std::string>& lines,
                          int index, const char* footer, bool present = true,
                          const SDL_Color* line_colors = nullptr, int hover = -1);

    // The login prompt's own screen. The bitmap font is uppercase-only (lowercase
    // folds to the same glyph), so capitals are drawn in the accent colour to make
    // case visible when verifying a typed password. step: 1=username, 2=password;
    // `typed` is the field being edited, `username` the committed first field.
    void draw_credential_screen(int step, const std::string& username,
                                const std::string& typed, const char* footer);

    // Hit-test the list draw_list_screen would draw for the same total/index.
    // Returns the line index under (mx,my), or -1.
    int  list_screen_item_at(int total, int index, int mx, int my) const;

    // Hit-test the catalog against the layout it was last drawn with. Returns the
    // entry index under (mx,my), or -1 (also -1 if it hasn't been drawn yet).
    // Cheap enough to call on every mouse-motion event; see drawn_catalog_layout_.
    int  catalog_entry_at(int mx, int my) const;

    // Hit-test the settings menu: reports which (column, row) cell a point falls in.
    // Geometry only — it does not know how many rows a column actually has, so the
    // caller validates `row` against its own column sizes (the single authority for
    // which controls exist in the current mode).
    bool match_menu_cell_at(const MatchMenu& menu, int mx, int my,
                            int& col, int& row) const;

    // Which part of a BOARD HUE/SAT/VAL row (DISPLAY column, rows 9-11) a screen
    // point falls in: -1 = not on either button, 0 = [-], 1 = [+]. `row` must
    // already be known to be one of those three (see match_menu_hit) — this
    // only computes button geometry, it doesn't validate the row itself.
    // Recomputes the same x-offsets draw_match_menu's draw_stepper uses; the
    // two must stay in sync by hand, same as match_menu_cell_at already is
    // with the row/column layout it hit-tests.
    int match_menu_stepper_zone(const MatchMenu& menu, int row, int mx, int my) const;

    // START popup action menu: dims the current backbuffer contents and draws a
    // centered command list on top. Draw-only (no present) — draw_board layers it
    // via the DrawState popup fields; list screens call it directly and present.
    //
    // `styles`, when given, is one entry per row and groups a long menu into
    // sections. Headings are labels, not commands: they get a section gap above
    // them, no highlight band, and popup_item_at never returns one, so the
    // caller's own up/down stepping is all that has to know to skip them.
    // Passing nullptr draws the plain flat list, which is what the short menus
    // want — sections on a two-item popup would be noise.
    void draw_popup_menu(const char* title, const std::string* items,
                         int count, int index, const PopupStyle* styles = nullptr);

    // Hit-test the popup that draw_popup_menu would draw for the same arguments —
    // the mouse counterpart to it, in the same spirit as screen_to_board/board_to_screen.
    // Returns the item index under (mx,my), or -1 if the point isn't on an item.
    // `inside_panel` (optional) reports whether the point lands inside the popup box
    // at all, so callers can tell "missed a row" from "clicked outside to dismiss".
    // `styles` must match what was passed to draw_popup_menu, or the rows this
    // tests will not be the rows that were drawn.
    int  popup_item_at(const char* title, const std::string* items, int count,
                       int mx, int my, bool* inside_panel = nullptr,
                       const PopupStyle* styles = nullptr) const;

    // ── Freehand chalk annotation ────────────────────────────────────────────
    // A persistent screen-space scribble layer, as if drawn on the glass: strokes
    // are stored as pixels in their own texture, so they neither move with the
    // board position nor cost anything to keep on screen. Segments are stamped in
    // as they arrive; only an explicit clear removes them.
    // `dark` picks the near-black stroke instead of chalk white. Chosen per segment
    // because strokes are baked into the layer as pixels — already-drawn ink keeps
    // whatever colour it was made with and can't be recoloured after the fact.
    void annot_segment(int x0, int y0, int x1, int y1, bool dark = false);
    void annot_clear();
    bool annot_has_content() const { return annot_any_; }

    void get_board_view(BoardView& view, int active_size = BOARD_SIZE) const;
    bool screen_to_board(const BoardView& view, int mx, int my, int& r, int& f) const;
    void board_to_screen(const BoardView& view, int br, int bf, int& x, int& y) const;

    void draw_board(const DrawState& ds);
    // While a Tetris capture animation is playing: how long an event loop that
    // sleeps between inputs may wait before redrawing — 0, since the animation
    // advances one step per presented frame. -1 = nothing animating.
    int  capture_anim_wait_ms() const;
    // Public: also used standalone by ogs_client's drill-list preview
    void render_mini_board(int x, int y, int size, const char board[][BOARD_SIZE], int board_size = BOARD_SIZE);

private:
    // Geometry of the popup panel, derived once and shared by draw_popup_menu and
    // popup_item_at. Neither may compute these rects independently: the width depends
    // on font metrics and the scale on a screen-width threshold, so two copies of the
    // math would silently drift apart the first time either is tweaked.
    struct PopupLayout {
        SDL_Rect box{};              // the panel itself
        int  scale       = 2;
        int  text_h      = 0;        // glyph height
        int  line_gap    = 0;
        int  line_h      = 0;        // row stride (text_h + line_gap)
        int  pad         = 0;
        bool has_title   = false;
        int  first_row_y = 0;        // top of item 0's text
        int  row_x       = 0;        // left edge of a row's highlight band
        int  row_w       = 0;        // width of a row's highlight band
        // Top of each row's text. Rows are no longer a fixed stride apart once
        // section headings are in play (each one opens a gap above itself), so
        // both the draw and the hit-test read positions from here rather than
        // multiplying by line_h.
        std::vector<int> row_y;
        int  rows_h      = 0;        // total height the rows occupy
    };
    PopupLayout popup_layout(const char* title, const std::string* items, int count,
                             const PopupStyle* styles = nullptr) const;

    // Same contract for the generic full-screen list: draw_list_screen and
    // list_screen_item_at both derive their rows from this, never independently.
    struct ListLayout {
        int scale = 2, text_h = 0, line_gap = 0, line_h = 0, hpad = 0;
        int top_y     = 0;   // y of the first visible row's text
        int max_lines = 0;   // rows that fit on screen
        int scroll    = 0;   // list index of the first visible row
        int row_x     = 0, row_w = 0;
    };
    ListLayout list_screen_layout(int total, int index) const;

    // Catalog geometry. The header block above the list is conditional (button
    // legend, index-building notice, search bar) and the row width comes from a
    // measurement pass over every entry — far too much arithmetic to risk keeping
    // two copies of, so render_catalog_overlay and catalog_entry_at share this.
    struct CatalogLayout {
        int scale = 2, th = 0, line_gap = 0, pad = 0, hpad = 0, header_gap = 0, line_h = 0;
        std::string title;              // measured here, drawn by the caller
        bool index_ready    = false;
        bool show_legend    = false, show_building = false, show_search = false;
        int  title_y = 0, legend_y = 0, building_y = 0, search_y = 0, count_y = 0;
        int  list_top_y = 0;            // y of the first visible row's text
        int  max_lines  = 0, scroll = 0;
        int  max_w = 0, max_black_w = 0, max_white_w = 0;
        int  vs_w = 0, col_gap = 0, date_col_x = 0, list_right = 0;
        int  row_x = 0, row_w = 0;      // highlight / hit band
        int  total = 0;                 // entries the layout was built for
    };
    CatalogLayout catalog_layout(const BoardView& view, const Catalog& cat,
                                 bool live_mode, bool readonly) const;

    // Layout the last render_catalog_overlay actually drew with. catalog_entry_at
    // hit-tests against this rather than recomputing: the width pass walks every
    // entry building two std::strings each (>2000 entries in the big pro folders),
    // which is far too heavy to redo on every mouse-motion event. Publishing the
    // drawn layout is also the more correct answer — a click should hit the pixels
    // currently on screen — and it tracks lazily-loaded player names for free.
    CatalogLayout drawn_catalog_layout_;
    bool          drawn_catalog_layout_valid_ = false;

    // Settings-menu geometry. The per-column row origins accumulate through
    // mode-dependent headers (the mode row and the BOARD SIZE / TIME CONTROL
    // headers only exist out of game), so draw_match_menu and match_menu_cell_at
    // must read them from here rather than each walking the conditionals.
    struct MatchMenuLayout {
        int scale = 2, th = 0, line_gap = 0, line_h = 0, hpad = 0, col_w = 0;
        int display_col = 2;
        int cols_top    = 0;         // y the columns start at, before their headers
        int col_ty[3]   = {0, 0, 0}; // y of row 0 in each column
    };
    MatchMenuLayout match_menu_layout(const MatchMenu& menu) const;

    void render_board(const BoardView& view, const Overlay* overlay, const DrawState& ds);
    void draw_stone_circle(const BoardView& view, int r, int f, int is_black, Uint8 alpha, bool shadow_pass = false);
    void draw_thick_line(int x1, int y1, int x2, int y2, int thickness, SDL_Color color);
    void draw_dashed_line(int x1, int y1, int x2, int y2, int dash_len, int gap_len);
    int  text_width_px(const char* text, int scale) const;
    void draw_text(int x, int y, int scale, const char* text, SDL_Color color);

public:
    // Controller symbol/label theming for control hints (see PadStyle). Global
    // so both the shared glyph renderer and the app-side hint strings agree.
    static void     set_pad_style(PadStyle s);
    static PadStyle pad_style();
    // Rewrite the abbreviated shoulder/trigger/menu/stick labels in a hint string
    // (L1, R2, OPT, SHARE, L3...) to the current pad style's names. Face-button
    // glyphs (~ @ # ^) are untouched — draw_text themes those as it renders.
    static std::string themed_labels(const char* tmpl);
    // 5x7 bitmap for a character (public so the face-button glyph helper can reuse
    // letter glyphs for Xbox/Nintendo button symbols).
    static const unsigned char* get_glyph_rows(char c);
private:
    void draw_color_swatch(int x, int y, int size, SDL_Color fill, SDL_Color outline);

    void render_chain_connections(const BoardView& view, const char board[][MAX_BOARD_SIZE], bool chain_mode, int stone_filter, bool shadows_only = false);
    void render_all_shadows(const BoardView& view, const char board[][MAX_BOARD_SIZE], bool chain_mode, int stone_filter, int n);
    void render_liberties(const BoardView& view, const int lib_r[], const int lib_f[], int lib_count);
    void render_player_labels(const BoardView& view, const DrawState& ds);
    void render_speed_label(const BoardView& view, int delay_ms, Uint32 until);
    void render_guess_score(const BoardView& view, const DrawState& ds);
    void render_mode_status(const BoardView& view, bool analysis_mode, bool game_mode, bool guess_mode, bool territory_drill, bool paused);
    void render_territory_overlay(const BoardView& view, const DrawState& ds);
    void render_who_won_overlay(const BoardView& view, const DrawState& ds);
    void render_result_message(const BoardView& view, const DrawState& ds);
    void render_game_date(const BoardView& view, const std::string& date);
    void render_help_overlay(const BoardView& view, bool show_help, bool live_mode = false);
    void render_catalog_overlay(const BoardView& view, const DrawState& ds);
    void render_software_cursor(const BoardView& view, const DrawState& ds);
    void render_guess_heatmap(const BoardView& view, const DrawState& ds);
    void render_board_coordinates(const BoardView& view, const DrawState& ds);
    void render_quit_confirm(const BoardView& view);
    void render_box_selection(const BoardView& view, const DrawState& ds);
    void render_flash_message(const BoardView& view, const DrawState& ds);
    void render_analysis_tree(const BoardView& view, const DrawState& ds);
    void render_score_graph(const BoardView& view, const DrawState& ds);
    void render_save_input(const BoardView& view, const DrawState& ds);
    void render_game_comment(const BoardView& view, const DrawState& ds);
    void draw_stone_at_px(int cx, int cy, int radius, int is_black, Uint8 alpha);
    void shade_stone(int cx, int cy, int radius, int is_black, Uint8 alpha, bool shadow_pass = false);
    void draw_stone_link(int x1, int y1, int x2, int y2, int thickness, int is_black, bool shadow_pass = false, int stone_radius = 0);

    // Shared palette so stones and chain links always shade consistently.
    struct StoneColors {
        SDL_Color base;    // shadow tone — drawn solid as the stone base
        SDL_Color lit;     // overlay colour applied at low alpha in shade_stone
        SDL_Color dark;    // cylinder shadow-edge only (slightly darker than base)
        float     alpha1;  // broad layer alpha
        float     alpha2;  // mid layer alpha  (0 = skip)
        float     alpha3;  // tight layer alpha
    };
    static StoneColors stone_colors(int is_black);
    void fill_circle(int cx, int cy, int radius);               // scanline fill, color already set
    void draw_circle(int cx, int cy, int radius);               // Bresenham outline only, color already set
    void fill_ring(int cx, int cy, int r_outer, int r_inner);  // scanline fill of annular region only
    // Regular octagon with its four flat edges `radius` from the centre, so a
    // grid of them at `2*radius` spacing tessellates edge to edge. `octagon_dx`
    // is its row half-width — the shape's single definition, shared by the fill
    // and by the bevel shading in shade_stone.
    static int  octagon_dx(int dy, int radius);
    void fill_octagon(int cx, int cy, int radius);              // scanline fill, color already set
    // Scanline fill of a rotated ellipse. ra = semi-axis along (ux,uy), rb = semi-axis along perpendicular.
    void fill_ellipse_rotated(int cx, int cy, float ux, float uy, int ra, int rb);

    // Board cache: the board+HUD are rendered to a texture and only rebuilt when
    // board state changes.  Cursor is composited on top each frame for free.
    void render_board_content(const BoardView& view, const Overlay* overlay, const DrawState& ds);
    uint64_t compute_cache_hash(const DrawState& ds) const;

    bool square_stones_  = false;  // mirrored from DrawState each draw_board call
    bool octagon_stones_ = false;  // ditto; wins over square_stones_ if both are set
    SDL_Texture* board_cache_ = nullptr;

    // Chalk layer. Deliberately separate from board_cache_: strokes composite on
    // top of the cached board every frame, so drawing never invalidates the cache
    // and a stroke costs one short line plus a blit, not a scene rebuild.
    SDL_Texture* annot_layer_ = nullptr;
    int          annot_w_     = 0;
    int          annot_h_     = 0;
    bool         annot_any_   = false;   // anything drawn since the last clear?
    void         annot_ensure_layer();
    int          cache_w_     = 0;
    int          cache_h_     = 0;
    uint64_t     cache_hash_  = ~0ULL;   // initialised so first frame always rebuilds

    // Tetris capture animation (DrawState::tetris_captures). anim_prev_ is the
    // board as of the previous draw_board; a diff against it that reads as exactly
    // one move-with-capture starts the blink for the stones that went missing.
    struct CapturedStone { int r, f, is_black; };
    char         anim_prev_[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    int          anim_prev_n_       = 0;       // 0 = no snapshot yet
    std::vector<CapturedStone> capture_anim_;  // empty = idle
    int          capture_anim_frame_ = 0;      // frames presented since the capture
    int          capture_anim_fpp_   = 5;      // frames per blink step, from refresh rate
    void         update_capture_anim(const DrawState& ds);
    void         render_capture_anim(const BoardView& view, const DrawState& ds, bool visible);

    static const char* format_result_message(const char* sgf_result);

    struct Glyph { char c; unsigned char rows[7]; };
    static const Glyph       font_glyphs[];
};
