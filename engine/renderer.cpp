#include "renderer.hpp"
#include "palette.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Font data (5x7 pixel glyphs, MSB = leftmost column)

const Renderer::Glyph Renderer::font_glyphs[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'=', {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'*', {0x00,0x00,0x00,0x0C,0x0C,0x00,0x00}},  // centered dot — menu focus cursor
    {'"', {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}},
    {';', {0x00,0x04,0x00,0x00,0x04,0x04,0x08}},
    {'<', {0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
    {'>', {0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
    {'&', {0x08,0x14,0x14,0x08,0x15,0x12,0x0D}},
    {'$', {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}},
    {'|', {0x04,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'`', {0x08,0x04,0x00,0x00,0x00,0x00,0x00}},
    {'{', {0x06,0x04,0x04,0x08,0x04,0x04,0x06}},
    {'}', {0x0C,0x04,0x04,0x02,0x04,0x04,0x0C}},
    // PlayStation face-button shapes, reachable via the GLYPH_PS_* placeholder
    // strings (renderer.hpp). Kept vertically centered with blank top/bottom rows
    // so the four read as a matched set at any scale.
    {'~', {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}},  // cross    (Xbox A)
    {'@', {0x00,0x0E,0x11,0x11,0x11,0x0E,0x00}},  // circle   (Xbox B)
    {'#', {0x00,0x1F,0x11,0x11,0x11,0x1F,0x00}},  // square   (Xbox X)
    {'^', {0x00,0x04,0x0A,0x0A,0x11,0x1F,0x00}},  // triangle (Xbox Y)
    {'%', {0x18,0x18,0x08,0x04,0x02,0x03,0x03}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x00,0x04}},
    {',', {0x00,0x00,0x00,0x00,0x00,0x04,0x08}},
    {'\'',{0x04,0x04,0x00,0x00,0x00,0x00,0x00}},
    {'/', {0x01,0x02,0x04,0x08,0x10,0x00,0x00}},
    {'\\',{0x10,0x08,0x04,0x02,0x01,0x00,0x00}},
    {'[', {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
    {']', {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
    {'_', {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
    {'(', {0x04,0x08,0x10,0x10,0x10,0x08,0x04}},
    {')', {0x04,0x02,0x01,0x01,0x01,0x02,0x04}},
    {':', {0x00,0x04,0x00,0x00,0x04,0x00,0x00}},
    {'?', {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A', {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x10,0x13,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x01,0x01,0x01,0x01,0x01,0x11,0x0E}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
};

const unsigned char* Renderer::get_glyph_rows(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (const auto& g : font_glyphs)
        if (g.c == c) return g.rows;
    for (const auto& g : font_glyphs)
        if (g.c == '?') return g.rows;
    return font_glyphs[0].rows; // unreachable: '?' is always present
}

// The bitmap font is single-byte/ASCII only, but the strings we render (status
// separators, OGS puzzle texts and names) carry UTF-8: em/en dashes, curly quotes,
// ellipses, and CJK. Map each sequence to one display character — typographic
// punctuation to its ASCII cousin, anything unknown to a single '?' per character
// (instead of one '?' per BYTE, which turned each CJK char into ???).
// Caller guarantees p[0] != '\0'; subsequent bytes are only read after the prior
// one is confirmed non-null, so this never reads past the terminator.
static char utf8_display_char(const char* p, int& bytes) {
    unsigned char c0 = (unsigned char)p[0];
    bytes = 1;
    if (c0 < 0x80) return (char)c0;

    // U+2013..U+2026 punctuation family (E2 80 xx)
    if (c0 == 0xE2 && p[1] && (unsigned char)p[1] == 0x80 && p[2]) {
        bytes = 3;
        switch ((unsigned char)p[2]) {
        case 0x93: case 0x94: return '-';    // – —
        case 0x98: case 0x99: return '\'';   // ' '
        case 0x9C: case 0x9D: return '"';    // " "
        case 0xA2:            return '*';    // bullet
        case 0xA6:            return '.';    // …
        default:              return '?';
        }
    }
    // Any other multi-byte sequence: consume it whole, show one '?'
    if ((c0 & 0xE0) == 0xC0)      bytes = p[1] ? 2 : 1;
    else if ((c0 & 0xF0) == 0xE0) bytes = (p[1] && p[2]) ? 3 : 1;
    else if ((c0 & 0xF8) == 0xF0) bytes = (p[1] && p[2] && p[3]) ? 4 : 1;
    return '?';
}

// ---------------------------------------------------------------------------
// Board view / coordinate helpers

void Renderer::get_board_view(BoardView& view, int active_size) const {
    int w = SCREEN_SIZE, h = SCREEN_SIZE;
    SDL_GetRendererOutputSize(sdl, &w, &h);
    int min_dim      = (w < h) ? w : h;
    view.active_size = active_size;
    view.square      = min_dim / (active_size + 2);
    if (view.square < 1) view.square = 1;
    view.margin      = view.square;
    view.board_px    = view.square * active_size;
    int bg_size      = view.square * (active_size + 2);
    view.offset_x    = (w - bg_size) / 2 + view.margin;
    view.offset_y    = (h - bg_size) / 2 + view.margin;
    view.screen_w    = w;
    view.screen_h    = h;
}

void Renderer::board_to_screen(const BoardView& view, int br, int bf, int& x, int& y) const {
    x = view.offset_x + bf * view.square;
    y = view.offset_y + br * view.square;
}

bool Renderer::screen_to_board(const BoardView& view, int mx, int my, int& r, int& f) const {
    if (mx < view.offset_x || my < view.offset_y) return false;
    if (mx >= view.offset_x + view.board_px || my >= view.offset_y + view.board_px) return false;
    int rel_x = mx - view.offset_x;
    int rel_y = my - view.offset_y;
    int bf = rel_x / view.square;
    int br = rel_y / view.square;
    if (br < 0 || br >= view.active_size || bf < 0 || bf >= view.active_size) return false;
    int inset   = view.square / 8;
    int local_x = rel_x - bf * view.square;
    int local_y = rel_y - br * view.square;
    if (local_x < inset || local_x >= view.square - inset) return false;
    if (local_y < inset || local_y >= view.square - inset) return false;
    r = br; f = bf;
    return true;
}

// ---------------------------------------------------------------------------
// Primitive drawing

int Renderer::text_width_px(const char* text, int scale) const {
    int count = 0;
    for (const char* p = text; *p; count++) {
        int bytes;
        utf8_display_char(p, bytes);
        p += bytes;
    }
    if (count <= 0) return 0;
    return (count * 6 - 1) * scale;
}

// Which controller's symbols the face-button placeholder chars render as. File
// scope so the shared glyph path and the app agree; defaults to PlayStation (the
// long-standing look, and correct until a differently-typed pad connects).
static PadStyle g_pad_style = PadStyle::PlayStation;

void     Renderer::set_pad_style(PadStyle s) { g_pad_style = s; }
PadStyle Renderer::pad_style()               { return g_pad_style; }

// The four face-button placeholder chars (~ @ # ^) encode SDL's positional layout
// (bottom / right / left / top). Resolve one to the glyph bitmap + color for the
// current pad style; the face symbol always keeps its own color so it pops out of
// the surrounding prompt, matching the physical controller. Returns false for any
// non-face char (normal text).
static bool face_button_glyph(char c, const unsigned char*& rows, SDL_Color& color) {
    int pos;  // 0=bottom(confirm) 1=right(cancel) 2=left 3=top
    switch (c) {
    case '~': pos = 0; break;
    case '@': pos = 1; break;
    case '#': pos = 2; break;
    case '^': pos = 3; break;
    default:  return false;
    }
    switch (g_pad_style) {
    case PadStyle::PlayStation: {
        static const char shape[4] = {'~', '@', '#', '^'};  // cross circle square triangle
        static const SDL_Color col[4] = {
            {124, 178, 232, 255},  // cross    — blue
            {240, 105, 105, 255},  // circle   — red
            {232, 130, 190, 255},  // square   — pink
            { 80, 205, 150, 255},  // triangle — green
        };
        rows  = Renderer::get_glyph_rows(shape[pos]);
        color = col[pos];
        return true;
    }
    case PadStyle::Xbox: {
        static const char letter[4] = {'A', 'B', 'X', 'Y'};  // by position
        static const SDL_Color col[4] = {
            { 96, 192,  96, 255},  // A — green
            {228,  96,  96, 255},  // B — red
            { 96, 160, 232, 255},  // X — blue
            {236, 196,  80, 255},  // Y — amber
        };
        rows  = Renderer::get_glyph_rows(letter[pos]);
        color = col[pos];
        return true;
    }
    case PadStyle::Nintendo: {
        // Switch physical layout at each position: bottom=B, right=A, left=Y, top=X.
        // Buttons are unlit white-on-dark, so a single neutral light tone.
        static const char letter[4] = {'B', 'A', 'Y', 'X'};
        rows  = Renderer::get_glyph_rows(letter[pos]);
        color = SDL_Color{225, 225, 225, 255};
        return true;
    }
    }
    return false;
}

// Abbreviated shoulder/trigger/menu/stick labels used in hint strings, themed to
// the current pad style. Face-button glyphs (~ @ # ^) are left untouched — draw_text
// themes those as it renders. Replacement outputs share no substrings with any
// source token, so the passes can't re-trigger each other.
std::string Renderer::themed_labels(const char* tmpl) {
    std::string s(tmpl ? tmpl : "");
    if (g_pad_style == PadStyle::PlayStation) return s;  // source tokens are PS labels

    struct Sub { const char* from; const char* to; };
    static const Sub xbox[] = {
        {"L1", "LB"}, {"R1", "RB"}, {"L2", "LT"}, {"R2", "RT"},
        {"L3", "LS"}, {"R3", "RS"}, {"SHARE", "VIEW"}, {"OPT", "MENU"},
    };
    static const Sub nsw[] = {
        {"L1", "L"},  {"R1", "R"},  {"L2", "ZL"}, {"R2", "ZR"},
        {"L3", "LS"}, {"R3", "RS"}, {"SHARE", "-"}, {"OPT", "+"},
    };
    const Sub* subs = (g_pad_style == PadStyle::Xbox) ? xbox : nsw;
    int n = 8;
    for (int i = 0; i < n; i++) {
        const std::string from = subs[i].from, to = subs[i].to;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return s;
}

void Renderer::draw_text(int x, int y, int scale, const char* text, SDL_Color color) {
    SDL_SetRenderDrawColor(sdl, color.r, color.g, color.b, color.a);
    int pen_x = x;
    for (const char* p = text; *p; ) {
        int bytes;
        char c = utf8_display_char(p, bytes);
        p += bytes;
        const unsigned char* rows;
        SDL_Color fc;
        bool face = face_button_glyph(c, rows, fc);
        if (face) SDL_SetRenderDrawColor(sdl, fc.r, fc.g, fc.b, fc.a);
        else      rows = get_glyph_rows(c);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (rows[row] & (1 << (4 - col))) {
                    SDL_Rect rect = {pen_x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(sdl, &rect);
                }
            }
        }
        if (face) SDL_SetRenderDrawColor(sdl, color.r, color.g, color.b, color.a);
        pen_x += 6 * scale;
    }
}

void Renderer::draw_color_swatch(int x, int y, int size, SDL_Color fill, SDL_Color outline) {
    SDL_Rect rect = {x, y, size, size};
    SDL_SetRenderDrawColor(sdl, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(sdl, &rect);
    SDL_SetRenderDrawColor(sdl, outline.r, outline.g, outline.b, outline.a);
    SDL_RenderDrawRect(sdl, &rect);
}

void Renderer::draw_thick_line(int x1, int y1, int x2, int y2, int thickness, SDL_Color color) {
    if (thickness < 1) thickness = 1;
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    float dx  = (float)(x2 - x1);
    float dy  = (float)(y2 - y1);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        SDL_SetRenderDrawColor(sdl, color.r, color.g, color.b, color.a);
        SDL_RenderDrawPoint(sdl, x1, y1);
        return;
    }
    float nx   = dy / len;
    float ny   = -dx / len;
    float half = (float)thickness * 0.5f;
    float ox   = nx * half;
    float oy   = ny * half;
    SDL_Vertex verts[4];
    verts[0].position = {(float)x1 + ox, (float)y1 + oy};
    verts[1].position = {(float)x1 - ox, (float)y1 - oy};
    verts[2].position = {(float)x2 - ox, (float)y2 - oy};
    verts[3].position = {(float)x2 + ox, (float)y2 + oy};
    for (int i = 0; i < 4; i++) {
        verts[i].color     = color;
        verts[i].tex_coord = {0.f, 0.f};
    }
    int indices[6] = {0, 1, 2, 2, 3, 0};
    SDL_RenderGeometry(sdl, nullptr, verts, 4, indices, 6);
}

// Scanline fill of a circle — ~2*r SDL calls instead of ~π*r² point calls.
// Colour and blend mode must be set by the caller.
void Renderer::fill_circle(int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrtf((float)(radius * radius - dy * dy));
        SDL_RenderDrawLine(sdl, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

void Renderer::draw_circle(int cx, int cy, int radius) {
    if (radius < 0) return;
    int x = radius, y = 0, err = 0;
    while (x >= y) {
        SDL_RenderDrawPoint(sdl, cx + x, cy + y);
        SDL_RenderDrawPoint(sdl, cx + y, cy + x);
        SDL_RenderDrawPoint(sdl, cx - y, cy + x);
        SDL_RenderDrawPoint(sdl, cx - x, cy + y);
        SDL_RenderDrawPoint(sdl, cx - x, cy - y);
        SDL_RenderDrawPoint(sdl, cx - y, cy - x);
        SDL_RenderDrawPoint(sdl, cx + y, cy - x);
        SDL_RenderDrawPoint(sdl, cx + x, cy - y);
        if (err <= 0) { y++; err += 2 * y + 1; }
        if (err >  0) { x--; err -= 2 * x + 1; }
    }
}

// Row half-width of a regular octagon whose four flat edges sit `radius` from the
// centre: full width across the flats, then a 45° cut at each corner. The corner
// cut starts where the flat edge ends, at (sqrt(2)-1)*radius — the value that makes
// all eight sides the same length.
int Renderer::octagon_dx(int dy, int radius) {
    int flat = (int)(radius * 0.41421356f + 0.5f);
    int ady  = (dy < 0) ? -dy : dy;
    if (ady <= flat) return radius;
    if (ady >  radius) return -1;
    return radius - (ady - flat);
}

void Renderer::fill_octagon(int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = octagon_dx(dy, radius);
        SDL_RenderDrawLine(sdl, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

void Renderer::fill_ring(int cx, int cy, int r_outer, int r_inner) {
    for (int dy = -r_outer; dy <= r_outer; dy++) {
        int dx_o = (int)sqrtf(float(r_outer * r_outer - dy * dy));
        int abs_dy = (dy < 0) ? -dy : dy;
        if (abs_dy < r_inner) {
            int dx_i = (int)sqrtf(float(r_inner * r_inner - dy * dy));
            SDL_RenderDrawLine(sdl, cx - dx_o, cy + dy, cx - dx_i, cy + dy);
            SDL_RenderDrawLine(sdl, cx + dx_i, cy + dy, cx + dx_o, cy + dy);
        } else {
            SDL_RenderDrawLine(sdl, cx - dx_o, cy + dy, cx + dx_o, cy + dy);
        }
    }
}

// Scanline fill of a rotated ellipse.
// ra = semi-axis along (ux,uy),  rb = semi-axis along the perpendicular (-uy,ux).
// Iterates over integer steps in the perpendicular direction.
void Renderer::fill_ellipse_rotated(int cx, int cy, float ux, float uy, int ra, int rb) {
    if (ra < 1 || rb < 1) return;
    float px = -uy, py = ux;   // perpendicular unit vector
    for (int t = -rb; t <= rb; t++) {
        float frac = (float)t / rb;
        float span = ra * sqrtf(std::max(0.f, 1.f - frac * frac));
        int   ispan = (int)span;
        float ox = px * t, oy = py * t;
        SDL_RenderDrawLine(sdl,
            cx + (int)(ox - ux * ispan), cy + (int)(oy - uy * ispan),
            cx + (int)(ox + ux * ispan), cy + (int)(oy + uy * ispan));
    }
}

void Renderer::draw_stone_circle(const BoardView& view, int r, int f, int is_black, Uint8 alpha, bool shadow_pass) {
    int cx     = view.offset_x + f * view.square + view.square / 2;
    int cy     = view.offset_y + r * view.square + view.square / 2;
    // Round and square stones sit inside the cell with a couple of pixels of board
    // showing between neighbours. Octagons deliberately fill it: at exactly half a
    // cell, orthogonal neighbours meet flat edge to flat edge, which is the whole
    // point of that mode.
    int radius = view.square / 2 - (octagon_stones_ ? 0 : 2);
    if (radius < 2) radius = 2;
    shade_stone(cx, cy, radius, is_black, alpha, shadow_pass);
}

// ---------------------------------------------------------------------------
// Shared stone colour palette — edit here to reskin both stones and chain links.

Renderer::StoneColors Renderer::stone_colors(int is_black) {
    if (is_black)
        //        base                lit overlay          dark edge            a1      a2      a3
        return { {18, 20, 25, 255}, {72, 66, 58, 255}, {10, 12, 16, 255}, 0.14f, 0.22f, 0.16f };
    else
        return { {210, 214, 220, 255}, {255, 252, 240, 255}, {178, 182, 190, 255}, 0.35f, 0.28f, 0.20f };
}

void Renderer::shade_stone(int cx, int cy, int radius, int is_black, Uint8 alpha, bool shadow_pass) {
    // Octagon stone mode: the square tile's bevel wrapped onto an octagon. Checked
    // before square_stones_ so a settings file with both flags set still renders one
    // definite shape rather than something half-way.
    if (octagon_stones_) {
        if (shadow_pass) {
            if (radius >= 4) {
                int sox = radius / 5 + 1;
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.18f));
                fill_octagon(cx + sox + 1, cy + sox + 1, radius + 2);
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.30f));
                fill_octagon(cx + sox,     cy + sox,     radius + 1);
            }
            return;
        }
        auto c = stone_colors(is_black);
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sdl, c.base.r, c.base.g, c.base.b, alpha);
        fill_octagon(cx, cy, radius);

        // Bevel: the band between the tile and an octagon inset by `bevel`. Rows
        // above the inset tile are all top edge (lit), rows below are all bottom
        // edge (dark), and in between the band splits into a left run and a right
        // run. Same top-left-lit / bottom-right-dark reading as the square tiles,
        // corners included — it's a light direction, not a per-edge normal.
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        int bevel = std::max(1, radius / 4);
        int inner = radius - bevel;
        SDL_Color lit = c.lit, dark = c.dark;
        Uint8 lit_a  = (Uint8)(alpha * c.alpha2);
        Uint8 dark_a = (Uint8)(alpha * 0.8f);
        for (int dy = -radius; dy <= radius; dy++) {
            int dxo = octagon_dx(dy, radius);
            int dxi = octagon_dx(dy, inner);
            if (dxi < 0) {   // past the inset tile entirely — whole row is bevel
                bool top = (dy < 0);
                SDL_SetRenderDrawColor(sdl, top ? lit.r : dark.r, top ? lit.g : dark.g,
                                       top ? lit.b : dark.b, top ? lit_a : dark_a);
                SDL_RenderDrawLine(sdl, cx - dxo, cy + dy, cx + dxo, cy + dy);
                continue;
            }
            SDL_SetRenderDrawColor(sdl, lit.r, lit.g, lit.b, lit_a);
            SDL_RenderDrawLine(sdl, cx - dxo, cy + dy, cx - dxi, cy + dy);
            SDL_SetRenderDrawColor(sdl, dark.r, dark.g, dark.b, dark_a);
            SDL_RenderDrawLine(sdl, cx + dxi, cy + dy, cx + dxo, cy + dy);
        }
        return;
    }

    // Square stone mode: flat tiles with a subtle top-left/bottom-right bevel,
    // same footprint and shadow offsets as the round stones.
    if (square_stones_) {
        SDL_Rect body = {cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1};
        if (shadow_pass) {
            if (radius >= 4) {
                int sox = radius / 5 + 1;
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.18f));
                SDL_Rect s1 = {body.x + sox + 1, body.y + sox + 1, body.w + 2, body.h + 2};
                SDL_RenderFillRect(sdl, &s1);
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.30f));
                SDL_Rect s2 = {body.x + sox, body.y + sox, body.w + 1, body.h + 1};
                SDL_RenderFillRect(sdl, &s2);
            }
            return;
        }
        auto c = stone_colors(is_black);
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sdl, c.base.r, c.base.g, c.base.b, alpha);
        SDL_RenderFillRect(sdl, &body);

        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        int bevel = std::max(1, radius / 4);
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha2));
        SDL_Rect top  = {body.x, body.y, body.w, bevel};
        SDL_Rect left = {body.x, body.y, bevel, body.h};
        SDL_RenderFillRect(sdl, &top);
        SDL_RenderFillRect(sdl, &left);
        SDL_SetRenderDrawColor(sdl, c.dark.r, c.dark.g, c.dark.b, (Uint8)(alpha * 0.8f));
        SDL_Rect bot   = {body.x, body.y + body.h - bevel, body.w, bevel};
        SDL_Rect right = {body.x + body.w - bevel, body.y, bevel, body.h};
        SDL_RenderFillRect(sdl, &bot);
        SDL_RenderFillRect(sdl, &right);
        return;
    }

    if (shadow_pass) {
        // Only the cast shadow — two soft dark circles offset lower-right.
        // Blend mode is set by the caller (render_all_shadows uses max-alpha blend).
        if (radius >= 4) {
            int sox = radius / 5 + 1, soy = sox;
            SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.18f));
            fill_circle(cx + sox + 1, cy + soy + 1, radius + 2);
            SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.30f));
            fill_circle(cx + sox,     cy + soy,     radius + 1);
        }
        return;
    }

    // Stone fill + shading
    auto c = stone_colors(is_black);
    int h1x = cx - radius / 6,  h1y = cy - radius / 6;  // broad
    int h2x = cx - radius / 5,  h2y = cy - radius / 5;  // mid
    int h3x = cx - radius / 4,  h3y = cy - radius / 4;  // tight

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, c.base.r, c.base.g, c.base.b, alpha);
    fill_circle(cx, cy, radius);

    // Highlight layers clipped to the stone boundary — h1's offset+radius exceeds the
    // stone radius by ~7%, causing a lit halo outside the stone without this clipping.
    auto clip_fill = [&](int hx, int hy, int hr) {
        int r2 = radius * radius;
        for (int dy = -hr; dy <= hr; dy++) {
            int py = hy + dy;
            int hdx = (int)sqrtf((float)(hr * hr - dy * dy));
            int xl = hx - hdx, xr = hx + hdx;
            // Intersect with stone circle row
            int sdy = py - cy;
            int sclip2 = r2 - sdy * sdy;
            if (sclip2 < 0) continue;
            int sdx = (int)sqrtf((float)sclip2);
            xl = std::max(xl, cx - sdx);
            xr = std::min(xr, cx + sdx);
            if (xl <= xr) SDL_RenderDrawLine(sdl, xl, py, xr, py);
        }
    };

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    if (c.alpha1 > 0.f) {
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha1));
        clip_fill(h1x, h1y, radius * 5 / 6);
    }
    if (c.alpha2 > 0.f) {
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha2));
        clip_fill(h2x, h2y, radius * 2 / 3);
    }
    if (c.alpha3 > 0.f) {
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha3));
        clip_fill(h3x, h3y, radius / 2);
    }
}

// Draw a chain link as a shaded cylinder.
// shadow_pass=true  → only the soft dark shadow quad (drawn first, over all board lines)
// shadow_pass=false → only the lit cylinder (drawn second, on top of all shadows)
void Renderer::draw_stone_link(int x1, int y1, int x2, int y2, int thickness, int is_black, bool shadow_pass, int stone_radius) {
    if (thickness < 1) thickness = 1;

    float fdx = (float)(x2 - x1), fdy = (float)(y2 - y1);
    float len  = sqrtf(fdx * fdx + fdy * fdy);
    if (len < 1.f) return;

    float nx = fdy / len, ny = -fdx / len;

    if (shadow_pass) {
        // Two soft dark quads offset lower-right — same scale as shade_stone.
        // Blend mode is set by the caller (render_all_shadows uses max-alpha blend).
        if (thickness < 4) return;
        int shx = thickness / 5 + 1, shy = shx;
        for (int p = 0; p < 2; p++) {
            float offx = (float)(p == 0 ? shx + 1 : shx);
            float offy = (float)(p == 0 ? shy + 1 : shy);
            float ht   = (p == 0 ? thickness + 2 : thickness + 1) * 0.5f;
            Uint8 sa   = (p == 0) ? (Uint8)(255 * 0.18f) : (Uint8)(255 * 0.30f);
            SDL_Color sc = {0, 0, 0, sa};
            float pox = nx * ht, poy = ny * ht;
            SDL_Vertex sv[4] = {
                {{(float)x1 - pox + offx, (float)y1 - poy + offy}, sc, {0,0}},
                {{(float)x1 + pox + offx, (float)y1 + poy + offy}, sc, {0,0}},
                {{(float)x2 + pox + offx, (float)y2 + poy + offy}, sc, {0,0}},
                {{(float)x2 - pox + offx, (float)y2 - poy + offy}, sc, {0,0}},
            };
            int si[6] = {0,1,2, 0,2,3};
            SDL_RenderGeometry(sdl, nullptr, sv, 4, si, 6);
        }
        return;
    }

    // Cylinder pass — discrete layered shading matching the stone:
    //   base fill (solid c.base) + lit overlay strips narrower and shifted toward the lit side,
    //   using the same alpha1/2/3 fractions as shade_stone.
    auto c = stone_colors(is_black);

    float ux = fdx / len, uy = fdy / len;  // unit vector A→B

    float overlap = thickness * 0.08f + 1.f;
    float shorten = std::max(0.f, stone_radius - overlap);
    float sx = x1 + ux * shorten, sy = y1 + uy * shorten;
    float ex = x2 - ux * shorten, ey = y2 - uy * shorten;

    float half = thickness * 0.5f;
    float bx = nx * half, by = ny * half;   // full half-width perpendicular vectors

    // Determine which perpendicular side faces the upper-left light
    bool flip = (nx * (-0.707f) + ny * (-0.707f)) < 0.f;
    float lit_nx = flip ? -nx : nx, lit_ny = flip ? -ny : ny;  // lit-side unit vector

    // Layer descriptors.  off_frac is centre offset toward lit side as a fraction of
    // half-thickness; w_frac is half-width as the same fraction.  Strips are painted
    // outermost first (col1) then progressively narrower ones on top, with BLENDMODE_NONE,
    // so they don't interact.
    //
    // The broad first strip (off=0.25, w=0.75) extends 0.5*half into the DARK side — just
    // like the stone's large lit circle which covers ~83% of the sphere and leaves only a
    // thin dark rim.  This means ~75% of the bar has at least col1 brightness, matching the
    // stone's proportions and making all three tones clearly visible at any board size.
    struct Layer { float off_frac; float w_frac; float alpha; };
    const Layer layers[] = {
        { 0.25f, 0.75f, c.alpha1 },   // broad:  ~75% of bar (extends into dark side)
        { 0.55f, 0.45f, c.alpha2 },   // medium: lit side of centre
        { 0.80f, 0.20f, c.alpha3 },   // tight:  near lit edge
    };

    // 1. Base fill — solid c.base rectangle
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    {
        SDL_Color col = { c.base.r, c.base.g, c.base.b, 255 };
        SDL_Vertex v[4] = {
            {{sx - bx, sy - by}, col, {0,0}},
            {{sx + bx, sy + by}, col, {0,0}},
            {{ex + bx, ey + by}, col, {0,0}},
            {{ex - bx, ey - by}, col, {0,0}},
        };
        int idx[6] = {0,1,2, 0,2,3};
        SDL_RenderGeometry(sdl, nullptr, v, 4, idx, 6);
    }

    // Pre-compute compounded opaque colors for each layer — same compound as shade_stone
    // uses with BLENDMODE_BLEND, but expressed as flat opaque values so no triangle-edge
    // double-compositing can create stripe artefacts.
    auto compose = [](SDL_Color base, SDL_Color lit, float alpha) -> SDL_Color {
        return { (Uint8)(base.r + (lit.r - base.r) * alpha),
                 (Uint8)(base.g + (lit.g - base.g) * alpha),
                 (Uint8)(base.b + (lit.b - base.b) * alpha), 255 };
    };
    SDL_Color col1 = compose(c.base, c.lit, c.alpha1);
    SDL_Color col2 = compose(col1,   c.lit, c.alpha2);   // compound: layer2 on top of layer1
    SDL_Color col3 = compose(col2,   c.lit, c.alpha3);   // compound: layer3 on top of both
    const SDL_Color layer_cols[3] = { col1, col2, col3 };

    // 2. Lit overlay strips — narrower, shifted toward lit side, drawn opaque outermost→innermost
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    for (int i = 0; i < 3; i++) {
        const Layer& L = layers[i];
        if (L.alpha <= 0.f) continue;
        float perp_off = half * L.off_frac;
        float lhalf    = thickness * L.w_frac * 0.5f;
        float lbx = nx * lhalf, lby = ny * lhalf;
        float ox2 = lit_nx * perp_off, oy2 = lit_ny * perp_off;
        SDL_Vertex v[4] = {
            {{sx + ox2 - lbx, sy + oy2 - lby}, layer_cols[i], {0,0}},
            {{sx + ox2 + lbx, sy + oy2 + lby}, layer_cols[i], {0,0}},
            {{ex + ox2 + lbx, ey + oy2 + lby}, layer_cols[i], {0,0}},
            {{ex + ox2 - lbx, ey + oy2 - lby}, layer_cols[i], {0,0}},
        };
        int idx[6] = {0,1,2, 0,2,3};
        SDL_RenderGeometry(sdl, nullptr, v, 4, idx, 6);
    }

    // 3. End caps — ellipses at bar endpoints, shaded with the same discrete layers.
    // ra = semi-axis along the bar direction (controls roundness; small = flat disc).
    // rb = half-width of the bar (perpendicular to bar).
    // Using BLENDMODE_NONE + pre-computed flat colors avoids any per-scanline gradient
    // banding (that was the old ribbing cause).
    {
        int cap_rb = (int)half;                            // full bar half-width
        int cap_ra = std::max(1, (int)(half * 0.35f));    // roundness along bar axis

        auto draw_cap = [&](float cx, float cy) {
            // Base disc
            SDL_SetRenderDrawColor(sdl, c.base.r, c.base.g, c.base.b, 255);
            fill_ellipse_rotated((int)cx, (int)cy, ux, uy, cap_ra, cap_rb);
            // Lit overlay ellipses — same perpendicular offsets/widths as bar strips
            for (int i = 0; i < 3; i++) {
                const Layer& L = layers[i];
                if (L.alpha <= 0.f) continue;
                float perp_off = half * L.off_frac;
                int lrb = std::max(1, (int)(half * L.w_frac));
                float ocx = cx + lit_nx * perp_off;
                float ocy = cy + lit_ny * perp_off;
                SDL_SetRenderDrawColor(sdl, layer_cols[i].r, layer_cols[i].g, layer_cols[i].b, 255);
                fill_ellipse_rotated((int)ocx, (int)ocy, ux, uy, cap_ra, lrb);
            }
        };

        draw_cap(sx, sy);
        draw_cap(ex, ey);
    }
}

// ---------------------------------------------------------------------------
// Overlay helpers

void Renderer::render_chain_connections(const BoardView& view, const char board[][MAX_BOARD_SIZE],
                                        bool chain_mode, int stone_filter, bool shadows_only) {
    if (!chain_mode) return;
    int n = view.active_size;
    int drawn[MAX_BOARD_SIZE][MAX_BOARD_SIZE][4] = {};
    for (int r = 0; r < n; r++) {
        for (int f = 0; f < n; f++) {
            if (board[r][f] == 0) continue;
            int color   = (board[r][f] == 1) ? 1 : 0;
            if (stone_filter == 1 && color == 0) continue;
            if (stone_filter == 2 && color == 1) continue;
            int visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
            int gr[MAX_BOARD_SIZE * MAX_BOARD_SIZE], gf[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
            int gc = 0;
            GoRules::get_group(board, r, f, color, visited, &gc, gr, gf, n);
            for (int i = 0; i < gc; i++) {
                int sr = gr[i], sf = gf[i];
                int adj[4][2] = {{sr-1,sf},{sr,sf+1},{sr+1,sf},{sr,sf-1}};
                for (int dir = 0; dir < 4; dir++) {
                    int ar = adj[dir][0], af = adj[dir][1];
                    if (ar < 0 || ar >= n || af < 0 || af >= n) continue;
                    if (board[ar][af] == 0 || board[ar][af] != board[sr][sf]) continue;
                    int rev = (dir + 2) % 4;
                    if (drawn[sr][sf][dir] || drawn[ar][af][rev]) continue;
                    drawn[sr][sf][dir] = drawn[ar][af][rev] = 1;
                    int x1 = view.offset_x + sf * view.square + view.square / 2;
                    int y1 = view.offset_y + sr * view.square + view.square / 2;
                    int x2 = view.offset_x + af * view.square + view.square / 2;
                    int y2 = view.offset_y + ar * view.square + view.square / 2;
                    int thick    = (view.square - 4) / 2;
                    int stone_r  = view.square / 2 - 2;
                    draw_stone_link(x1, y1, x2, y2, thick, color, shadows_only, stone_r);
                }
            }
        }
    }
}

void Renderer::render_liberties(const BoardView& view,
                                const int lib_r[], const int lib_f[], int lib_count) {
    if (lib_count == 0) return;
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < lib_count; i++) {
        int cx = view.offset_x + lib_f[i] * view.square + view.square / 2;
        int cy = view.offset_y + lib_r[i] * view.square + view.square / 2;
        int rad = view.square / 4;
        if (rad < 2) rad = 2;
        SDL_SetRenderDrawColor(sdl, Palette::LIBERTY_DOT.r, Palette::LIBERTY_DOT.g, Palette::LIBERTY_DOT.b, Palette::LIBERTY_DOT.a);
        fill_circle(cx, cy, rad);
    }
}

// ---------------------------------------------------------------------------
// HUD overlays

const char* Renderer::format_result_message(const char* r) {
    if (!r || r[0] == '\0') return "";
    static char buf[64];
    if      (strcmp(r, "B+R") == 0 || strcmp(r, "B+Resign") == 0) return "Black Resigns";
    else if (strcmp(r, "W+R") == 0 || strcmp(r, "W+Resign") == 0) return "White Resigns";
    else if (strcmp(r, "B+T") == 0) return "Black Wins by Time";
    else if (strcmp(r, "W+T") == 0) return "White Wins by Time";
    else if (strcmp(r, "1/2-1/2") == 0 || strcmp(r, "Jigo") == 0) return "Draw";
    else if (strcmp(r, "Void") == 0) return "Game Void";
    else if (strcmp(r, "Unfinished") == 0) return "Unfinished";
    else if (r[0] == 'B' && r[1] == '+') { snprintf(buf, sizeof(buf), "Black Wins by %s", r+2); return buf; }
    else if (r[0] == 'W' && r[1] == '+') { snprintf(buf, sizeof(buf), "White Wins by %s", r+2); return buf; }
    strncpy(buf, r, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
    return buf;
}


void Renderer::render_mode_status(const BoardView& view,
                                  bool analysis_mode, bool game_mode, bool guess_mode,
                                  bool territory_drill, bool /*paused*/) {
    const char* txt = nullptr;
    if      (territory_drill) txt = "TERRITORY DRILL";
    else if (game_mode)       txt = "GAME MODE";
    else if (analysis_mode)   txt = "ANALYSIS MODE";
    else if (guess_mode)      txt = "GUESS MODE";
    if (!txt) return;
    int scale  = (view.square >= 30) ? 3 : 2;
    int pad    = (view.square >= 30) ? 16 : 8;
    int tw = text_width_px(txt, scale);
    // Anchor to background left/top edge (one view.margin outside the grid)
    int x  = view.offset_x - view.margin - pad - tw;
    int y  = view.offset_y - view.margin + pad;
    draw_text(x, y, scale, txt, Palette::ACCENT);
}

void Renderer::render_result_message(const BoardView& view, const DrawState& ds) {
    if (!ds.game.game_finished || ds.result_message.empty()) return;
    const char* txt = format_result_message(ds.result_message.c_str());
    if (!txt || txt[0] == '\0') return;
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int tw     = text_width_px(txt, scale);
    int th     = 7 * scale;
    int bg_h   = view.board_px + 2 * view.margin;
    // Anchor to background right/vertical-centre
    int x      = view.offset_x + view.board_px + view.margin + margin;
    int y      = view.offset_y - view.margin + (bg_h - th) / 2;
    (void)tw;
    draw_text(x, y, scale, txt, Palette::ACCENT);
}

void Renderer::render_speed_label(const BoardView& view, int delay_ms, Uint32 until) {
    if (until == 0) return;
    Uint32 now = SDL_GetTicks();
    if (now >= until) return;
    char buf[32];
    int whole = delay_ms / 1000;
    int rem   = delay_ms % 1000;
    if (rem == 0) {
        const char* unit = (whole == 1) ? "second" : "seconds";
        snprintf(buf, sizeof(buf), "%d %s/move", whole, unit);
    } else {
        snprintf(buf, sizeof(buf), "%d.%d seconds/move", whole, rem / 100);
    }
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int tw     = text_width_px(buf, scale);
    int th     = 7 * scale;
    int bg_left = view.offset_x - view.margin;
    int bg_size = view.board_px + 2 * view.margin;
    int x      = bg_left + (bg_size - tw) / 2;
    if (x < bg_left + margin) x = bg_left + margin;
    int y = view.offset_y - view.margin + margin;
    int pad = (scale >= 3) ? 4 : 3;
    SDL_Rect bg = {x - pad, y - pad, tw + pad * 2, th + pad * 2};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, Palette::OVERLAY_SPEED.r, Palette::OVERLAY_SPEED.g, Palette::OVERLAY_SPEED.b, Palette::OVERLAY_SPEED.a);
    SDL_RenderFillRect(sdl, &bg);
    draw_text(x, y, scale, buf, Palette::TEXT_WHITE);
}

// Colour of each scoring ring, shared by the heatmap cursor and the HUD legend so
// the numbers on screen and the shells under the pointer always agree. A cold-to-hot
// ramp: the bullseye reads as the hot centre, the outer ring as barely-warm.
static const SDL_Color GUESS_RING_COLOR[3] = {
    {255,  70,  40, 255},   // ring 0 — 10 pts, red hot
    {255, 165,  40, 255},   // ring 1 —  5 pts, orange
    { 80, 150, 230, 255},   // ring 2 —  1 pt,  cool blue
};

void Renderer::render_guess_score(const BoardView& view, const DrawState& ds) {
    if (!ds.guess_mode) return;
    // The live client owns this gutter with its status line and would overdraw us;
    // it reports the same scoreboard through that status instead.
    if (ds.live_mode) return;
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int th     = 7 * scale;
    int gap    = scale + 2;
    int line_h = th + gap;
    // Right-aligned into the same left gutter the "GUESS MODE" label occupies,
    // stacked underneath it.
    int right = view.offset_x - view.margin - margin;
    int y     = view.offset_y - view.margin + margin + line_h;
    auto line = [&](const char* txt, SDL_Color col) {
        draw_text(right - text_width_px(txt, scale), y, scale, txt, col);
        y += line_h;
    };

    char buf[48];
    snprintf(buf, sizeof(buf), "SCORE %d", ds.guess_score);
    line(buf, Palette::SCORE_TEXT);

    // Out of what: every guess could have been a bullseye, so the ceiling is
    // 10/guess. Without it a big score says nothing about how well you're doing.
    if (ds.guess_count > 0) {
        int best = ds.guess_count * GUESS_MAX_POINTS;
        snprintf(buf, sizeof(buf), "OF %d  (%d%%)", best,
                 (ds.guess_score * 100 + best / 2) / best);
        line(buf, Palette::TEXT_DIM);
    }

    // Last guess, coloured by the ring it landed in — the reward signal, so it
    // gets the ring's own colour rather than generic text grey.
    if (ds.guess_last_points >= 0) {
        y += gap;
        if (ds.guess_last_points > 0) {
            snprintf(buf, sizeof(buf), "+%d", ds.guess_last_points);
            SDL_Color c = (ds.guess_last_dist >= 0 && ds.guess_last_dist < 3)
                              ? GUESS_RING_COLOR[ds.guess_last_dist] : Palette::TEXT_DIM;
            line(buf, c);
            line(ds.guess_last_dist == 0 ? "BULLSEYE!" : "CLOSE", c);
        } else {
            line("MISS", Palette::TEXT_DIM);
        }
    }
}

void Renderer::render_player_labels(const BoardView& view, const DrawState& ds) {
    if (ds.puzzle_mode) return;   // right gutter belongs to the puzzle text box
    int margin   = (view.square >= 30) ? 16 : 8;
    // Anchor to background right edge (one view.margin outside the grid)
    int right_x0 = view.offset_x + view.board_px + view.margin + margin;
    int right_x1 = view.screen_w - margin;
    if (right_x1 <= right_x0) return;

    // Use board stone radius as a column-width estimate for the scale check
    int board_radius = view.square / 2 - 2;
    int gap          = 12;
    if (right_x1 - right_x0 - board_radius * 2 - gap <= 0) return;

    const char* bn = ds.black_name.empty() ? "Black" : ds.black_name.c_str();
    const char* wn = ds.white_name.empty() ? "White" : ds.white_name.c_str();

    int bp = ds.analysis ? ds.analysis->black_prisoners : ds.game.black_prisoners;
    int wp = ds.analysis ? ds.analysis->white_prisoners : ds.game.white_prisoners;

    char bpstr[32], wpstr[32];
    char b_pris[32], w_pris[32];
    // A filled board carries its own score: every point is a stone of its owner's
    // colour, so counting the stones IS the Chinese count. The clock line is dead
    // weight there (the screensaver has no clocks), so the count takes that slot —
    // biggest text in the panel, readable from across the room.
    bool area_count = (ds.live_area_black >= 0 && ds.live_area_white >= 0);
    if (area_count) {
        snprintf(bpstr,  sizeof(bpstr),  "%d", ds.live_area_black);
        snprintf(wpstr,  sizeof(wpstr),  "%d", ds.live_area_white);
        snprintf(b_pris, sizeof(b_pris), "Prisoners: %d", bp);
        snprintf(w_pris, sizeof(w_pris), "Prisoners: %d", wp);
    } else if (ds.live_mode) {
        // Show clock on line 2; for byo-yomi show period info when main time is gone
        auto fmt_clock = [](int secs, int periods, int period_secs, char* buf, size_t n) {
            if (secs < 0) { snprintf(buf, n, "--:--"); return; }
            if (secs > 0 || periods <= 0) {
                // Main time remaining (or no period info)
                snprintf(buf, n, "%d:%02d", secs / 60, secs % 60);
                if (periods > 0)  // in byo-yomi but period countdown hasn't hit zero
                    snprintf(buf + strlen(buf), n - strlen(buf), " x%d", periods);
            } else {
                // Main time exhausted, in byo-yomi
                if (period_secs > 0)
                    snprintf(buf, n, "%ds x%d", period_secs, periods);
                else
                    snprintf(buf, n, "BYO x%d", periods);
            }
        };
        fmt_clock(ds.live_black_secs, ds.live_black_periods, ds.live_black_period_secs, bpstr, sizeof(bpstr));
        fmt_clock(ds.live_white_secs, ds.live_white_periods, ds.live_white_period_secs, wpstr, sizeof(wpstr));
        // Prisoners on their own line 3
        snprintf(b_pris, sizeof(b_pris), "Prisoners: %d", bp);
        snprintf(w_pris, sizeof(w_pris), "Prisoners: %d", wp);
    } else {
        snprintf(bpstr, sizeof(bpstr), "Prisoners: %d", bp);
        snprintf(wpstr, sizeof(wpstr), "Prisoners: %d", wp);
        b_pris[0] = '\0';
        w_pris[0] = '\0';
    }

    // Clocks render two steps larger than the name/prisoner lines in live mode —
    // the countdown is what actually gets read at a glance mid-game.
    int scale = 3;
    int avail = right_x1 - right_x0 - board_radius * 2 - gap;
    auto measure = [&](int sc, int clock_sc) {
        int w = text_width_px(bn, sc);
        { int ww = text_width_px(wn, sc); if (ww > w) w = ww; }
        // In live mode bpstr/wpstr are the clock lines (bigger scale);
        // otherwise they're the prisoner lines (normal scale).
        int line2_sc = ds.live_mode ? clock_sc : sc;
        { int pw = text_width_px(bpstr, line2_sc); if (pw > w) w = pw; }
        { int pw = text_width_px(wpstr, line2_sc); if (pw > w) w = pw; }
        if (b_pris[0]) { int pw = text_width_px(b_pris, sc); if (pw > w) w = pw; }
        if (w_pris[0]) { int pw = text_width_px(w_pris, sc); if (pw > w) w = pw; }
        return w;
    };
    int clock_scale = scale + 2;
    if (measure(scale, clock_scale) > avail) {
        scale       = 2;
        clock_scale = 4;
        if (measure(scale, clock_scale) > avail) {
            clock_scale = 3;  // last resort: shrink the clock before giving up
            if (measure(scale, clock_scale) > avail) return;
        }
    }

    int th       = 7 * scale;
    int clock_th = ds.live_mode ? 7 * clock_scale : th;
    int line_gap = scale + 2;
    int block_h  = ds.live_mode ? (th + line_gap + clock_th + line_gap + th)
                                : (2 * th + line_gap);

    // Cap radius to fit within the text block height; matches board size on smaller screens
    int radius    = std::min(board_radius, block_h / 2);
    int stone_dim = radius * 2;
    int tx        = right_x0 + stone_dim + gap;
    int scx       = right_x0 + radius;

    // In live mode, highlight whichever player's turn it is.
    SDL_Color black_name_color = (ds.live_mode && ds.game.turn_is_black == 1)
                                 ? Palette::ACCENT : Palette::TEXT_PRIMARY;
    SDL_Color white_name_color = (ds.live_mode && ds.game.turn_is_black == 0)
                                 ? Palette::ACCENT : Palette::TEXT_PRIMARY;
    SDL_Color prisoner_color = Palette::TEXT_DIM;
    // Red whenever the player is actually in byo-yomi — both while a period is
    // counting down (live_*_in_byo, computed by the clock tick, since the displayed
    // numbers alone can't distinguish a period countdown from low main time) and
    // while parked between moves with main time exhausted (secs<=0, periods>0).
    auto byo_color = [&](bool in_byo, int secs, int periods) -> SDL_Color {
        return (ds.live_mode && (in_byo || (secs <= 0 && periods > 0)))
               ? SDL_Color{220, 60, 60, 255}
               : Palette::TEXT_DIM;
    };
    SDL_Color b_clock_color = byo_color(ds.live_black_in_byo, ds.live_black_secs, ds.live_black_periods);
    SDL_Color w_clock_color = byo_color(ds.live_white_in_byo, ds.live_white_secs, ds.live_white_periods);
    // The area count is a result, not a clock, so it gets the accent colour. Both
    // sides equally: these are raw stone counts with no komi in them, so the bigger
    // number is not necessarily the winner and shouldn't be dressed up as one.
    if (area_count) {
        b_clock_color = Palette::ACCENT;
        w_clock_color = Palette::ACCENT;
    }

    // Helper: draw a filled circle for the player stone
    auto draw_stone = [&](int cx, int cy, bool is_black) {
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sdl,
            is_black ?  30 : 240,
            is_black ?  30 : 240,
            is_black ?  30 : 240,
            255);
        for (int dy = -radius; dy <= radius; dy++)
            for (int dx = -radius; dx <= radius; dx++)
                if (dx*dx + dy*dy <= radius*radius)
                    SDL_RenderDrawPoint(sdl, cx + dx, cy + dy);
    };

    // Line 2 (the clock in live mode) draws at clock_scale; line 3 sits below it.
    int line2_sc = ds.live_mode ? clock_scale : scale;
    int line3_y  = th + line_gap + clock_th + line_gap;

    // Black at the top — stone centered over the text block
    // Anchor to background top edge (one view.margin above the grid)
    int top_y = view.offset_y - view.margin + margin;
    draw_stone(scx, top_y + block_h / 2, true);
    draw_text(tx, top_y,                  scale,    bn,    black_name_color);
    draw_text(tx, top_y + th + line_gap,  line2_sc, bpstr, b_clock_color);
    if (b_pris[0])
        draw_text(tx, top_y + line3_y,    scale,    b_pris, prisoner_color);

    // White at the bottom — anchor to background bottom edge
    int bot_y = view.offset_y + view.board_px + view.margin - margin - block_h;
    if (bot_y < top_y) bot_y = top_y;
    draw_stone(scx, bot_y + block_h / 2, false);
    draw_text(tx, bot_y,                  scale,    wn,    white_name_color);
    draw_text(tx, bot_y + th + line_gap,  line2_sc, wpstr, w_clock_color);
    if (w_pris[0])
        draw_text(tx, bot_y + line3_y,    scale,    w_pris, prisoner_color);
}

void Renderer::render_game_date(const BoardView& view, const std::string& date) {
    if (date.size() < 4) return;
    char year[5];
    memcpy(year, date.c_str(), 4);
    year[4] = '\0';
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int th     = 7 * scale;
    int tw     = text_width_px(year, scale);
    // Primary: left of background, sitting comfortably above the bottom edge
    int x = view.offset_x - view.margin - margin - tw;
    int y = view.offset_y + view.board_px + view.margin - margin - th;
    // Fallback: below the board left-aligned (only on non-widescreen layouts)
    if (x < 0) {
        x = view.offset_x - view.margin;
        y = view.offset_y + view.board_px + view.margin + margin / 2;
    }
    draw_text(x, y, scale, year, Palette::TEXT_DIM);
}

void Renderer::render_help_overlay(const BoardView& view, bool show_help, bool live_mode) {
    if (!show_help) return;

    // key=nullptr  → section header (desc is label text)
    // key=""       → blank spacer
    // otherwise    → key in yellow, desc in white
    struct Row { const char* key; const char* desc; };
    static const Row rows_go[] = {
        {nullptr,      "GO VIEWER HELP"},
        {"",           ""},
        {nullptr,      "NAVIGATION"},
        {"Q",          "QUIT"},
        {"N",          "NEXT GAME"},
        {"R",          "RESTART"},
        {"C",          "CATALOG"},
        {"S",          "SAVE POSITION"},
        {"ESC",        "TOGGLE HELP"},
        {"",           ""},
        {nullptr,      "PLAYBACK"},
        {"UP/DOWN",    "ADJUST SPEED"},
        {"LEFT/RIGHT", "STEP MOVES"},
        {"CLICK STONE","SHOW LIBERTIES"},
        {"",           ""},
        {nullptr,      "MODES"},
        {"SPACE/A",    "ANALYSIS MODE"},
        {"G",          "GUESS MODE"},
        {"P",          "PLAY (2 PLAYERS)"},
        {"T",          "TERRITORY DRILL"},
        {"U",          "CHAIN MODE"},
        {"F",          "FREE MODE"},
        {"",           ""},
        {nullptr,      "ANALYSIS MODE"},
        {"CLICK",      "PLACE STONE"},
        {"HOLD B/W",   "FORCE COLOR"},
        {"R.CLICK",    "REMOVE STONE"},
        {"X",          "CLEAR BOARD"},
        {"",           ""},
        {nullptr,      "GUESS MODE (G)"},
        {"CLICK",      "GUESS THE NEXT MOVE"},
        {"BULLSEYE",   "10 PTS - EXACT POINT"},
        {"RING 1",     "5 PTS - ONE POINT AWAY"},
        {"RING 2",     "1 PT  - TWO POINTS AWAY"},
        {"",           ""},
        {nullptr,      "CATALOG"},
        {"UP/DOWN",    "NAVIGATE"},
        {"ENTER",      "OPEN"},
        {"TYPE",       "SEARCH"},
        {"ESC",        "CLOSE / CLEAR SEARCH"},
    };
    // Live client: three columns — controller binding, keyboard/mouse binding,
    // action — so both input schemes read side by side in the same context.
    if (live_mode) {
        struct Row3 { const char* pad; const char* kb; const char* desc; };
        static const Row3 rows[] = {
            {nullptr,       nullptr,        "OGS CLIENT HELP"},
            {"",            "",             ""},
            {"PAD",         "KEYBOARD",     ""},   // column captions (desc empty → gray)
            {"",            "",             ""},
            {nullptr,       nullptr,        "LOBBY"},
            {GLYPH_PS_CROSS " x2","ENTER x2","FIND MATCH"},
            {GLYPH_PS_SQUARE,"C",           "OPEN CATALOG"},
            {GLYPH_PS_TRIANGLE,"M",         "FREE ANALYSIS"},
            {GLYPH_PS_CIRCLE,"B",           "PUZZLES"},
            {"OPT",         "R / F",        "MENU (MATCH SETTINGS, PRO GAMES, JOSEKI...)"},
            {"SHARE",       "S",            "SETTINGS MENU"},
            {"",            "",             ""},
            {nullptr,       nullptr,        "GAME"},
            {"D-PAD/STICK", "ARROWS/MOUSE", "MOVE CURSOR"},
            {GLYPH_PS_CROSS,"ENTER/CLICK",  "PLACE STONE"},
            {GLYPH_PS_CIRCLE " x2","B x2",  "PASS"},
            {GLYPH_PS_TRIANGLE " x2","M x2","MARK MOVE FOR REVIEW"},
            {GLYPH_PS_SQUARE,"C",           "UNDO (VS KATAGO)"},
            {"L2/R2",       ", . WHEEL",    "STEP MOVE HISTORY"},
            {"R2 HOLD",     "",             "SHOW LAST MOVE"},
            {"OPT",         "R / F",        "GAME MENU (RESIGN...)"},
            {"",            "",             ""},
            {nullptr,       nullptr,        "GAME OVER / REVIEW"},
            {"D-PAD/STICK", "ARROWS/MOUSE", "MOVE CURSOR"},
            {"L2/R2",       ", . WHEEL",    "STEP MAIN LINE"},
            {GLYPH_PS_CIRCLE,"B / R.CLICK", "LABEL POINT (A-Z)"},
            {"L1/R1",       "[ ]",          "SWITCH BRANCH"},
            {GLYPH_PS_CROSS,"ENTER/CLICK",  "PLAY BRANCH STONE"},
            {GLYPH_PS_TRIANGLE " x2","M x2","MARK/UNMARK MOVE"},
            {GLYPH_PS_SQUARE,"C",           "OPEN CATALOG"},
            {"L3/R3",       "PGUP/PGDN",    "PREV/NEXT FILE"},
            {"",            "G",            "GUESS THE MOVE (10/5/1 BY DISTANCE)"},
            {"OPT",         "R / F",        "MENU (LOBBY, DRILL TOOLS...)"},
            {"SHARE",       "S",            "SETTINGS MENU"},
            {"",            "",             ""},
            {nullptr,       nullptr,        "PUZZLES"},
            {"D-PAD",       "ARROWS/MOUSE", "MOVE CURSOR / NAVIGATE LIST"},
            {GLYPH_PS_CROSS,"ENTER/CLICK",  "PLAY MOVE / OPEN"},
            {GLYPH_PS_TRIANGLE,"M",         "FREE PLAY, BOTH SIDES"},
            {"L2/R2",       ", . WHEEL",    "REVIEW (WHILE EXPLORING)"},
            {GLYPH_PS_CIRCLE,"B",           "BACK TO SOLVING / RETRY"},
            {GLYPH_PS_SQUARE,"C",           "BACK TO LIST"},
            {"L3/R3",       "PGUP/PGDN",    "PREV/NEXT PUZZLE"},
            {"OPT",         "R / F",        "MENU (RETRY, EDIT/RENAME/DELETE DRILL...)"},
            {"",            "",             ""},
            {nullptr,       nullptr,        "CATALOG"},
            {"UP/DOWN",     "ARROWS/WHEEL", "NAVIGATE"},
            {"L1/R1",       "[ ]",          "JUMP 10 ROWS"},
            {"L2/R2",       ", .",          "JUMP TO NEXT LETTER"},
            {GLYPH_PS_CROSS,"ENTER/CLICK",  "OPEN"},
            {GLYPH_PS_SQUARE,"C",           "AUTOPLAY FROM HERE"},
            {GLYPH_PS_TRIANGLE " x2","M x2","DELETE GAME"},
            {GLYPH_PS_CIRCLE,"B",           "CLOSE"},
            {"",            "",             ""},
            {"",            "ESC",          "TOGGLE HELP"},
            {"",            "Q x2",         "QUIT"},
        };
        int n = (int)(sizeof(rows) / sizeof(rows[0]));

        int scale    = (view.square >= 30) ? 3 : 2;
        int line_gap = (scale >= 3) ? 4 : 3;
        int th       = 7 * scale;
        int col_gap  = 6 * scale;

        // Pad column labels (L1, OPT...) theme to the connected controller.
        int pad_w = 0, kb_w = 0;
        for (int i = 0; i < n; i++) {
            if (!rows[i].pad) continue;
            if (rows[i].pad[0]) pad_w = std::max(pad_w, text_width_px(themed_labels(rows[i].pad).c_str(), scale));
            if (rows[i].kb[0])  kb_w  = std::max(kb_w,  text_width_px(rows[i].kb,  scale));
        }
        int desc_x = pad_w + col_gap + kb_w + col_gap;
        int max_w  = 0;
        for (int i = 0; i < n; i++) {
            int w = !rows[i].pad ? text_width_px(rows[i].desc, scale)
                                 : desc_x + text_width_px(rows[i].desc, scale);
            max_w = std::max(max_w, w);
        }

        int total_h = n * th + (n - 1) * line_gap;
        int pad     = (scale >= 3) ? 12 : 8;
        int bw = max_w + pad * 2;
        int bh = total_h + pad * 2;
        int x  = (view.screen_w - bw) / 2;
        int y  = (view.screen_h - bh) / 2;

        SDL_Rect bg = {x, y, bw, bh};
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl, Palette::OVERLAY_DARK.r, Palette::OVERLAY_DARK.g,
                               Palette::OVERLAY_DARK.b, Palette::OVERLAY_DARK.a);
        SDL_RenderFillRect(sdl, &bg);

        SDL_Color col_title  = Palette::TEXT_WHITE;
        SDL_Color col_header = {160, 160, 160, 255};
        SDL_Color col_key    = Palette::ACCENT;
        SDL_Color col_desc   = Palette::TEXT_SECONDARY;

        int ty = y + pad;
        for (int i = 0; i < n; i++) {
            if (!rows[i].pad) {
                SDL_Color hc = (i == 0) ? col_title : col_header;
                draw_text(x + pad, ty, scale, rows[i].desc, hc);
            } else {
                // Caption rows (empty desc) draw their key columns in gray
                SDL_Color kc = rows[i].desc[0] ? col_key : col_header;
                if (rows[i].pad[0])
                    draw_text(x + pad, ty, scale, themed_labels(rows[i].pad).c_str(), kc);
                if (rows[i].kb[0])
                    draw_text(x + pad + pad_w + col_gap, ty, scale, rows[i].kb, kc);
                if (rows[i].desc[0])
                    draw_text(x + pad + desc_x, ty, scale, rows[i].desc, col_desc);
            }
            ty += th + line_gap;
        }
        return;
    }

    // go_viewer keeps the original two-column table
    const Row* rows = rows_go;
    int n = (int)(sizeof(rows_go) / sizeof(rows_go[0]));

    int scale    = (view.square >= 30) ? 3 : 2;
    int line_gap = (scale >= 3) ? 4 : 3;
    int th       = 7 * scale;
    int col_gap  = 6 * scale;

    int key_col_w = 0;
    for (int i = 0; i < n; i++)
        if (rows[i].key && rows[i].key[0])
            key_col_w = std::max(key_col_w, text_width_px(rows[i].key, scale));

    int max_w = 0;
    for (int i = 0; i < n; i++) {
        int w = 0;
        if (!rows[i].key)
            w = text_width_px(rows[i].desc, scale);
        else if (rows[i].key[0])
            w = key_col_w + col_gap + text_width_px(rows[i].desc, scale);
        max_w = std::max(max_w, w);
    }

    int total_h = n * th + (n - 1) * line_gap;
    int pad     = (scale >= 3) ? 12 : 8;
    int bw = max_w + pad * 2;
    int bh = total_h + pad * 2;
    int x  = (view.screen_w - bw) / 2;
    int y  = (view.screen_h - bh) / 2;

    SDL_Rect bg = {x, y, bw, bh};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, Palette::OVERLAY_DARK.r, Palette::OVERLAY_DARK.g, Palette::OVERLAY_DARK.b, Palette::OVERLAY_DARK.a);
    SDL_RenderFillRect(sdl, &bg);

    SDL_Color col_title  = Palette::TEXT_WHITE;
    SDL_Color col_header = {160, 160, 160, 255};
    SDL_Color col_key    = Palette::ACCENT;
    SDL_Color col_desc   = Palette::TEXT_SECONDARY;

    int ty = y + pad;
    for (int i = 0; i < n; i++) {
        if (!rows[i].key) {
            SDL_Color hc = (i == 0) ? col_title : col_header;
            draw_text(x + pad, ty, scale, rows[i].desc, hc);
        } else if (rows[i].key[0]) {
            draw_text(x + pad,                        ty, scale, rows[i].key,  col_key);
            draw_text(x + pad + key_col_w + col_gap,  ty, scale, rows[i].desc, col_desc);
        }
        ty += th + line_gap;
    }
}

void Renderer::render_mini_board(int x, int y, int size,
                                  const char board[][BOARD_SIZE], int board_size) {
    // Background
    SDL_Rect bg = {x, y, size, size};
    SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
    SDL_RenderFillRect(sdl, &bg);

    int sq  = size / board_size;                    // pixels per intersection
    if (sq < 1) sq = 1;
    int margin = (size - board_size * sq) / 2;      // center the grid in the box
    int px0 = x + margin + sq / 2;                 // top-left intersection (x)
    int py0 = y + margin + sq / 2;                 // top-left intersection (y)
    int span = (board_size - 1) * sq;

    // Grid lines
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    for (int i = 0; i < board_size; i++) {
        SDL_RenderDrawLine(sdl, px0 + i * sq, py0, px0 + i * sq, py0 + span);
        SDL_RenderDrawLine(sdl, px0, py0 + i * sq, px0 + span, py0 + i * sq);
    }

    // Border
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 180);
    SDL_RenderDrawRect(sdl, &bg);

    // Stones
    int r_stone = sq / 2;
    if (r_stone < 1) r_stone = 1;
    for (int row = 0; row < board_size; row++) {
        for (int col = 0; col < board_size; col++) {
            int cell = board[row][col];
            if (cell == 0) continue;
            int cx = px0 + col * sq;
            int cy = py0 + row * sq;
            bool is_black = (cell == 1);
            SDL_Color c = is_black ? Palette::STONE_BLACK : Palette::STONE_WHITE;
            SDL_SetRenderDrawColor(sdl, c.r, c.g, c.b, 255);
            fill_circle(cx, cy, r_stone);
        }
    }
}

Renderer::MatchMenuLayout Renderer::match_menu_layout(const MatchMenu& menu) const {
    MatchMenuLayout L;
    int sw, sh;
    SDL_GetRendererOutputSize(sdl, &sw, &sh);

    L.scale       = (sw >= 900) ? 3 : 2;
    L.th          = 7 * L.scale;
    L.line_gap    = (L.scale >= 3) ? 8 : 5;
    L.line_h      = L.th + L.line_gap;
    L.hpad        = 80;
    L.col_w       = (sw - L.hpad * 2) / 3;
    // The KataGo screen has no DISPLAY column — those toggles live in MATCH
    // SETTINGS and the in-game SETTINGS, and duplicating them across screens
    // would give the same switch two homes.
    L.display_col = menu.ingame ? 0 : (menu.katago_mode ? -1 : 2);

    // Same sequence draw_match_menu renders: title, hint line.
    int ty = L.hpad / 2;
    ty += L.th + L.line_gap * 3;                        // title
    ty += L.th + L.line_gap * 4;                        // hint line
    L.cols_top = ty;

    L.col_ty[0] = L.col_ty[1] = L.col_ty[2] = ty;
    if (!menu.ingame) {
        L.col_ty[0] += L.line_h + L.line_gap;           // BOARD SIZE header
        L.col_ty[1] += L.line_h + L.line_gap;           // TIME CONTROL / STRENGTH header
    }
    if (L.display_col >= 0)
        L.col_ty[L.display_col] += L.line_h + L.line_gap;   // DISPLAY header
    return L;
}

bool Renderer::match_menu_cell_at(const MatchMenu& menu, int mx, int my,
                                  int& col, int& row) const {
    MatchMenuLayout L = match_menu_layout(menu);
    if (mx < L.hpad || L.col_w <= 0) return false;
    int c = (mx - L.hpad) / L.col_w;
    if (c < 0 || c > 2) return false;
    // Full-stride rows, centred on the text, so there are no dead gaps to click into.
    int rel = my - (L.col_ty[c] - L.line_gap / 2);
    if (rel < 0) return false;
    int r = rel / L.line_h;
    if (r < 0) return false;
    col = c;
    row = r;
    return true;
}

int Renderer::match_menu_stepper_zone(const MatchMenu& menu, int row, int mx, int my) const {
    MatchMenuLayout L = match_menu_layout(menu);
    if (L.display_col < 0) return -1;
    int scale = L.scale;
    int x  = L.hpad + L.display_col * L.col_w;
    int y  = L.col_ty[L.display_col] + row * L.line_h;
    int cx = x + (scale >= 3 ? 14 : 10);
    int lx = cx + text_width_px("[999]", scale) + scale * 3;
    int bx = lx + text_width_px("BOARD HUE", scale) + scale * 6;
    int bw = text_width_px("[-]", scale);
    int px = bx + bw + scale * 3;
    int pw = text_width_px("[+]", scale);

    // Full row height, not just the glyph height — same "no dead gaps" rule
    // match_menu_cell_at uses.
    int top = y - L.line_gap / 2;
    if (my < top || my >= top + L.line_h) return -1;
    if (mx >= bx && mx < bx + bw) return 0;   // [-]
    if (mx >= px && mx < px + pw) return 1;   // [+]
    return -1;
}

void Renderer::draw_match_menu(const MatchMenu& menu) {
    int sw, sh;
    SDL_GetRendererOutputSize(sdl, &sw, &sh);

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    SDL_Rect bg = {0, 0, sw, sh};
    SDL_RenderFillRect(sdl, &bg);

    MatchMenuLayout L = match_menu_layout(menu);
    int scale    = L.scale;
    int th       = L.th;
    int line_gap = L.line_gap;
    int line_h   = L.line_h;
    int hpad     = L.hpad;
    int col_w    = L.col_w;
    int display_col = L.display_col;

    int ty = hpad / 2;
    draw_text(hpad, ty, scale,
              menu.ingame      ? "SETTINGS"
              : menu.katago_mode ? "KATAGO SETTINGS"
                                 : "MATCH SETTINGS", Palette::ACCENT);
    ty += th + line_gap * 3;

    // Hint line (PlayStation button glyphs — Boris plays on a DualShock). The old
    // L1 mode toggle is gone: OGS and KataGo settings are separate screens off the
    // popup menu now, so there is no hidden hotkey to advertise.
    if (menu.ingame)
        draw_text(hpad, ty, scale, GLYPH_PS_CROSS "=TOGGLE   L1/R1=ADJUST   DPAD=NAVIGATE   " GLYPH_PS_CIRCLE "=CLOSE", Palette::TEXT_DIM);
    else if (menu.katago_mode)
        draw_text(hpad, ty, scale, GLYPH_PS_CROSS "=SELECT   DPAD=NAVIGATE   OPT=CLOSE", Palette::TEXT_DIM);
    else
        draw_text(hpad, ty, scale, GLYPH_PS_CROSS "=TOGGLE   L1/R1=ADJUST   DPAD=NAVIGATE   OPT=CLOSE", Palette::TEXT_DIM);
    ty += th + line_gap * 4;

    int col_ty[3] = {L.col_ty[0], L.col_ty[1], L.col_ty[2]};

    static const char* size_labels[3]  = {"9x9", "13x13", "19x19"};
    static const char* speed_labels[3] = {"FAST   (30s + 5x10)", "MEDIUM (5m + 5x30)", "SLOW   (20m + 5x30)"};
    static const char* str_labels[8]   = {
        "20 KYU", "15 KYU", "10 KYU", "5 KYU", "1 KYU", "1 DAN", "5 DAN", "9 DAN"
    };
    static const char* display_labels[9] = {"SHOW COORDINATES", "ENGINE ANALYSIS", "CHAIN LINKS",
                                            "SQUARE STONES", "OCTAGON STONES", "SQUARE GRID",
                                            "SHOW TERRITORY", "SHOW NEXT MOVE", "TETRIS CAPTURES"};

    // Column headers all sit on the same line, one row above their column's first
    // control — col_ty already points past them (see match_menu_layout).
    if (!menu.ingame) {
        draw_text(hpad,         L.cols_top, scale, "BOARD SIZE", Palette::ACCENT);
        draw_text(hpad + col_w, L.cols_top, scale,
                  menu.katago_mode ? "STRENGTH" : "TIME CONTROL", Palette::ACCENT);
    }
    // DISPLAY column: col 2 normally, col 0 in-game, absent on the KataGo screen
    if (display_col >= 0)
        draw_text(hpad + display_col * col_w, L.cols_top, scale, "DISPLAY", Palette::ACCENT);

    // checkbox item (multi-select). disabled: always dim regardless of focus/
    // selection — used for rows that are present but currently inert (e.g.
    // ENGINE ANALYSIS with no KataGo process running).
    auto draw_check = [&](int col, int row, const char* label, bool selected, bool disabled = false) {
        int x = hpad + col * col_w;
        int y = col_ty[col] + row * line_h;
        bool focused = (menu.focus_col == col && menu.focus_row == row);
        draw_text(x, y, scale, focused ? "*" : " ", (focused && !disabled) ? Palette::ACCENT : Palette::TEXT_DIM);
        int cx = x + (scale >= 3 ? 14 : 10);
        draw_text(cx, y, scale, selected ? "[X]" : "[ ]",
                  (selected && !disabled) ? Palette::ACCENT : Palette::TEXT_WHITE);
        int lx = cx + text_width_px("[X]", scale) + scale * 3;
        draw_text(lx, y, scale, label,
                  disabled ? Palette::TEXT_DIM
                           : focused ? Palette::ACCENT : selected ? Palette::TEXT_WHITE : Palette::TEXT_DIM);
    };

    // radio item (single-select)
    auto draw_radio = [&](int col, int row, const char* label, bool selected) {
        int x = hpad + col * col_w;
        int y = col_ty[col] + row * line_h;
        bool focused = (menu.focus_col == col && menu.focus_row == row);
        draw_text(x, y, scale, focused ? "*" : " ", focused ? Palette::ACCENT : Palette::TEXT_DIM);
        int cx = x + (scale >= 3 ? 14 : 10);
        draw_text(cx, y, scale, selected ? "[X]" : "[ ]",
                  selected ? Palette::ACCENT : Palette::TEXT_WHITE);
        int lx = cx + text_width_px("[X]", scale) + scale * 3;
        draw_text(lx, y, scale, label,
                  focused ? Palette::ACCENT : selected ? Palette::TEXT_WHITE : Palette::TEXT_DIM);
    };

    if (!menu.ingame) {
        // Board size is single-select vs KataGo (one local game needs exactly one
        // size) but multi-select for OGS search (multiple acceptable sizes widens
        // the automatch pool) — same distinction as STRENGTH vs TIME CONTROL below.
        if (menu.katago_mode) {
            // Local play uses its own single size, not the OGS multi-select
            for (int i = 0; i < 3; i++)
                draw_radio(0, i, size_labels[i], menu.katago_size == i);
        } else {
            for (int i = 0; i < 3; i++)
                draw_check(0, i, size_labels[i], menu.size_sel[i]);
        }

        if (menu.katago_mode) {
            for (int i = 0; i < 8; i++)
                draw_radio(1, i, str_labels[i], menu.katago_str == i);
            // Row 8: adaptive strength — label carries the current resolved rank
            draw_radio(1, 8,
                       menu.adaptive_label.empty() ? "ADAPTIVE" : menu.adaptive_label.c_str(),
                       menu.katago_str == 8);
        } else {
            for (int i = 0; i < 3; i++)
                draw_check(1, i, speed_labels[i], menu.speed_sel[i]);
        }
    }

    if (display_col >= 0) {
        draw_check(display_col, 0, display_labels[0], menu.show_coords_sel);
        draw_check(display_col, 1, display_labels[1], menu.analysis_sel, !menu.analysis_available);
        draw_check(display_col, 2, display_labels[2], menu.chain_sel);
        // SQUARE and OCTAGON are one shape choice spelled as two checkboxes — the
        // App toggles clear the other one, so at most one is ever ticked.
        draw_check(display_col, 3, display_labels[3], menu.square_sel);
        draw_check(display_col, 4, display_labels[4], menu.octagon_sel);
        draw_check(display_col, 5, display_labels[5], menu.square_grid_sel);
        draw_check(display_col, 6, display_labels[6], menu.territory_sel, !menu.territory_available);
        draw_check(display_col, 7, display_labels[7], menu.next_move_sel);
        draw_check(display_col, 8, display_labels[8], menu.tetris_sel);

        // Board colour: HUE/SAT/VAL as a bracketed number in the same slot draw_check
        // uses for [X]/[ ], followed by dedicated [-]/[+] buttons — a mouse-clickable
        // pair, since L1/R1 (and their keyboard mirrors) aren't discoverable from the
        // screen alone and right-click is already "close menu" on this screen. One
        // shared swatch to the right shows the colour those three values actually
        // produce — a swatch per row was redundant since all three control the same
        // single colour.
        int stepper_bx = 0;   // x of [-] on the last row drawn; same for all three
        auto draw_stepper = [&](int row, const char* label, int value) {
            int x = hpad + display_col * col_w;
            int y = col_ty[display_col] + row * line_h;
            bool focused = (menu.focus_col == display_col && menu.focus_row == row);
            draw_text(x, y, scale, focused ? "*" : " ", focused ? Palette::ACCENT : Palette::TEXT_DIM);
            int cx = x + (scale >= 3 ? 14 : 10);
            char buf[8];
            snprintf(buf, sizeof(buf), "[%d]", value);
            draw_text(cx, y, scale, buf, focused ? Palette::ACCENT : Palette::TEXT_WHITE);
            int lx = cx + text_width_px("[999]", scale) + scale * 3;
            draw_text(lx, y, scale, label, focused ? Palette::ACCENT : Palette::TEXT_DIM);
            int bx = lx + text_width_px("BOARD HUE", scale) + scale * 6;
            draw_text(bx, y, scale, "[-]", focused ? Palette::ACCENT : Palette::TEXT_WHITE);
            int px = bx + text_width_px("[-]", scale) + scale * 3;
            draw_text(px, y, scale, "[+]", focused ? Palette::ACCENT : Palette::TEXT_WHITE);
            stepper_bx = bx;
        };
        draw_stepper(9,  "BOARD HUE", Palette::BOARD_HUE);
        draw_stepper(10, "BOARD SAT", Palette::BOARD_SAT);
        draw_stepper(11, "BOARD VAL", Palette::BOARD_VAL);

        int stepper_px = stepper_bx + text_width_px("[-]", scale) + scale * 3;   // [+] start, same formula as draw_stepper
        int swatch_x    = stepper_px + text_width_px("[+]", scale) + scale * 6;
        int swatch_top  = col_ty[display_col] + 9 * line_h;
        int swatch_size = 2 * line_h + th;   // spans row 9's top to row 11's text bottom
        draw_color_swatch(swatch_x, swatch_top, swatch_size, Palette::BOARD, Palette::TEXT_DIM);
    }

    // ── Controls reference — fills the otherwise-unused bottom of the menu ──────
    // Three columns mirroring the ESC help overlay, in PlayStation button glyphs,
    // so the bindings can be checked from the controller without a keyboard.
    {
        struct CRow { const char* key; const char* desc; };
        static const CRow game_rows[] = {
            {GLYPH_PS_CROSS,        "PLACE STONE"},
            {GLYPH_PS_CIRCLE " x2", "PASS"},
            {"OPTIONS",             "GAME MENU"},
            {GLYPH_PS_TRIANGLE " x2","MARK MOVE"},
            {GLYPH_PS_SQUARE,       "UNDO (VS KATAGO)"},
            {"L2/R2",               "STEP HISTORY"},
            {"R2 HOLD",             "SHOW LAST MOVE"},
            {"SHARE",               "THIS MENU"},
        };
        static const CRow review_rows[] = {
            {"L2/R2",               "STEP MAIN LINE"},
            {GLYPH_PS_CIRCLE,       "LABEL POINT (A-Z)"},
            {"L1/R1",               "SWITCH BRANCH"},
            {GLYPH_PS_CROSS,        "PLAY BRANCH STONE"},
            {GLYPH_PS_TRIANGLE " x2","MARK/UNMARK MOVE"},
            {GLYPH_PS_SQUARE,       "OPEN CATALOG"},
            {"L3/R3",               "PREV/NEXT FILE"},
            {"G",                   "GUESS THE MOVE"},
            {"OPTIONS",             "GAME MENU"},
        };
        static const CRow catalog_rows[] = {
            {"UP/DOWN",             "NAVIGATE"},
            {GLYPH_PS_CROSS,        "OPEN GAME"},
            {GLYPH_PS_SQUARE,       "AUTOPLAY FROM HERE"},
            {GLYPH_PS_TRIANGLE " x2","DELETE GAME"},
            {GLYPH_PS_CIRCLE,       "CLOSE"},
        };
        struct CCol { const char* header; const CRow* rows; int n; };
        const CCol cols[3] = {
            {"GAME",     game_rows,    (int)(sizeof(game_rows)    / sizeof(game_rows[0]))},
            {"REVIEW",   review_rows,  (int)(sizeof(review_rows)  / sizeof(review_rows[0]))},
            {"CATALOG",  catalog_rows, (int)(sizeof(catalog_rows) / sizeof(catalog_rows[0]))},
        };

        int max_rows = 0;
        for (const auto& c : cols) max_rows = std::max(max_rows, c.n);
        int block_h  = (max_rows + 1) * line_h + line_gap * 2;  // +1 for column headers
        int cy_top   = sh - block_h - hpad / 2;

        // Longest option column above is DISPLAY (header + 12 rows: 9 toggles plus
        // the board HUE/SAT/VAL steppers) — must track DISPLAY_ROWS in main.cpp's
        // match_menu_cols(). Skip the reference entirely rather than draw over it
        // when the window is too short.
        int options_bottom = ty + (line_h + line_gap) + 12 * line_h + line_gap * 2;
        if (cy_top < options_bottom) { SDL_RenderPresent(sdl); return; }

        SDL_Color col_header = {160, 160, 160, 255};
        for (int c = 0; c < 3; c++) {
            int cx = hpad + c * col_w;
            int cy = cy_top;
            draw_text(cx, cy, scale, cols[c].header, col_header);
            cy += line_h + line_gap;
            // Align descriptions within the column to its widest key
            int key_w = 0;
            for (int r = 0; r < cols[c].n; r++)
                key_w = std::max(key_w, text_width_px(cols[c].rows[r].key, scale));
            for (int r = 0; r < cols[c].n; r++) {
                draw_text(cx, cy, scale, cols[c].rows[r].key, Palette::ACCENT);
                draw_text(cx + key_w + scale * 6, cy, scale, cols[c].rows[r].desc, Palette::TEXT_DIM);
                cy += line_h;
            }
        }
    }

    SDL_RenderPresent(sdl);
}

Renderer::ListLayout Renderer::list_screen_layout(int total, int index) const {
    ListLayout L;
    int sw, sh;
    SDL_GetRendererOutputSize(sdl, &sw, &sh);

    L.scale    = (sw >= 900) ? 3 : 2;
    L.text_h   = 7 * L.scale;
    L.line_gap = (L.scale >= 3) ? 8 : 5;
    L.line_h   = L.text_h + L.line_gap;
    L.hpad     = 80;
    // Title occupies the first line; the list starts below it.
    L.top_y    = L.hpad / 2 + L.text_h + L.line_gap * 3;

    int avail_h  = sh - L.top_y - L.hpad;   // leave room for the footer
    L.max_lines  = std::max(4, avail_h / L.line_h);
    // Visible window keeps the highlighted row on screen. Derived purely from
    // index/total, so there is no stored scroll position to get out of step.
    L.scroll = 0;
    if (index >= L.max_lines) L.scroll = index - L.max_lines + 1;
    if (L.scroll > std::max(0, total - L.max_lines))
        L.scroll = std::max(0, total - L.max_lines);

    L.row_x = L.hpad - 6;
    L.row_w = sw - L.hpad * 2 + 12;
    return L;
}

void Renderer::draw_list_screen(const char* title, const std::vector<std::string>& lines,
                                int index, const char* footer, bool present,
                                const SDL_Color* line_colors, int hover) {
    int sw, sh;
    SDL_GetRendererOutputSize(sdl, &sw, &sh);

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    SDL_Rect bg = {0, 0, sw, sh};
    SDL_RenderFillRect(sdl, &bg);

    int total = (int)lines.size();
    ListLayout L = list_screen_layout(total, index);

    draw_text(L.hpad, L.hpad / 2, L.scale, title, Palette::ACCENT);

    for (int i = 0; i < L.max_lines; i++) {
        int li = L.scroll + i;
        if (li >= total) break;
        int ry = L.top_y + i * L.line_h;
        if (li == index || li == hover) {
            const SDL_Color& c = (li == index) ? Palette::CATALOG_SELECT
                                               : Palette::CATALOG_HOVER;
            SDL_Rect hi = {L.row_x, ry - 3, L.row_w, L.text_h + 6};
            SDL_SetRenderDrawColor(sdl, c.r, c.g, c.b, c.a);
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
            SDL_RenderFillRect(sdl, &hi);
        }
        SDL_Color tc = (li == index) ? Palette::ACCENT : Palette::TEXT_WHITE;
        if (line_colors && line_colors[li].a != 0) tc = line_colors[li];
        draw_text(L.hpad, ry, L.scale, lines[li].c_str(), tc);
    }

    if (footer && footer[0])
        draw_text(L.hpad, sh - L.hpad / 2 - L.text_h, L.scale, footer, Palette::TEXT_DIM);

    if (present)
        SDL_RenderPresent(sdl);
}

// Literal symbol glyphs for the five chars whose shared-font slot holds something
// else: ~ @ # ^ are the face-button shapes and * is the menu-cursor dot. Only the
// login screen needs these drawn as their real ASCII symbols, so they live here
// instead of in font_glyphs (which the rest of the app relies on for the reuse).
static bool login_symbol_glyph(char c, unsigned char out[7]) {
    static const unsigned char AT[7]   = {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E};
    static const unsigned char HASH[7] = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A};
    static const unsigned char CARET[7]= {0x04,0x0A,0x11,0x00,0x00,0x00,0x00};
    static const unsigned char TILDE[7]= {0x00,0x00,0x0D,0x16,0x00,0x00,0x00};
    static const unsigned char STAR[7] = {0x00,0x04,0x15,0x0E,0x15,0x04,0x00};
    const unsigned char* g = nullptr;
    switch (c) {
        case '@': g = AT;    break;
        case '#': g = HASH;  break;
        case '^': g = CARET; break;
        case '~': g = TILDE; break;
        case '*': g = STAR;  break;
        default: return false;
    }
    memcpy(out, g, 7);
    return true;
}

void Renderer::draw_credential_screen(int step, const std::string& username,
                                      const std::string& typed, const char* footer) {
    int sw, sh;
    SDL_GetRendererOutputSize(sdl, &sw, &sh);

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    SDL_Rect bg = {0, 0, sw, sh};
    SDL_RenderFillRect(sdl, &bg);

    int scale = (sw >= 900) ? 3 : 2;
    int th    = 7 * scale;
    int gap   = th;                 // roomy line spacing
    int hpad  = 6 * scale * 3;      // ~3 characters in from the edge

    draw_text(hpad, hpad / 2, scale, "OGS LOGIN", Palette::ACCENT);

    const SDL_Color base = Palette::TEXT_WHITE;   // lowercase / symbols
    const SDL_Color cap  = Palette::ACCENT;       // capitals — the case signal

    // A label, then the value with uppercase letters drawn in `cap`. The font is
    // uppercase-only (lowercase folds to the same glyph), so colour is the only way
    // to tell 'a' from 'A' — which is the whole point when checking a password.
    auto field = [&](int y, const char* lbl, const std::string& val, bool caret) {
        draw_text(hpad, y, scale, lbl, Palette::TEXT_SECONDARY);
        int pen = hpad + (int)strlen(lbl) * 6 * scale;
        std::string show = val + (caret ? "_" : "");
        for (unsigned char c : show) {
            bool up = (c >= 'A' && c <= 'Z');
            const SDL_Color& col = up ? cap : base;
            SDL_SetRenderDrawColor(sdl, col.r, col.g, col.b, col.a);
            unsigned char sym[7];
            const unsigned char* rows = login_symbol_glyph((char)c, sym)
                                            ? sym : get_glyph_rows((char)c);
            for (int r = 0; r < 7; r++)
                for (int cc = 0; cc < 5; cc++)
                    if (rows[r] & (1 << (4 - cc))) {
                        SDL_Rect px = {pen + cc * scale, y + r * scale, scale, scale};
                        SDL_RenderFillRect(sdl, &px);
                    }
            pen += 6 * scale;
        }
    };

    int y = sh / 3;
    field(y, "USERNAME:  ", (step <= 1) ? typed : username, step <= 1);
    if (step >= 2) {
        y += th + gap;
        field(y, "PASSWORD:  ", typed, true);
    }

    y += (th + gap) * 2;
    draw_text(hpad, y, scale,
              step <= 1 ? "TYPE YOUR OGS USERNAME, THEN ENTER"
                        : "TYPE YOUR OGS PASSWORD, THEN ENTER",
              Palette::TEXT_DIM);

    // The font has no distinct lowercase, so the accent colour is the case cue —
    // spell that out, with the word COLOR itself drawn in the accent to show it.
    y += th + gap;
    const char* leg = "CAPITAL LETTERS SHOWN IN ";
    draw_text(hpad, y, scale, leg, Palette::TEXT_DIM);
    draw_text(hpad + (int)strlen(leg) * 6 * scale, y, scale, "COLOR", Palette::ACCENT);

    // Watching, reviewing and analysis don't need an account.
    y += (th + gap) * 2;
    draw_text(hpad, y, scale, "ESC: CONTINUE WITHOUT LOGGING IN", Palette::TEXT_DIM);

    if (footer && footer[0])
        draw_text(hpad, sh - hpad / 2 - th, scale, footer, Palette::TEXT_DIM);

    SDL_RenderPresent(sdl);
}

int Renderer::list_screen_item_at(int total, int index, int mx, int my) const {
    if (total <= 0) return -1;
    ListLayout L = list_screen_layout(total, index);
    if (mx < L.row_x || mx >= L.row_x + L.row_w) return -1;
    // Full-stride rows, no inset dead zone — a list should be forgiving to click.
    int rel = my - (L.top_y - L.line_gap / 2);
    if (rel < 0) return -1;
    int row = rel / L.line_h;
    if (row < 0 || row >= L.max_lines) return -1;
    int li = L.scroll + row;
    return (li >= 0 && li < total) ? li : -1;
}

// Hint line under the popup items. It participates in the panel's width, so the
// layout and the draw must use the same string — hence one definition here.
static const char* const POPUP_HINT = GLYPH_PS_CROSS "=SELECT   " GLYPH_PS_CIRCLE "=CLOSE";

// How far a command sits in from its section heading, in character widths — a
// glyph advance is 6 * scale px, so this scales with the text like everything
// else in the panel. Enough that a heading visibly owns the rows beneath it,
// small enough that the list still reads as one column.
static const int POPUP_INDENT_CHARS = 2;

static inline int popup_indent_px(int scale) { return POPUP_INDENT_CHARS * 6 * scale; }

Renderer::PopupLayout Renderer::popup_layout(const char* title, const std::string* items,
                                             int count, const PopupStyle* styles) const {
    PopupLayout L;
    int sw, sh;
    SDL_GetRendererOutputSize(sdl, &sw, &sh);

    L.scale     = (sw >= 900) ? 3 : 2;
    L.text_h    = 7 * L.scale;
    L.line_gap  = (L.scale >= 3) ? 10 : 6;
    L.line_h    = L.text_h + L.line_gap;
    L.pad       = L.line_gap * 3;
    L.has_title = (title && title[0]);

    int w = text_width_px(POPUP_HINT, L.scale);
    if (L.has_title) w = std::max(w, text_width_px(title, L.scale));
    for (int i = 0; i < count; i++)
        w = std::max(w, text_width_px(items[i].c_str(), L.scale));
    // Commands under a heading are indented, so their rows need that much more
    // width before the longest of them would touch the panel edge.
    if (styles) w += popup_indent_px(L.scale);

    // Row offsets first: the panel's height depends on them once headings can
    // open a gap, so the box cannot be sized before the rows are placed.
    std::vector<int> offset;
    offset.reserve(count);
    int h = 0;
    for (int i = 0; i < count; i++) {
        if (styles && styles[i].heading && i > 0) h += L.line_gap * 2;
        offset.push_back(h);
        h += L.line_h;
    }
    L.rows_h = h;

    int box_w = w + L.pad * 2;
    int box_h = L.pad * 2 + L.rows_h + L.line_gap + L.text_h
              + (L.has_title ? L.line_h + L.line_gap : 0);
    L.box = {(sw - box_w) / 2, (sh - box_h) / 2, box_w, box_h};

    L.first_row_y = L.box.y + L.pad + (L.has_title ? L.line_h + L.line_gap : 0);
    L.row_x       = L.box.x + L.pad - 6;
    L.row_w       = box_w - L.pad * 2 + 12;
    L.row_y.reserve(count);
    for (int o : offset) L.row_y.push_back(L.first_row_y + o);
    return L;
}

void Renderer::draw_popup_menu(const char* title, const std::string* items,
                               int count, int index, const PopupStyle* styles) {
    if (!items || count <= 0) return;
    int sw, sh;
    SDL_GetRendererOutputSize(sdl, &sw, &sh);

    // Dim whatever is already on the backbuffer
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, 0, 0, 0, 140);
    SDL_Rect dim = {0, 0, sw, sh};
    SDL_RenderFillRect(sdl, &dim);

    PopupLayout L = popup_layout(title, items, count, styles);

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    SDL_RenderFillRect(sdl, &L.box);
    SDL_SetRenderDrawColor(sdl, Palette::ACCENT.r, Palette::ACCENT.g, Palette::ACCENT.b, 255);
    SDL_RenderDrawRect(sdl, &L.box);

    int tx = L.box.x + L.pad;
    if (L.has_title)
        draw_text(tx, L.box.y + L.pad, L.scale, title, Palette::ACCENT);

    for (int i = 0; i < count; i++) {
        int  ty      = L.row_y[i];
        bool heading = styles && styles[i].heading;

        if (heading) {
            // A rule running from the end of the label to the panel edge, so the
            // section reads as a divider without spending a whole row on one.
            int lw = text_width_px(items[i].c_str(), L.scale);
            int x0 = tx + lw + L.line_gap;
            int x1 = L.row_x + L.row_w - L.line_gap;
            if (x1 > x0) {
                SDL_Color c = styles[i].color;
                SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(sdl, c.r, c.g, c.b, 90);
                SDL_Rect rule = {x0, ty + L.text_h / 2, x1 - x0, std::max(1, L.scale - 1)};
                SDL_RenderFillRect(sdl, &rule);
            }
            draw_text(tx, ty, L.scale, items[i].c_str(), styles[i].color);
            continue;
        }

        int ix = tx + (styles ? popup_indent_px(L.scale) : 0);
        if (i == index) {
            SDL_Rect hi = {L.row_x, ty - 3, L.row_w, L.text_h + 6};
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(sdl, Palette::CATALOG_SELECT.r, Palette::CATALOG_SELECT.g,
                                   Palette::CATALOG_SELECT.b, Palette::CATALOG_SELECT.a);
            SDL_RenderFillRect(sdl, &hi);
        }
        draw_text(ix, ty, L.scale, items[i].c_str(),
                  i == index ? Palette::ACCENT
                             : (styles ? styles[i].color : Palette::TEXT_WHITE));
    }
    draw_text(tx, L.first_row_y + L.rows_h + L.line_gap, L.scale,
              POPUP_HINT, Palette::TEXT_DIM);
}

int Renderer::popup_item_at(const char* title, const std::string* items, int count,
                            int mx, int my, bool* inside_panel,
                            const PopupStyle* styles) const {
    if (inside_panel) *inside_panel = false;
    if (!items || count <= 0) return -1;

    PopupLayout L = popup_layout(title, items, count, styles);
    if (inside_panel)
        *inside_panel = (mx >= L.box.x && mx < L.box.x + L.box.w &&
                         my >= L.box.y && my < L.box.y + L.box.h);

    if (mx < L.row_x || mx >= L.row_x + L.row_w) return -1;
    // Rows are hit across their full stride (no inset dead zone like the board's —
    // a menu should be forgiving), with the band centred on the text. Walking the
    // rows rather than dividing by a stride, because section gaps make the rows
    // unevenly spaced; the gap above a heading belongs to neither neighbour.
    for (int i = 0; i < count; i++) {
        if (styles && styles[i].heading) continue;   // a label, not a command
        int top = L.row_y[i] - L.line_gap / 2;
        if (my >= top && my < top + L.line_h) return i;
    }
    return -1;
}

Renderer::CatalogLayout Renderer::catalog_layout(const BoardView& view, const Catalog& cat,
                                                 bool live_mode, bool readonly) const {
    CatalogLayout L;
    int total = (int)cat.entries.size();

    L.scale      = (view.square >= 30) ? 3 : 2;
    L.line_gap   = (L.scale >= 3) ? 8 : 4;
    L.th         = 7 * L.scale;
    L.pad        = (L.scale >= 3) ? 10 : 8;
    L.hpad       = 100;
    L.header_gap = L.line_gap + (L.scale >= 3 ? 4 : 2);
    L.line_h     = L.th + L.line_gap;

    // --- Header block ---
    // Close hint matches the input device: go_viewer's mouse/keyboard catalog
    // still says ESC; ogs_client (live_mode) has no keyboard here, so it's the
    // PS glyph instead (same live_mode-gated pattern render_help_overlay uses).
    const char* close_hint = live_mode ? GLYPH_PS_CIRCLE "=CLOSE" : "ESC to close";
    char title_buf[160];
    if (cat.search_mode) {
        L.title = "CATALOG  (ESC to clear search)";
    } else if (cat.virtual_player_mode && !cat.virtual_player.empty()) {
        snprintf(title_buf, sizeof(title_buf), "GAMES: %s  %s",
                 cat.virtual_player.c_str(), close_hint);
        L.title = title_buf;
    } else {
        // Read-only pro game library gets its own title so it's never mistaken
        // for the user's own (deletable) catalog.
        snprintf(title_buf, sizeof(title_buf), "%s  %s%s",
                 readonly ? "PRO GAME LIBRARY" : "CATALOG",
                 close_hint, readonly ? "  (READ-ONLY)" : "");
        L.title = title_buf;
    }

    // Every conditional header line lives here, once, so the list can never start
    // at a y the rows weren't measured against.
    int ty = L.pad;
    L.title_y = ty;  ty += L.th + L.header_gap;
    // Button legend — gamepad only (go_viewer's mouse/keyboard catalog has no
    // equivalent scheme).
    L.show_legend = live_mode;
    if (L.show_legend) { L.legend_y = ty; ty += L.th + L.header_gap; }

    L.index_ready   = cat.game_index.loaded();
    L.show_building = cat.game_index.is_loading() && !L.index_ready;
    if (L.show_building) { L.building_y = ty; ty += L.th + L.line_gap + (L.scale >= 3 ? 4 : 2); }

    L.show_search = (!cat.search_query.empty() || cat.search_mode);
    if (L.show_search) {
        L.search_y = ty; ty += L.th + L.line_gap;
        L.count_y  = ty; ty += L.th + L.line_gap + (L.scale >= 3 ? 4 : 2);
    }
    L.list_top_y = ty;

    // --- Measure list width and black-column width for aligned player names ---
    char lbl[1024];
    // Player rank is only ever interesting outside of live play (catalog review
    // has no "in-progress game" to protect), so this always includes it.
    auto name_rank = [](const std::string& name, const std::string& rank) -> std::string {
        std::string n = name.empty() ? "?" : name;
        if (!rank.empty()) n += " [" + rank + "]";
        return n;
    };
    L.vs_w    = text_width_px("vs", L.scale);
    L.col_gap = L.scale * 6;   // pixel gap between columns
    L.max_w   = text_width_px(L.title.c_str(), L.scale);

    for (int i = 0; i < total; i++) {
        const auto& e = cat.entries[i];
        int w;
        if (e.type == 0 && (!e.player_black.empty() || !e.player_white.empty())) {
            // Four-column: black  vs  white  date
            std::string bn = name_rank(e.player_black, e.player_black_rank);
            std::string wn = name_rank(e.player_white, e.player_white_rank);
            int bw = text_width_px(bn.c_str(), L.scale);
            int ww = text_width_px(wn.c_str(), L.scale);
            if (bw > L.max_black_w) L.max_black_w = bw;
            if (ww > L.max_white_w) L.max_white_w = ww;
            w = bw + L.col_gap + L.vs_w + L.col_gap + ww;
        } else {
            if      (e.type == 1) snprintf(lbl, sizeof(lbl), "[DIR] %s", e.display_name.c_str());
            else if (e.type == 2) snprintf(lbl, sizeof(lbl), "[..]");
            else if (e.type == 3) snprintf(lbl, sizeof(lbl), "[%s]", e.display_name.c_str());
            else                  snprintf(lbl, sizeof(lbl), "%s", e.display_name.c_str());
            w = text_width_px(lbl, L.scale);
        }
        if (w > L.max_w) L.max_w = w;
    }
    // Date column starts after the white name with extra gap
    L.date_col_x = L.hpad + L.max_black_w + L.col_gap + L.vs_w + L.col_gap
                 + L.max_white_w + L.col_gap * 3;

    // The loop above measured each row against its OWN name widths, but rows are
    // drawn column-aligned (every white name sits at the widest-black-name offset,
    // etc.) with a trailing date column — so a drawn row can extend past that
    // per-row max_w. Widen max_w to the real aligned extent, so the selection
    // highlight bar and the thumbnail area both clear the full drawn line.
    if (L.max_black_w > 0 || L.max_white_w > 0) {
        bool any_date = false;
        for (int i = 0; i < total && !any_date; i++)
            any_date = (cat.entries[i].type == 0 && !cat.entries[i].date.empty());
        int aligned_w = L.max_black_w + L.col_gap + L.vs_w + L.col_gap + L.max_white_w;
        if (any_date)
            aligned_w = (L.date_col_x - L.hpad) + text_width_px("0000-00-00", L.scale);
        L.max_w = std::max(L.max_w, aligned_w);
    }
    L.list_right = L.hpad + L.max_w;

    // --- Visible window ---
    int avail_h   = view.screen_h - L.list_top_y - L.pad;
    L.max_lines   = (avail_h > 0) ? (avail_h / L.line_h) : 4;
    if (L.max_lines < 4)     L.max_lines = 4;
    if (L.max_lines > total) L.max_lines = total;

    L.scroll = cat.scroll;
    if (cat.index < L.scroll)                 L.scroll = cat.index;
    if (cat.index >= L.scroll + L.max_lines)  L.scroll = cat.index - L.max_lines + 1;

    L.row_x = L.hpad - 3;
    L.row_w = L.max_w + 6;
    L.total = total;
    return L;
}

int Renderer::catalog_entry_at(int mx, int my) const {
    if (!drawn_catalog_layout_valid_) return -1;
    const CatalogLayout& L = drawn_catalog_layout_;
    if (L.total <= 0) return -1;
    if (mx < L.row_x || mx >= L.row_x + L.row_w) return -1;
    int rel = my - (L.list_top_y - L.line_gap / 2);
    if (rel < 0) return -1;
    int row = rel / L.line_h;
    if (row < 0 || row >= L.max_lines) return -1;
    int ei = L.scroll + row;
    return (ei >= 0 && ei < L.total) ? ei : -1;
}

void Renderer::render_catalog_overlay(const BoardView& view, const DrawState& ds) {
    const Catalog& cat = ds.catalog;
    if (!cat.active) return;

    // Full-screen panel
    SDL_Rect bg = {0, 0, view.screen_w, view.screen_h};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    SDL_RenderFillRect(sdl, &bg);

    int total = (int)cat.entries.size();
    CatalogLayout L = catalog_layout(view, cat, ds.live_mode, ds.catalog_readonly);
    // Publish it for catalog_entry_at — clicks hit-test the frame on screen rather
    // than paying for this width pass again on every mouse move.
    drawn_catalog_layout_       = L;
    drawn_catalog_layout_valid_ = true;
    const char* close_hint = ds.live_mode ? GLYPH_PS_CIRCLE "=CLOSE" : "ESC to close";
    int tx    = L.hpad;
    int scale = L.scale;

    // --- Header (positions all come from the layout) ---
    draw_text(tx, L.title_y, scale, L.title.c_str(), Palette::ACCENT);

    // Shown inline, in-context, so which physical button does what is never a
    // guessing game while actually browsing.
    if (L.show_legend) {
        std::string legend = GLYPH_PS_CROSS "=OPEN   " GLYPH_PS_SQUARE "=AUTOPLAY   ";
        if (!ds.catalog_readonly)
            legend += GLYPH_PS_TRIANGLE "x2=DELETE   ";
        legend += close_hint;
        draw_text(tx, L.legend_y, scale, legend.c_str(), Palette::TEXT_DIM);
    }
    if (L.show_building)
        draw_text(tx, L.building_y, scale, "Building index...", Palette::TEXT_WHITE);

    if (L.show_search) {
        char search_lbl[256];
        if (!L.index_ready && !cat.search_query.empty()) {
            snprintf(search_lbl, sizeof(search_lbl), "SEARCH: %s_  (indexing...)",
                     cat.search_query.c_str());
        } else {
            snprintf(search_lbl, sizeof(search_lbl), "SEARCH: %s_", cat.search_query.c_str());
        }
        draw_text(tx, L.search_y, scale, search_lbl, Palette::ACCENT);
        char count_lbl[64];
        snprintf(count_lbl, sizeof(count_lbl), "%d result%s", total, total == 1 ? "" : "s");
        draw_text(tx, L.count_y, scale, count_lbl, Palette::TEXT_WHITE);
    }

    char lbl[1024];
    auto name_rank = [](const std::string& name, const std::string& rank) -> std::string {
        std::string n = name.empty() ? "?" : name;
        if (!rank.empty()) n += " [" + rank + "]";
        return n;
    };
    int max_black_w = L.max_black_w, col_gap = L.col_gap, vs_w = L.vs_w;
    int date_col_x  = L.date_col_x,  list_right = L.list_right;

    // --- Thumbnails ---
    int thumb_inner_gap = 40;
    int thumb_vpad      = 40;
    int thumb_size      = (view.screen_h - thumb_vpad * 2 - thumb_inner_gap) * 4 / 10;
    int two_h           = thumb_size * 2 + thumb_inner_gap;
    int thumb_x         = list_right + (view.screen_w - list_right - thumb_size) / 2;
    int thumb_y_top     = (view.screen_h - two_h) / 2;

    bool has_thumb = ds.catalog_thumb_valid
                     && ds.catalog_thumb_open  != nullptr
                     && ds.catalog_thumb_final != nullptr;
    if (has_thumb) {
        if (ds.catalog_thumb_single) {
            // Single-position file (opening == final): one larger centered board
            // instead of the same picture twice.
            int single = std::min((view.screen_h - thumb_vpad * 2) * 6 / 10,
                                  view.screen_w - list_right - 80);
            if (single > 0) {
                int sx = list_right + (view.screen_w - list_right - single) / 2;
                render_mini_board(sx, (view.screen_h - single) / 2,
                                  single, ds.catalog_thumb_open, ds.catalog_thumb_board_size);
            }
        } else {
            render_mini_board(thumb_x, thumb_y_top,
                              thumb_size, ds.catalog_thumb_open,  ds.catalog_thumb_board_size);
            render_mini_board(thumb_x, thumb_y_top + thumb_size + thumb_inner_gap,
                              thumb_size, ds.catalog_thumb_final, ds.catalog_thumb_board_size);
        }
    }

    // --- Entry list ---
    int idx = cat.index;
    for (int i = 0; i < L.max_lines; i++) {
        int ei = L.scroll + i;
        if (ei >= total) break;
        const auto& e = cat.entries[ei];
        int ty = L.list_top_y + i * L.line_h;

        // Highlight bar for the selected entry. No hover variant on purpose — see
        // the catalog note in ogs_client's mouse-motion handler.
        if (ei == idx) {
            SDL_Rect hi = {L.row_x, ty - 3, L.row_w, L.th + 6};
            SDL_SetRenderDrawColor(sdl, Palette::CATALOG_SELECT.r, Palette::CATALOG_SELECT.g,
                                   Palette::CATALOG_SELECT.b, Palette::CATALOG_SELECT.a);
            SDL_RenderFillRect(sdl, &hi);
        }

        if (e.type == 0 && (!e.player_black.empty() || !e.player_white.empty())) {
            // Four-column layout: [black ACCENT] [vs WHITE] [white ACCENT] [date DIM]
            std::string bn = name_rank(e.player_black, e.player_black_rank);
            std::string wn = name_rank(e.player_white, e.player_white_rank);
            draw_text(tx,                                         ty, scale, bn.c_str(),   Palette::ACCENT);
            draw_text(tx + max_black_w + col_gap,                 ty, scale, "vs", Palette::TEXT_WHITE);
            draw_text(tx + max_black_w + col_gap + vs_w + col_gap, ty, scale, wn.c_str(),  Palette::ACCENT);
            if (!e.date.empty()) {
                // Show first 10 chars (covers YYYY-MM-DD)
                char dbuf[11];
                int dlen = (int)e.date.size();
                int show = dlen < 10 ? dlen : 10;
                memcpy(dbuf, e.date.c_str(), show);
                dbuf[show] = '\0';
                draw_text(date_col_x, ty, scale, dbuf, Palette::TEXT_DIM);
            }
        } else {
            if      (e.type == 1) snprintf(lbl, sizeof(lbl), "[DIR] %s", e.display_name.c_str());
            else if (e.type == 2) snprintf(lbl, sizeof(lbl), "[..]");
            else if (e.type == 3) snprintf(lbl, sizeof(lbl), "[%s]", e.display_name.c_str());
            else                  snprintf(lbl, sizeof(lbl), "%s", e.display_name.c_str());
            draw_text(tx, ty, scale, lbl, Palette::TEXT_WHITE);
        }
    }
}

// ── Freehand chalk annotation ───────────────────────────────────────────────
// Strokes live as pixels in their own full-screen texture rather than as a list of
// points replayed each frame: the layer is stamped once per segment and then just
// blitted, so an hour of scribbling costs exactly one blit per frame.

void Renderer::annot_ensure_layer() {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(sdl, &w, &h);
    if (annot_layer_ && annot_w_ == w && annot_h_ == h) return;

    // Size changed (or first use) — a screen-space scribble has no meaning at a
    // different resolution, so start clean rather than stretch it.
    if (annot_layer_) SDL_DestroyTexture(annot_layer_);
    annot_layer_ = SDL_CreateTexture(sdl, SDL_PIXELFORMAT_RGBA8888,
                                     SDL_TEXTUREACCESS_TARGET, w, h);
    if (!annot_layer_) return;
    SDL_SetTextureBlendMode(annot_layer_, SDL_BLENDMODE_BLEND);
    annot_w_ = w;
    annot_h_ = h;
    annot_any_ = false;

    SDL_Texture* prev = SDL_GetRenderTarget(sdl);
    SDL_SetRenderTarget(sdl, annot_layer_);
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, 0, 0, 0, 0);
    SDL_RenderClear(sdl);
    SDL_SetRenderTarget(sdl, prev);
}

void Renderer::annot_segment(int x0, int y0, int x1, int y1, bool dark) {
    annot_ensure_layer();
    if (!annot_layer_) return;

    // draw_thick_line collapses a zero-length segment to a single 1px point, which
    // would make a pen tap invisible. Give it a hair of direction so the tap lands
    // as a proper round-ish chalk mark at full width.
    if (x0 == x1 && y0 == y1) x1 += 1;

    int sw = 0, sh = 0;
    SDL_GetRendererOutputSize(sdl, &sw, &sh);
    // Scale with the display so the line feels the same weight on any monitor.
    // Roughly 9px at 1080p, 12px at 1440p, 18px at 4K — a smaller divisor is a
    // fatter stick of chalk.
    int core = std::max(5, sh / 120);

    SDL_Texture* prev = SDL_GetRenderTarget(sdl);
    SDL_SetRenderTarget(sdl, annot_layer_);
    draw_thick_line(x0, y0, x1, y1, core, dark ? Palette::CHALK_DARK : Palette::CHALK);
    SDL_SetRenderTarget(sdl, prev);
    annot_any_ = true;
}

void Renderer::annot_clear() {
    if (!annot_layer_) { annot_any_ = false; return; }
    SDL_Texture* prev = SDL_GetRenderTarget(sdl);
    SDL_SetRenderTarget(sdl, annot_layer_);
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, 0, 0, 0, 0);
    SDL_RenderClear(sdl);
    SDL_SetRenderTarget(sdl, prev);
    annot_any_ = false;
}

// Draw a dashed line between two pixel points, alternating drawn/skipped segments.
void Renderer::draw_dashed_line(int x1, int y1, int x2, int y2, int dash_len, int gap_len) {
    int dx = x2 - x1, dy = y2 - y1;
    int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps == 0) return;
    float sx = (float)dx / steps, sy = (float)dy / steps;
    int period = dash_len + gap_len;
    for (int i = 0; i <= steps; i++) {
        if ((i % period) < dash_len) {
            int px = x1 + (int)(sx * i);
            int py = y1 + (int)(sy * i);
            SDL_RenderDrawPoint(sdl, px, py);
        }
    }
}

void Renderer::render_box_selection(const BoardView& view, const DrawState& ds) {
    bool has_committed = (ds.box_sel_pts != nullptr && ds.box_sel_count > 0);
    if (!has_committed && !ds.box_drag_active) return;

    int half   = view.square / 2;
    int dot_r  = std::max(2, view.square / 5);

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);

    // --- Draw committed intersection dots ---
    if (has_committed) {
        SDL_SetRenderDrawColor(sdl, Palette::BOX_SELECT.r, Palette::BOX_SELECT.g,
                               Palette::BOX_SELECT.b, Palette::BOX_SELECT.a);
        for (int r = 0; r < view.active_size; r++) {
            for (int f = 0; f < view.active_size; f++) {
                if (!ds.box_sel_pts[r][f]) continue;
                int x, y;
                board_to_screen(view, r, f, x, y);
                fill_circle(x + half, y + half, dot_r);
            }
        }
    }

    // --- Draw dashed rubber-band rectangle for active drag ---
    if (ds.box_drag_active) {
        int r1 = ds.box_drag_r1, f1 = ds.box_drag_f1;
        int r2 = ds.box_drag_r2, f2 = ds.box_drag_f2;
        int rmin = std::min(r1, r2), rmax = std::max(r1, r2);
        int fmin = std::min(f1, f2), fmax = std::max(f1, f2);

        int x1, y1, x2, y2;
        board_to_screen(view, rmin, fmin, x1, y1);
        board_to_screen(view, rmax, fmax, x2, y2);
        // Shift to intersection centers
        x1 += half; y1 += half;
        x2 += half; y2 += half;

        // Thick-ish dashed rect: draw 2px wide by offsetting
        SDL_Color dc = Palette::BOX_SELECT;
        SDL_SetRenderDrawColor(sdl, dc.r, dc.g, dc.b, dc.a);
        int dash = 6, gap = 4;
        for (int t = 0; t <= 1; t++) {
            draw_dashed_line(x1-t, y1-t, x2+t, y1-t, dash, gap);  // top
            draw_dashed_line(x1-t, y2+t, x2+t, y2+t, dash, gap);  // bottom
            draw_dashed_line(x1-t, y1-t, x1-t, y2+t, dash, gap);  // left
            draw_dashed_line(x2+t, y1-t, x2+t, y2+t, dash, gap);  // right
        }
    }

    // --- Count label on the right side of the board ---
    int total = ds.box_sel_count;
    // Add drag preview intersections to the count display
    if (ds.box_drag_active) {
        int r1 = ds.box_drag_r1, f1 = ds.box_drag_f1;
        int r2 = ds.box_drag_r2, f2 = ds.box_drag_f2;
        int rmin = std::min(r1, r2), rmax = std::max(r1, r2);
        int fmin = std::min(f1, f2), fmax = std::max(f1, f2);
        total += (rmax - rmin + 1) * (fmax - fmin + 1);
    }
    if (total > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", total);
        int scale  = (view.square >= 30) ? 3 : 2;
        int margin = (view.square >= 30) ? 16 : 8;
        int tx = view.offset_x + view.board_px + view.margin + margin;
        int ty = view.offset_y - view.margin + (view.board_px + 2 * view.margin) / 2 - 7 * scale / 2;
        draw_text(tx, ty, scale, buf, Palette::ACCENT);
    }
}

void Renderer::render_game_comment(const BoardView& view, const DrawState& ds) {
    if (ds.game_comment.empty() || ds.catalog.active) return;

    int pad     = 10;   // gap between screen/board edges and the box
    int box_pad = 14;   // interior padding of the box

    // The box fills the gutter beside the board: left normally, RIGHT in puzzle
    // mode (the solution tree occupies the left panel there)
    int left_x, right_x;
    if (ds.puzzle_mode) {
        left_x  = view.offset_x + view.board_px + view.margin + pad;
        right_x = view.screen_w - pad;
    } else {
        left_x  = pad;
        right_x = view.offset_x - view.margin - pad;
    }
    int box_w = right_x - left_x;
    int inner_w = box_w - box_pad * 2;
    if (inner_w < 60) return;  // too narrow to be useful

    int bg_top    = view.offset_y - view.margin;
    int bg_bottom = view.offset_y + view.board_px + view.margin;
    int avail_h   = (bg_bottom - bg_top) - box_pad * 2;

    // Word-wrap at a given scale; returns the wrapped lines.
    auto wrap = [&](int scale) {
        std::vector<std::string> lines;
        std::string current, word;
        auto flush_word = [&]() {
            if (word.empty()) return;
            std::string candidate = current.empty() ? word : current + " " + word;
            if (text_width_px(candidate.c_str(), scale) <= inner_w) {
                current = std::move(candidate);
            } else {
                if (!current.empty()) lines.push_back(current);
                current = word;
            }
            word.clear();
        };
        for (char c : ds.game_comment) {
            if (c == ' ' || c == '\t') {
                flush_word();
            } else if (c == '\n' || c == '\r') {
                flush_word();
                lines.push_back(current);
                current.clear();
            } else {
                // If a single word is wider than the column, break it mid-word
                word += c;
                if (text_width_px(word.c_str(), scale) > inner_w && word.size() > 1) {
                    word.pop_back();
                    lines.push_back(word);
                    word.clear();
                    word += c;
                }
            }
        }
        flush_word();
        if (!current.empty()) lines.push_back(current);
        return lines;
    };

    // Biggest font whose wrapped block still fits the gutter height
    std::vector<std::string> lines;
    int scale = 2;
    for (int s = 4; s >= 2; s--) {
        auto wrapped = wrap(s);
        int lh      = 7 * s + s + 2;
        int total_h = (int)wrapped.size() * lh - (s + 2);
        if (total_h <= avail_h || s == 2) {
            scale = s;
            lines = std::move(wrapped);
            break;
        }
    }
    if (lines.empty()) return;

    int th       = 7 * scale;
    int line_gap = scale + 2;
    int lh       = th + line_gap;
    int total_h  = (int)lines.size() * lh - line_gap;
    if (total_h > avail_h) total_h = avail_h;  // clip pathological walls of text

    // Box vertically centred in the board's span
    int box_h = total_h + box_pad * 2;
    int box_y = bg_top + (bg_bottom - bg_top - box_h) / 2;
    SDL_Rect box = {left_x, box_y, box_w, box_h};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, Palette::OVERLAY_DARK.r, Palette::OVERLAY_DARK.g,
                           Palette::OVERLAY_DARK.b, Palette::OVERLAY_DARK.a);
    SDL_RenderFillRect(sdl, &box);
    SDL_SetRenderDrawColor(sdl, Palette::TEXT_DIM.r, Palette::TEXT_DIM.g,
                           Palette::TEXT_DIM.b, 160);
    SDL_RenderDrawRect(sdl, &box);

    int ty = box_y + box_pad;
    for (const auto& line : lines) {
        if (ty + th > box_y + box_h - box_pad) break;  // never draw past the box
        draw_text(left_x + box_pad, ty, scale, line.c_str(), Palette::TEXT_SECONDARY);
        ty += lh;
    }
}

void Renderer::render_save_input(const BoardView& view, const DrawState& ds) {
    if (ds.save_input_step == 0) return;

    int scale    = (view.square >= 30) ? 3 : 2;
    int th       = 7 * scale;
    int line_gap = scale + 2;
    int lh       = th + line_gap;
    int pad      = (scale >= 3) ? 16 : 10;

    // Lines to display
    const char* title   = "SAVE POSITION";
    const char* prompt  = (ds.save_input_step == 1) ? "Name:" : "Note:";
    const char* hint1   = "ENTER to continue";
    const char* hint2   = "ESC to cancel";
    if (ds.save_input_step == 2) hint1 = "ENTER to save";

    // Cursor-appended input text
    std::string input_display = ds.save_input_buf + "_";

    // Measure
    int max_w = text_width_px(title, scale);
    auto mw = [&](const char* s) { int w = text_width_px(s, scale); if (w > max_w) max_w = w; };
    mw(hint1); mw(hint2);
    {
        // "Name: text_" or "Note: text_"
        std::string full = std::string(prompt) + " " + input_display;
        mw(full.c_str());
    }

    int bw = max_w + pad * 2;
    // Layout: title, blank, prompt+input, blank, hint1, hint2
    // Each "blank" is one lh advance; hint2 ends with th (no trailing gap).
    int bh = lh * 5 + th + pad * 2;
    int bx = (view.screen_w - bw) / 2;
    int by = (view.screen_h - bh) / 2;

    SDL_Rect bg = {bx, by, bw, bh};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, Palette::OVERLAY_DARK.r, Palette::OVERLAY_DARK.g,
                           Palette::OVERLAY_DARK.b, Palette::OVERLAY_DARK.a);
    SDL_RenderFillRect(sdl, &bg);

    int tx = bx + pad;
    int ty = by + pad;
    draw_text(tx, ty, scale, title, Palette::ACCENT);
    ty += lh * 2;  // blank line after title

    // "Name: typed_text_"
    draw_text(tx, ty, scale, prompt, Palette::TEXT_WHITE);
    int prompt_w = text_width_px(prompt, scale) + scale * 2;
    draw_text(tx + prompt_w, ty, scale, input_display.c_str(), Palette::ACCENT);
    ty += lh * 2;  // blank line

    draw_text(tx, ty, scale, hint1, Palette::TEXT_DIM);
    ty += lh;
    draw_text(tx, ty, scale, hint2, Palette::TEXT_DIM);
}

void Renderer::render_flash_message(const BoardView& view, const DrawState& ds) {
    if (ds.flash_message.empty() || ds.flash_message_until == 0) return;
    Uint32 now = SDL_GetTicks();
    if (now >= ds.flash_message_until) return;

    const char* txt = ds.flash_message.c_str();
    int scale = (view.square >= 30) ? 3 : 2;
    int tw    = text_width_px(txt, scale);
    int th    = 7 * scale;
    int pad   = (scale >= 3) ? 10 : 7;
    int bw    = tw + pad * 2;
    int bh    = th + pad * 2;
    int bx    = (view.screen_w - bw) / 2;
    int by    = view.screen_h - bh - pad * 2;

    SDL_Rect bg = {bx, by, bw, bh};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, 20, 90, 20, 220);
    SDL_RenderFillRect(sdl, &bg);
    draw_text(bx + pad, by + pad, scale, txt, Palette::TEXT_WHITE);
}

void Renderer::render_quit_confirm(const BoardView& view) {
    int scale = (view.square >= 30) ? 3 : 2;
    int th    = 7 * scale;
    int pad   = scale >= 3 ? 16 : 12;

    const char* line1 = "QUIT?";
    const char* line2 = "press Q again to quit";
    const char* line3 = "or ESC to cancel";
    int gap   = scale >= 3 ? 6 : 4;
    int lh    = th + gap;
    int w     = text_width_px(line2, scale);
    int bw    = w + pad * 2;
    int bh    = th + lh * 2 + pad * 2;
    int bx    = (view.screen_w - bw) / 2;
    int by    = (view.screen_h - bh) / 2;

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    SDL_Rect bg = {bx, by, bw, bh};
    SDL_RenderFillRect(sdl, &bg);

    int tx = bx + pad;
    int ty = by + pad;
    draw_text(tx, ty, scale, line1, Palette::ACCENT);
    ty += lh;
    draw_text(tx, ty, scale, line2, Palette::TEXT_PRIMARY);
    ty += lh;
    draw_text(tx, ty, scale, line3, Palette::TEXT_DIM);
}

void Renderer::render_territory_overlay(const BoardView& view, const DrawState& ds) {
    if (!ds.territory_drill) return;
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int th     = 7 * scale;
    int lh     = th + 4;
    int tx     = view.offset_x + view.board_px + view.margin + margin;
    int ty     = view.offset_y - view.margin + (view.board_px + 2 * view.margin) / 2 - lh * 2;

    SDL_Color white  = Palette::SCORE_TEXT;
    SDL_Color yellow = Palette::ACCENT;
    SDL_Color green  = Palette::SCORE_CORRECT;
    SDL_Color red    = {255, 100, 100, 255};

    if (!ds.territory_answered) {
        draw_text(tx, ty,      scale, "WHICH IS",  white);
        draw_text(tx, ty + lh, scale, "LARGER?",   white);
        draw_text(tx, ty + lh*3, scale, "B OR W",  yellow);
    } else {
        draw_text(tx, ty, scale,
                  ds.territory_correct ? "CORRECT!" : "WRONG",
                  ds.territory_correct ? green : red);
        char buf[32];
        snprintf(buf, sizeof(buf), "BLACK %d", ds.territory_b_score);
        draw_text(tx, ty + lh*2, scale, buf, white);
        snprintf(buf, sizeof(buf), "WHITE %d", ds.territory_w_score);
        draw_text(tx, ty + lh*3, scale, buf, white);
        draw_text(tx, ty + lh*5, scale, "SPACE",    white);
        draw_text(tx, ty + lh*6, scale, "FOR NEXT", white);
    }
}

// "Who won?" quiz. Sits in the panel left of the board (the board itself is drawn
// in free_mode, so nothing else competes for that space). Before answering, only
// the position is on screen — players and result are withheld so nothing gives the
// result away; the reveal fills them in.
void Renderer::render_who_won_overlay(const BoardView& view, const DrawState& ds) {
    if (!ds.who_won_active) return;
    int scale = (view.square >= 30) ? 3 : 2;
    int pad   = (view.square >= 30) ? 16 : 8;
    int th    = 7 * scale;
    int lh    = th + 6;
    int tx    = pad * 2;
    int avail = view.offset_x - view.margin - tx - pad;

    SDL_Color white  = Palette::SCORE_TEXT;
    SDL_Color dim    = Palette::TEXT_SECONDARY;
    SDL_Color accent = Palette::ACCENT;
    SDL_Color green  = Palette::SCORE_CORRECT;
    SDL_Color red    = {255, 100, 100, 255};

    // Shrink to fit the panel rather than overflowing onto the board.
    auto fit = [&](const char* s, int sc) {
        while (sc > 1 && text_width_px(s, sc) > avail) sc--;
        return sc;
    };

    int y = view.offset_y - view.margin + pad;
    draw_text(tx, y, scale, "WHO WON?", accent);
    y += lh * 2;

    if (!ds.who_won_answered) {
        draw_text(tx, y, scale, "GUESS THE", white);      y += lh;
        draw_text(tx, y, scale, "WINNER:",   white);      y += lh * 2;
        draw_text(tx, y, scale, "< BLACK",   white);      y += lh;
        draw_text(tx, y, scale, "WHITE >",   white);      y += lh * 2;
        draw_text(tx, y, scale, "DPAD L/R",  dim);
    } else {
        draw_text(tx, y, scale,
                  ds.who_won_correct ? "CORRECT!" : "WRONG",
                  ds.who_won_correct ? green : red);
        y += lh * 2;

        if (!ds.result_message.empty()) {
            int s = fit(ds.result_message.c_str(), scale + 1);
            draw_text(tx, y, s, ds.result_message.c_str(), accent);
            y += s * 7 + 12;
        }
        // Players, one per line — pro names are long, so they get their own rows.
        if (!ds.black_name.empty()) {
            draw_text(tx, y, fit("B", scale), "B", dim);
            int s = fit(ds.black_name.c_str(), scale);
            draw_text(tx + 6 * scale * 2, y, s, ds.black_name.c_str(), white);
            y += lh;
        }
        if (!ds.white_name.empty()) {
            draw_text(tx, y, fit("W", scale), "W", dim);
            int s = fit(ds.white_name.c_str(), scale);
            draw_text(tx + 6 * scale * 2, y, s, ds.white_name.c_str(), white);
            y += lh;
        }
        if (!ds.game_date.empty()) {
            y += lh / 2;
            draw_text(tx, y, fit(ds.game_date.c_str(), scale), ds.game_date.c_str(), dim);
            y += lh;
        }
        y += lh;
        draw_text(tx, y, scale, GLYPH_PS_CROSS " NEXT", accent);
    }

    // Running tally, pinned to the bottom of the panel.
    if (ds.who_won_total > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", ds.who_won_right, ds.who_won_total);
        int by = view.offset_y - view.margin + view.board_px + 2 * view.margin - pad - th;
        draw_text(tx, by - lh, scale, "SCORE", dim);
        draw_text(tx, by,      scale, buf,     white);
    }
}

// ---------------------------------------------------------------------------
// Analysis tree panel (post-game review, ogs_client only)

void Renderer::render_analysis_tree(const BoardView& view, const DrawState& ds) {
    if (!ds.live_analysis_tree || ds.live_analysis_tree_count == 0) return;

    int avail_w = view.offset_x - view.margin;
    if (avail_w < 60) return;

    // Panel: 80% of left panel width, 55% of screen height.
    // Positioned so tree + gap + score graph are vertically centred together.
    int panel_w    = avail_w * 4 / 5;
    int panel_h    = view.screen_h * 11 / 20;
    int graph_h    = view.screen_h / 5;
    int tree_gap   = 20;
    int combo_h    = panel_h + tree_gap + graph_h;
    int panel_left = (avail_w - panel_w) / 2;
    int panel_top  = (view.screen_h - combo_h) / 2;

    // Panel background — match board colour so black stones read clearly
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
    SDL_Rect panel_rect = {panel_left, panel_top, panel_w, panel_h};
    SDL_RenderFillRect(sdl, &panel_rect);
    SDL_SetRenderDrawColor(sdl, 55, 65, 80, 255);
    SDL_RenderDrawRect(sdl, &panel_rect);

    SDL_RenderSetClipRect(sdl, &panel_rect);

    const int pad = 10;
    // Node geometry scales down for wide trees so every column fits the panel
    // instead of clipping off the right edge (some OGS puzzles fan out 20+ ways).
    int max_col = 0;
    for (int i = 0; i < ds.live_analysis_tree_count; i++)
        max_col = std::max(max_col, ds.live_analysis_tree[i].col);
    int inner_w = panel_w - 2 * pad;
    int col_w   = 42;
    if ((max_col + 1) * col_w > inner_w)
        col_w = std::max(8, inner_w / (max_col + 1));
    int r_node = std::min(15, std::max(3, col_w / 2 - 2));
    int row_h  = std::max(18, r_node * 3);   // rows shrink with the nodes
    int thick  = std::max(1, r_node / 5);    // connector line weight follows too

    int inner_h    = panel_h - 2 * pad;
    int visible_rows = inner_h / row_h;
    int scroll     = std::max(0, ds.live_analysis_tree_cur_depth - visible_rows * 2 / 3);

    // Column 0 x-centre
    int x_origin = panel_left + pad + r_node;

    auto node_x = [&](int col) { return x_origin + col * col_w; };
    auto node_y = [&](int depth) {
        return panel_top + pad + (depth - scroll) * row_h + row_h / 2;
    };

    int vis_top    = panel_top + pad - row_h;
    int vis_bottom = panel_top + pad + visible_rows * row_h + row_h;

    // Yellow row highlights for marked moves (drawn first, behind everything)
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, 255, 220, 40, 35);
    for (int i = 0; i < ds.live_analysis_tree_count; i++) {
        const auto& n = ds.live_analysis_tree[i];
        if (!n.marked) continue;
        int cy = node_y(n.depth);
        if (cy < vis_top || cy > vis_bottom) continue;
        SDL_Rect row = {panel_left + 1, cy - row_h / 2, panel_w - 2, row_h};
        SDL_RenderFillRect(sdl, &row);
    }

    // Lines (bold, drawn before nodes so circles appear on top)
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_Color line_col = {150, 168, 195, 255};
    for (int i = 0; i < ds.live_analysis_tree_count; i++) {
        const auto& n = ds.live_analysis_tree[i];
        if (n.parent_depth < 0) continue;

        int ny = node_y(n.depth);
        int py = node_y(n.parent_depth);
        int nx = node_x(n.col);
        int px = node_x(n.parent_col);

        if (ny < vis_top && py < vis_top) continue;
        if (ny > vis_bottom && py > vis_bottom) continue;

        if (n.col == n.parent_col) {
            draw_thick_line(nx, py, nx, ny, thick, line_col);
        } else {
            int bend_y = py + row_h / 2;
            draw_thick_line(px, py, px, bend_y, thick, line_col);
            draw_thick_line(px, bend_y, nx, bend_y, thick, line_col);
            draw_thick_line(nx, bend_y, nx, ny, thick, line_col);
        }
    }

    // Node circles
    for (int i = 0; i < ds.live_analysis_tree_count; i++) {
        const auto& n = ds.live_analysis_tree[i];
        int ny = node_y(n.depth);
        int nx = node_x(n.col);

        if (ny < vis_top || ny > vis_bottom) continue;

        if (n.current) {
            SDL_SetRenderDrawColor(sdl, 255, 235, 80, 255);
            fill_circle(nx, ny, r_node + std::max(2, r_node / 4));
        } else if (n.goal) {
            // Solution endpoint — green halo on the node (puzzle trees)
            SDL_SetRenderDrawColor(sdl, 70, 205, 130, 255);
            fill_circle(nx, ny, r_node + std::max(2, r_node / 4));
        }

        if (n.move_color == 1) {
            SDL_SetRenderDrawColor(sdl, 38, 38, 38, 255);
            fill_circle(nx, ny, r_node);
        } else if (n.move_color == 0) {
            SDL_SetRenderDrawColor(sdl, 215, 215, 215, 255);
            fill_circle(nx, ny, r_node);
        } else {
            // Root — hollow ring
            SDL_SetRenderDrawColor(sdl, 85, 100, 120, 255);
            fill_circle(nx, ny, r_node);
            SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
            fill_circle(nx, ny, std::max(1, r_node - 3));
        }
    }

    SDL_RenderSetClipRect(sdl, nullptr);
}

// ---------------------------------------------------------------------------
// Score graph (post-game review, below the analysis tree)

void Renderer::render_score_graph(const BoardView& view, const DrawState& ds) {
    if (!ds.live_score_graph || ds.live_score_graph_len == 0) return;

    int avail_w = view.offset_x - view.margin;
    if (avail_w < 60) return;

    // Mirror the layout constants from render_analysis_tree so they stay aligned.
    int panel_w   = avail_w * 4 / 5;
    int panel_h   = view.screen_h * 11 / 20;
    int graph_h   = view.screen_h / 5;
    int tree_gap  = 20;
    int combo_h   = panel_h + tree_gap + graph_h;
    int panel_left = (avail_w - panel_w) / 2;
    int graph_top  = (view.screen_h - combo_h) / 2 + panel_h + tree_gap;

    if (graph_h < 30) return;

    // Panel background
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
    SDL_Rect bg = {panel_left, graph_top, panel_w, graph_h};
    SDL_RenderFillRect(sdl, &bg);
    SDL_SetRenderDrawColor(sdl, 55, 65, 80, 255);
    SDL_RenderDrawRect(sdl, &bg);

    SDL_RenderSetClipRect(sdl, &bg);

    const int pad     = 6;
    const int inner_l = panel_left + pad;
    const int inner_r = panel_left + panel_w - pad;
    const int inner_t = graph_top  + pad;
    const int inner_b = graph_top  + graph_h - pad;
    const int inner_w = inner_r - inner_l;
    const int inner_h = inner_b - inner_t;
    const int mid_y   = inner_t + inner_h / 2;   // y = zero line

    int n = ds.live_score_graph_len;

    // Auto-scale: find max |score| among known values, round up to 10-point multiple
    float max_abs = 10.f;
    for (int i = 0; i < n; i++) {
        float v = ds.live_score_graph[i];
        if (v != FLT_MAX) {
            float a = v < 0.f ? -v : v;
            if (a > max_abs) max_abs = a;
        }
    }
    max_abs = floorf(max_abs / 10.f + 1.f) * 10.f;

    auto score_to_y = [&](float s) -> int {
        float c = s < -max_abs ? -max_abs : (s > max_abs ? max_abs : s);
        return mid_y - (int)(c / max_abs * (float)(inner_h / 2));
    };
    auto depth_to_x = [&](int d) -> int {
        if (n <= 1) return inner_l;
        return inner_l + (int)((float)d / (float)(n - 1) * (float)inner_w);
    };

    // Riemann-sum-style area fill: one discrete rectangle per move (dx = inner_w / n),
    // full box height — white from the top edge down to the curve, black from the
    // curve down to the bottom edge. Not relative to the zero line: white's region
    // grows as White's lead grows (curve moves down, away from the top edge), black's
    // grows as Black's lead grows (curve moves up, away from the bottom edge) — same
    // "each side grows with its own superiority" property, just measured against the
    // box edges rather than the midline. Gaps (unknown depth) are left unfilled.
    if (n > 0) {
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
        SDL_Color black_fill = {20, 20, 22, 255};
        SDL_Color white_fill = {218, 218, 218, 255};
        for (int i = 0; i < n; i++) {
            float v = ds.live_score_graph[i];
            if (v == FLT_MAX) continue;
            int x0 = inner_l + (int)((float)i     / (float)n * (float)inner_w);
            int x1 = inner_l + (int)((float)(i+1) / (float)n * (float)inner_w);
            if (x1 <= x0) x1 = x0 + 1;
            int y = score_to_y(v);
            SDL_Rect white_rect = {x0, inner_t, x1 - x0, y - inner_t};
            SDL_Rect black_rect = {x0, y, x1 - x0, inner_b - y};
            SDL_SetRenderDrawColor(sdl, white_fill.r, white_fill.g, white_fill.b, white_fill.a);
            SDL_RenderFillRect(sdl, &white_rect);
            SDL_SetRenderDrawColor(sdl, black_fill.r, black_fill.g, black_fill.b, black_fill.a);
            SDL_RenderFillRect(sdl, &black_rect);
        }
    }

    // Zero line — drawn after the fill so it stays visible as a crisp reference line
    draw_thick_line(inner_l, mid_y, inner_r, mid_y, 3, {130, 155, 180, 255});

    // Current-position scan line: vertical yellow stripe
    int cur = ds.live_score_graph_cur;
    if (cur >= 0 && cur < n) {
        int cx = depth_to_x(cur);
        draw_thick_line(cx, inner_t, cx, inner_b, 2, {Palette::ACCENT.r, Palette::ACCENT.g, Palette::ACCENT.b, 200});
    }

    // Axis labels (small, only when there is room). Shadowed now that the fill
    // beneath them can be solid black or white rather than the mid-tone board colour.
    if (inner_h >= 40 && inner_w >= 50) {
        char buf[16];
        snprintf(buf, sizeof(buf), "B+%.0f", max_abs);
        draw_text(inner_l + 3, inner_t + 3, 1, buf, {0, 0, 0, 180});
        draw_text(inner_l + 2, inner_t + 2, 1, buf, {140, 220, 180, 230});
        snprintf(buf, sizeof(buf), "W+%.0f", max_abs);
        draw_text(inner_l + 3, inner_b - 8, 1, buf, {0, 0, 0, 180});
        draw_text(inner_l + 2, inner_b - 9, 1, buf, {230, 140, 160, 230});
    }

    SDL_RenderSetClipRect(sdl, nullptr);
}

// ---------------------------------------------------------------------------
// Main draw entry points

void Renderer::draw_board(const DrawState& ds) {
    BoardView view;
    get_board_view(view, ds.active_board_size);
    square_stones_  = ds.square_stones;   // read by shade_stone (also hashed for the cache)
    octagon_stones_ = ds.octagon_stones;  // ditto — and by draw_stone_circle, which sizes octagons
                                          // to the full cell so neighbours share an edge
    update_capture_anim(ds);
    render_board(view, nullptr, ds);
}

// ---------------------------------------------------------------------------
// Tetris capture animation — the Game Boy Tetris line clear: a finished row
// blinks between its blocks and a flat light fill a few times, then drops out.
// Here the "row" is whatever a capture just took off the board.

namespace {
// Timed in presented frames, not milliseconds, the way the original was: the
// screen only changes on vsync, so a wall-clock step (80ms = 4.8 frames at 60Hz)
// rounds to an uneven 5,5,5,4... frame rhythm. A whole number of frames per step
// keeps every blink the same length.
constexpr int CAPTURE_PHASE_MS = 80;  // target length of one blink half-cycle
constexpr int CAPTURE_PHASES   = 7;   // flash, stone, flash, stone, flash, stone, flash — then gone
}

void Renderer::update_capture_anim(const DrawState& ds) {
    // Territory drill and the Chinese fill view draw a board that isn't the game
    // position; diffing across them would read as a burst of phantom moves. Drop
    // the snapshot instead, so the first real frame afterwards just re-baselines.
    if (ds.territory_board || ds.live_ownership_fill) {
        anim_prev_n_ = 0;
        capture_anim_.clear();
        return;
    }
    const char (*board)[MAX_BOARD_SIZE] =
        (ds.analysis_mode && ds.analysis) ? ds.analysis->board : ds.game.board;
    int n = ds.active_board_size;

    if (ds.tetris_captures && n == anim_prev_n_) {
        // A capture reads as exactly one stone appearing and one or more stones of
        // the other colour vanishing, nothing else. Stepping back through history,
        // loading a different game or jumping several moves never fits that shape,
        // so only a real capture (played, received, or replayed forward) blinks.
        int placed = 0, placed_color = 0;
        bool other_change = false;
        std::vector<CapturedStone> gone;
        for (int r = 0; r < n && !other_change; r++)
            for (int f = 0; f < n; f++) {
                char a = anim_prev_[r][f], b = board[r][f];
                if (a == b) continue;
                if (a == 0)      { placed++; placed_color = b; }
                else if (b == 0) gone.push_back({r, f, a == 1});
                else             { other_change = true; break; }
            }
        bool capture = !other_change && placed == 1 && !gone.empty();
        for (const auto& g : gone)
            if ((g.is_black ? 1 : 2) == placed_color) capture = false;
        if (capture) {
            capture_anim_       = std::move(gone);
            capture_anim_frame_ = 0;
            // Frames per step for this display's refresh rate (5 at 60Hz, 12 at
            // 144Hz), so the blink runs at the same speed on any monitor.
            int hz = 60;
            SDL_DisplayMode mode;
            SDL_Window* win = SDL_RenderGetWindow(sdl);
            if (win && SDL_GetWindowDisplayMode(win, &mode) == 0 && mode.refresh_rate > 0)
                hz = mode.refresh_rate;
            capture_anim_fpp_ = std::max(1, (CAPTURE_PHASE_MS * hz + 500) / 1000);
        }
    }
    if (!ds.tetris_captures) capture_anim_.clear();

    for (int r = 0; r < n; r++)
        memcpy(anim_prev_[r], board[r], (size_t)n);
    anim_prev_n_ = n;
}

int Renderer::capture_anim_wait_ms() const {
    // Frame-counted, so redraw every frame; the vsync'd present paces the loop.
    return capture_anim_.empty() ? -1 : 0;
}

void Renderer::render_capture_anim(const BoardView& view, const DrawState& ds, bool visible) {
    if (capture_anim_.empty()) return;
    int phase = capture_anim_frame_ / capture_anim_fpp_;
    if (phase >= CAPTURE_PHASES) { capture_anim_.clear(); return; }
    // Counts every presented frame, shown or not, so a help/catalog overlay
    // opened mid-blink doesn't leave it paused underneath.
    if (!ds.suppress_present) capture_anim_frame_++;
    if (!visible) return;
    bool flash = phase % 2 == 0;

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    for (const auto& c : capture_anim_) {
        if (ds.stone_filter == 1 && !c.is_black) continue;
        if (ds.stone_filter == 2 &&  c.is_black) continue;
        if (flash) {
            // The stone's own silhouette, filled flat and unshaded — losing the
            // shading is what makes white stones visibly blink too. Centre and
            // radius mirror draw_stone_circle so the flash covers the stone exactly.
            int cx     = view.offset_x + c.f * view.square + view.square / 2;
            int cy     = view.offset_y + c.r * view.square + view.square / 2;
            int radius = std::max(2, view.square / 2 - (octagon_stones_ ? 0 : 2));
            SDL_SetRenderDrawColor(sdl, 250, 250, 236, 255);
            if (octagon_stones_) {
                fill_octagon(cx, cy, radius);
            } else if (square_stones_) {
                SDL_Rect body = {cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1};
                SDL_RenderFillRect(sdl, &body);
            } else {
                fill_circle(cx, cy, radius);
            }
        } else {
            draw_stone_circle(view, c.r, c.f, c.is_black, 255, /*shadow_pass=*/false);
        }
    }
}

// ---------------------------------------------------------------------------
// Board cache hash — covers everything that affects the visual output
// except the cursor position (cursor_x/y/type), which is composited on top
// each frame without touching the cache.

uint64_t Renderer::compute_cache_hash(const DrawState& ds) const {
    // FNV-1a 64-bit
    uint64_t h = 14695981039346656037ULL;
    auto mix8  = [&](uint8_t  v) { h ^= v; h *= 1099511628211ULL; };
    auto mix64 = [&](uint64_t v) {
        for (int i = 0; i < 8; i++, v >>= 8) mix8(uint8_t(v));
    };
    auto mix_str = [&](const std::string& s) {
        for (char c : s) mix8(uint8_t(c));
        mix8(0);
    };

    // Active board array
    const char (*board)[MAX_BOARD_SIZE] =
        ds.territory_board ? ds.territory_board :
        (ds.analysis_mode && ds.analysis ? ds.analysis->board : ds.game.board);
    for (int r = 0; r < ds.active_board_size; r++)
        for (int f = 0; f < ds.active_board_size; f++)
            mix8(uint8_t(board[r][f]));

    // Stone filter
    mix8(uint8_t(ds.stone_filter));

    // Quit confirmation
    mix8(uint8_t(ds.show_move_numbers));
    mix64(uint64_t(ds.sgf_game_index));
    mix8(uint8_t(ds.quit_confirm));

    // Box selection is drawn outside the cache (like the cursor), so none of its
    // state goes into the hash — dragging never triggers a cache rebuild.

    // Mode flags
    mix8(uint8_t(ds.analysis_mode));
    mix8(uint8_t(ds.game_mode));
    mix8(uint8_t(ds.guess_mode));
    mix8(uint8_t(ds.chain_mode));
    mix8(uint8_t(ds.free_mode));
    mix8(uint8_t(ds.square_stones));
    mix8(uint8_t(ds.octagon_stones));
    mix8(uint8_t(ds.square_grid));
    mix8(uint8_t(ds.puzzle_mode));
    mix64(uint64_t(ds.active_board_size));
    mix8(uint8_t(ds.show_help));
    mix8(uint8_t(ds.territory_drill));
    mix64(uint64_t(ds.guess_score));
    mix64(uint64_t(ds.guess_count));
    mix64(uint64_t(ds.guess_last_points + 1));   // -1 ("no guess yet") must still mix
    mix64(uint64_t(ds.guess_last_dist   + 1));

    // Turn / liberty overlay
    if (ds.analysis) {
        mix8(uint8_t(ds.analysis->turn_is_black));
        mix64(uint64_t(ds.analysis->liberty_count));
        mix64(uint64_t(ds.analysis->liberty_display_r));
        mix64(uint64_t(ds.analysis->liberty_display_f));
        mix64(uint64_t(ds.analysis->selected_group_count));
    }
    mix64(uint64_t(ds.game.liberty_count));
    mix8(uint8_t(ds.game.game_finished));

    // Catalog (only need to track when active)
    mix8(uint8_t(ds.catalog.active));
    if (ds.catalog.active) {
        mix64(uint64_t(ds.catalog.index));
        mix64(uint64_t(ds.catalog.scroll));
        mix8(uint8_t(ds.catalog_thumb_valid));
        mix8(uint8_t(ds.catalog.search_mode));
        mix8(uint8_t(ds.catalog.game_index.loaded()));
        mix8(uint8_t(ds.catalog.game_index.is_loading()));
        mix8(uint8_t(ds.catalog.virtual_year_mode));
        mix_str(ds.catalog.virtual_year);
        mix8(uint8_t(ds.catalog.virtual_player_mode));
        mix_str(ds.catalog.virtual_player);
        mix_str(ds.catalog.current_subdir);
        mix_str(ds.catalog.search_query);
        mix64(uint64_t(ds.catalog.entries.size()));
        mix8(uint8_t(ds.catalog_readonly));
    }

    // HUD text
    mix_str(ds.black_name);
    mix_str(ds.white_name);
    mix_str(ds.result_message);
    mix_str(ds.game_date);
    mix_str(ds.game_comment);

    // Speed message: hash whether it is currently visible (and the delay value)
    Uint32 now = SDL_GetTicks();
    bool speed_on = ds.speed_message_until > 0 && now < ds.speed_message_until;
    mix8(uint8_t(speed_on));
    if (speed_on) mix64(uint64_t(ds.move_delay_ms));

    // Flash messages are deliberately absent: they're drawn after the cached texture
    // is blitted, so they can't affect it. Hashing them here used to rebuild the
    // entire board cache every time a notification appeared or expired.

    // Save input overlay
    mix8(uint8_t(ds.save_input_step));
    if (ds.save_input_step) mix_str(ds.save_input_buf);

    // Territory drill answer state
    if (ds.territory_drill) {
        mix64(uint64_t(ds.territory_b_score));
        mix64(uint64_t(ds.territory_w_score));
        mix8(uint8_t(ds.territory_answered));
        mix8(uint8_t(ds.territory_correct));
    }

    // Live game (clocks + status; cursor is drawn outside the cache)
    mix8(uint8_t(ds.live_mode));
    if (ds.live_mode) {
        mix64(uint64_t(ds.live_black_secs));
        mix64(uint64_t(ds.live_white_secs));
        mix64(uint64_t(ds.live_black_periods));
        mix64(uint64_t(ds.live_white_periods));
        mix8(uint8_t(ds.live_my_turn));
        mix8(uint8_t(ds.live_my_color));
        if (ds.live_status) mix_str(ds.live_status);
        if (ds.live_result_banner) mix_str(ds.live_result_banner);
        // Stone removal overlay — must be hashed or cache won't rebuild when data arrives
        int n = ds.active_board_size;
        if (ds.live_dead_stones) {
            for (int r = 0; r < n; r++)
                for (int f = 0; f < n; f++)
                    mix8(uint8_t(ds.live_dead_stones[r][f]));
        }
        if (ds.live_ownership) {
            for (int r = 0; r < n; r++)
                for (int f = 0; f < n; f++)
                    mix8(uint8_t(ds.live_ownership[r][f]));
        }
        mix8(uint8_t(ds.live_ownership_fill));
        mix64(uint64_t(ds.live_area_black));
        mix64(uint64_t(ds.live_area_white));
        for (int i = 0; i < ds.live_suggestion_count; i++) {
            mix8(uint8_t(ds.live_suggestions[i].row));
            mix8(uint8_t(ds.live_suggestions[i].col));
            mix8(uint8_t(int(ds.live_suggestions[i].score_lead * 10)));
        }
        mix8(uint8_t(ds.live_hovered_suggestion + 1));  // invalidate cache when hover changes
        if (ds.live_kata_score_lead != FLT_MAX)
            mix64(uint64_t(int(ds.live_kata_score_lead * 10)));
        if (ds.live_actual_move_r >= 0) {
            mix8(uint8_t(ds.live_actual_move_r));
            mix8(uint8_t(ds.live_actual_move_f));
            if (ds.live_actual_move_score != FLT_MAX)
                mix64(uint64_t(int(ds.live_actual_move_score * 10)));
        }
        // Letter labels (analysis annotations)
        mix64(uint64_t(ds.live_label_count));
        for (int i = 0; i < ds.live_label_count; i++) {
            mix8(uint8_t(ds.live_labels[i].r));
            mix8(uint8_t(ds.live_labels[i].f));
            mix8(uint8_t(ds.live_labels[i].ch));
        }
        // Analysis tree (topology + current position)
        mix64(uint64_t(ds.live_analysis_tree_count));
        mix64(uint64_t(ds.live_analysis_tree_cur_depth));
        for (int i = 0; i < ds.live_analysis_tree_count; i++) {
            const auto& tn = ds.live_analysis_tree[i];
            mix8(uint8_t(tn.col));
            mix8(uint8_t(tn.current));
            mix8(uint8_t(tn.marked));
            mix8(uint8_t(tn.goal));
        }
        // Score graph
        if (ds.live_score_graph) {
            mix64(uint64_t(ds.live_score_graph_len));
            mix64(uint64_t(ds.live_score_graph_cur));
            for (int i = 0; i < ds.live_score_graph_len; i++) {
                float v = ds.live_score_graph[i];
                mix64(v == FLT_MAX ? UINT64_MAX
                                   : uint64_t((int)(v * 10.f + 0.5f) + 50000));
            }
        }
        mix8(uint8_t(ds.live_last_move_r + 1));
        mix8(uint8_t(ds.live_last_move_f + 1));
    }

    return h;
}

// Render all cast shadows (stones + chain bars) into a temporary texture using
// max-alpha blending, then composite once onto the current render target.
// This prevents shadows from stacking darker at intersections.
void Renderer::render_all_shadows(const BoardView& view,
                                   const char board[][MAX_BOARD_SIZE],
                                   bool chain_mode, int stone_filter, int n) {
    int w, h;
    SDL_GetRendererOutputSize(sdl, &w, &h);

    SDL_Texture* prev_target = SDL_GetRenderTarget(sdl);

    SDL_Texture* shadow_tex = SDL_CreateTexture(sdl, SDL_PIXELFORMAT_ARGB8888,
                                                SDL_TEXTUREACCESS_TARGET, w, h);
    if (!shadow_tex) return;
    SDL_SetTextureBlendMode(shadow_tex, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(sdl, shadow_tex);
    SDL_SetRenderDrawColor(sdl, 0, 0, 0, 0);
    SDL_RenderClear(sdl);

    // Max-alpha blend: overlapping shadows take the maximum darkness, not the sum.
    SDL_BlendMode max_blend = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_MAXIMUM,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_MAXIMUM);
    SDL_SetRenderDrawBlendMode(sdl, max_blend);

    // Stone shadows
    for (int r = 0; r < n; r++)
        for (int f = 0; f < n; f++) {
            int cell = board[r][f];
            if (cell == 0) continue;
            int is_black = (cell == 1);
            if (stone_filter == 1 && !is_black) continue;
            if (stone_filter == 2 &&  is_black) continue;
            draw_stone_circle(view, r, f, is_black, 255, /*shadow_pass=*/true);
        }

    // Link shadows (blend mode already set; draw_stone_link shadow pass won't override it)
    render_chain_connections(view, board, chain_mode, stone_filter, /*shadows_only=*/true);

    // Composite shadow texture onto board with standard BLEND (darkens board by shadow alpha)
    SDL_SetRenderTarget(sdl, prev_target);
    SDL_RenderCopy(sdl, shadow_tex, nullptr, nullptr);
    SDL_DestroyTexture(shadow_tex);
}

// Draws board+HUD to whatever render target is currently active.
// Does NOT draw the software cursor or call SDL_RenderPresent.
void Renderer::render_board_content(const BoardView& view, const Overlay* overlay, const DrawState& ds) {
    SDL_SetRenderDrawColor(sdl, Palette::BACKGROUND.r, Palette::BACKGROUND.g, Palette::BACKGROUND.b, 255);
    SDL_RenderClear(sdl);

    // Board background
    int bg = view.board_px + 2 * view.margin;
    SDL_Rect board_rect = {view.offset_x - view.margin, view.offset_y - view.margin, bg, bg};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
    SDL_RenderFillRect(sdl, &board_rect);

    // Grid lines
    SDL_Color grid_color = Palette::GRID;
    int n          = view.active_size;
    int normal_t   = (view.square >= 30) ? 2 : 1;
    int boundary_t = normal_t * 2;

    if (ds.square_grid) {
        // Alternate layout: point (r,f) sits at the centre of its own square
        // cell instead of at a crossing of grid lines. The pixel formula for
        // a point's centre — offset + coord*square + square/2 — is exactly
        // the same as the crossing-line layout below, so every stone/marker/
        // cursor draw call elsewhere needs no changes at all; only the board
        // structure underneath differs, drawn here. Cells stay the same flat
        // Palette::BOARD colour already filled in above — just the hairline
        // separators between them.
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sdl, grid_color.r, grid_color.g, grid_color.b, 255);
        for (int i = 0; i <= n; i++) {
            int y = view.offset_y + i * view.square;
            SDL_RenderDrawLine(sdl, view.offset_x, y, view.offset_x + view.board_px, y);
            int x = view.offset_x + i * view.square;
            SDL_RenderDrawLine(sdl, x, view.offset_y, x, view.offset_y + view.board_px);
        }
        // Thicker outer boundary, matching the crossing-line layout's weight
        draw_thick_line(view.offset_x, view.offset_y,
                        view.offset_x + view.board_px, view.offset_y, boundary_t, grid_color);
        draw_thick_line(view.offset_x, view.offset_y + view.board_px,
                        view.offset_x + view.board_px, view.offset_y + view.board_px, boundary_t, grid_color);
        draw_thick_line(view.offset_x, view.offset_y,
                        view.offset_x, view.offset_y + view.board_px, boundary_t, grid_color);
        draw_thick_line(view.offset_x + view.board_px, view.offset_y,
                        view.offset_x + view.board_px, view.offset_y + view.board_px, boundary_t, grid_color);
    } else {
        int boundary_idx[2] = {0, n - 1};
        for (int bi = 0; bi < 2; bi++) {
            int i = boundary_idx[bi];
            int y = view.offset_y + i * view.square + view.square / 2;
            draw_thick_line(view.offset_x + boundary_t/2, y,
                            view.offset_x + view.board_px - boundary_t/2, y,
                            boundary_t, grid_color);
            int x = view.offset_x + i * view.square + view.square / 2;
            draw_thick_line(x, view.offset_y + boundary_t/2,
                            x, view.offset_y + view.board_px - boundary_t/2,
                            boundary_t, grid_color);
        }
        for (int i = 1; i < n - 1; i++) {
            int y  = view.offset_y + i * view.square + view.square / 2;
            int x0 = view.offset_x + view.square / 2;
            int x1 = view.offset_x + (n - 1) * view.square + view.square / 2;
            draw_thick_line(x0, y, x1, y, normal_t, grid_color);
            int x  = view.offset_x + i * view.square + view.square / 2;
            int y0 = view.offset_y + view.square / 2;
            int y1 = view.offset_y + (n - 1) * view.square + view.square / 2;
            draw_thick_line(x, y0, x, y1, normal_t, grid_color);
        }
    }

    // Star points (hoshi) — computed for the active board size
    {
        int star_r = (view.square >= 30) ? 4 : 3;
        SDL_SetRenderDrawColor(sdl, 0, 0, 0, 255);
        // offset from edge (0-indexed); also used for mid-edge and tengen
        int ho   = (n >= 13) ? 3 : 2;
        int ctr  = n / 2;
        // Build the set of hoshi positions
        int pts[9][2]; int npts = 0;
        if (n >= 7) {
            // Four corner hoshi
            pts[npts][0] = ho;      pts[npts][1] = ho;      npts++;
            pts[npts][0] = ho;      pts[npts][1] = n-1-ho;  npts++;
            pts[npts][0] = n-1-ho;  pts[npts][1] = ho;      npts++;
            pts[npts][0] = n-1-ho;  pts[npts][1] = n-1-ho;  npts++;
            // Tengen (centre) if n is odd
            if (n % 2 == 1) { pts[npts][0] = ctr; pts[npts][1] = ctr; npts++; }
        }
        // For 19×19 and 13×13: mid-edge hoshi
        if (n == 19) {
            pts[npts][0] = ho;  pts[npts][1] = ctr;  npts++;
            pts[npts][0] = n-1-ho; pts[npts][1] = ctr; npts++;
            pts[npts][0] = ctr; pts[npts][1] = ho;   npts++;
            pts[npts][0] = ctr; pts[npts][1] = n-1-ho; npts++;
        }
        for (int i = 0; i < npts; i++) {
            int x = view.offset_x + pts[i][1] * view.square + view.square / 2;
            int y = view.offset_y + pts[i][0] * view.square + view.square / 2;
            SDL_Rect sr = {x - star_r, y - star_r, star_r * 2, star_r * 2};
            SDL_RenderFillRect(sdl, &sr);
        }
    }

    // Choose board array and liberty state depending on mode
    const char (*active_board)[MAX_BOARD_SIZE] =
        ds.territory_board ? ds.territory_board :
        (ds.analysis_mode && ds.analysis ? ds.analysis->board : ds.game.board);
    const int* lib_r   = ds.analysis_mode && ds.analysis ? ds.analysis->liberty_r  : ds.game.liberty_r;
    const int* lib_f   = ds.analysis_mode && ds.analysis ? ds.analysis->liberty_f  : ds.game.liberty_f;
    int lib_count      = (ds.territory_drill || !(ds.analysis_mode && ds.analysis))
                         ? (ds.territory_drill ? 0 : ds.game.liberty_count)
                         : ds.analysis->liberty_count;

    // Chinese-style fill view (STONE_REMOVAL, Y/Triangle toggle): instead of marking
    // territory with small squares, redraw the board with every empty point AND every
    // dead-stone point turned into a full stone of the owning color — so the final
    // position reads like a completely filled Chinese-rules board.
    char fill_buf[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
    bool fill_view = ds.live_ownership_fill && ds.live_ownership;
    if (fill_view) {
        for (int r = 0; r < n; r++)
            for (int f = 0; f < n; f++) {
                bool dead = ds.live_dead_stones && ds.live_dead_stones[r][f];
                char cell = active_board[r][f];
                if (cell != 0 && !dead) { fill_buf[r][f] = cell; continue; }  // living stone
                int ow = ds.live_ownership[r][f];
                fill_buf[r][f] = (ow > 0) ? 1 : (ow < 0) ? 2 : 0;  // owner's stone, or empty if neutral
            }
    }
    const char (*display_board)[MAX_BOARD_SIZE] = fill_view ? fill_buf : active_board;

    // Layered render:
    //   1. All shadows (max-alpha composite — no stacking)
    //   2. Stone fills
    //   3. Link cylinders with elliptical caps on top (bars drawn over stones,
    //      shortened to stone edge so caps give cylinder-meets-sphere illusion)
    render_all_shadows(view, display_board, ds.chain_mode, ds.stone_filter, n);
    for (int r = 0; r < n; r++)
        for (int f = 0; f < n; f++) {
            int cell = display_board[r][f];
            if (cell == 0) continue;
            int is_black = (cell == 1);
            if (ds.stone_filter == 1 && !is_black) continue;
            if (ds.stone_filter == 2 &&  is_black) continue;
            draw_stone_circle(view, r, f, is_black, 255, /*shadow_pass=*/false);
        }
    render_chain_connections(view, display_board, ds.chain_mode, ds.stone_filter, /*shadows_only=*/false);

    // Stone-removal overlay: grey out dead stones, mark territory. Skipped entirely
    // in fill_view — the filled stones above already show ownership.
    if (!fill_view && (ds.live_dead_stones || ds.live_ownership)) {
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        int sq    = view.square;
        int sq2   = sq / 2;
        int tmark = std::max(3, sq / 5);  // territory square half-size

        for (int r = 0; r < n; r++) {
            for (int f = 0; f < n; f++) {
                int cx = view.offset_x + f * sq + sq2;
                int cy = view.offset_y + r * sq + sq2;

                // Territory squares: empty cells AND dead-stone cells.
                // Dead stones are removed for scoring, so their cells count as the
                // capturing player's territory.  Draw the square first so the X marker
                // for dead stones is rendered on top of it.
                if (ds.live_ownership) {
                    bool show_sq = (active_board[r][f] == 0) ||
                                   (ds.live_dead_stones && ds.live_dead_stones[r][f]);
                    if (show_sq) {
                        int ow = ds.live_ownership[r][f];
                        if (ow != 0) {
                            SDL_Rect sq_rect = {cx - tmark, cy - tmark, tmark*2, tmark*2};
                            if (ow > 0)  // black territory
                                SDL_SetRenderDrawColor(sdl, 30, 30, 30, 220);
                            else         // white territory
                                SDL_SetRenderDrawColor(sdl, 220, 220, 220, 220);
                            SDL_RenderFillRect(sdl, &sq_rect);
                            // Outline
                            SDL_SetRenderDrawColor(sdl, 0, 0, 0, 180);
                            SDL_RenderDrawRect(sdl, &sq_rect);
                        }
                    }
                }

                // Dead stone: thick X in contrasting colour, drawn on top of territory square
                if (ds.live_dead_stones && ds.live_dead_stones[r][f] && active_board[r][f] != 0) {
                    bool dead_is_black = (active_board[r][f] == 1);
                    int arm   = sq * 2 / 5;
                    int gap   = std::max(2, sq / 9);
                    int thick = std::max(2, sq / 8);
                    // White X over black stones, black X over white stones
                    if (dead_is_black)
                        SDL_SetRenderDrawColor(sdl, 255, 255, 255, 230);
                    else
                        SDL_SetRenderDrawColor(sdl, 20, 20, 20, 230);
                    for (int d = -thick/2; d <= thick/2; d++) {
                        SDL_RenderDrawLine(sdl, cx-gap-d, cy-gap+d, cx-arm-d, cy-arm+d);
                        SDL_RenderDrawLine(sdl, cx+gap+d, cy-gap+d, cx+arm+d, cy-arm+d);
                        SDL_RenderDrawLine(sdl, cx-gap-d, cy+gap-d, cx-arm-d, cy+arm-d);
                        SDL_RenderDrawLine(sdl, cx+gap+d, cy+gap-d, cx+arm+d, cy+arm-d);
                    }
                }
            }
        }
    }

    // Last-played-stone marker: teal ring, shown only while RT is held (see
    // App::make_draw_state — deliberately opt-in rather than always-on, since a
    // permanent row/column highlight was found too distracting during live play).
    if (ds.live_last_move_r >= 0 && ds.live_last_move_f >= 0) {
        int sq   = view.square;
        int half = sq / 2;
        int rad  = std::max(4, sq * 3 / 8);
        int lr   = ds.live_last_move_r;
        int lf   = ds.live_last_move_f;
        if (lr < n && lf < n) {
            int cx = view.offset_x + lf * sq + half;
            int cy = view.offset_y + lr * sq + half;
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
            // Thin dark fringe (1 px) so the ring reads against any background
            SDL_SetRenderDrawColor(sdl, 0, 0, 0, 130);
            fill_ring(cx, cy, rad + 6, rad + 5);
            // Teal ring (5 px thick) — centre left transparent so the stone still reads
            SDL_SetRenderDrawColor(sdl, 0, 210, 190, 255);
            fill_ring(cx, cy, rad + 5, rad);
        }
    }

    // Actual game move marker: yellow filled circle (larger than suggestions),
    // drawn first so suggestion circles appear on top and form a visible ring.
    // Hidden while hovering a suggestion (PV line is shown instead), and off
    // unless SHOW NEXT MOVE is ticked (see the caller filling these fields) —
    // it spoils the position for anyone reading ahead on their own.
    if (ds.live_actual_move_r >= 0 && ds.live_actual_move_f >= 0 &&
        ds.live_hovered_suggestion < 0) {
        int sq   = view.square;
        int half = sq / 2;
        int rad  = std::max(4, sq * 3 / 8);
        int ar   = ds.live_actual_move_r;
        int af   = ds.live_actual_move_f;
        if (ar < n && af < n) {
            int cx = view.offset_x + af * sq + half;
            int cy = view.offset_y + ar * sq + half;
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
            // Thin dark fringe (1 px) so the ring reads against any background
            SDL_SetRenderDrawColor(sdl, 0, 0, 0, 130);
            fill_ring(cx, cy, rad + 6, rad + 5);
            // Yellow ring (5 px thick) — centre left transparent
            SDL_SetRenderDrawColor(sdl, Palette::ACCENT.r, Palette::ACCENT.g, Palette::ACCENT.b, 230);
            fill_ring(cx, cy, rad + 5, rad);
            if (ds.live_actual_move_score != FLT_MAX && rad >= 10) {
                float lead = ds.live_actual_move_score;
                char buf[8];
                if (std::abs(lead) < 10.f)
                    snprintf(buf, sizeof(buf), "%.1f", lead);
                else
                    snprintf(buf, sizeof(buf), "%d", (int)(lead + (lead >= 0 ? 0.5f : -0.5f)));
                int tw = text_width_px(buf, 2);
                draw_text(cx - tw / 2, cy - 7, 2, buf, SDL_Color{255, 255, 255, 255});
            }
        }
    }

    // KataGo move suggestions (drawn during history review in GAME_OVER)
    if (ds.live_suggestions && ds.live_suggestion_count > 0) {
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        int sq   = view.square;
        int half = sq / 2;
        int rad  = std::max(4, sq * 3 / 8);

        for (int i = 0; i < std::min(ds.live_suggestion_count, 3); i++) {
            if (ds.live_hovered_suggestion >= 0 && i != ds.live_hovered_suggestion) continue;
            const MoveSuggestion& s = ds.live_suggestions[i];
            if (s.row < 0 || s.col < 0 || s.row >= n || s.col >= n) continue;
            if (active_board[s.row][s.col] != 0) continue;  // stone already there

            int cx = view.offset_x + s.col * sq + half;
            int cy = view.offset_y + s.row * sq + half;

            if (i == ds.live_hovered_suggestion) {
                // Hovered: show as the player's actual stone, numbered 1
                bool is_black = (ds.game.turn_is_black == 1);
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, 200);
                fill_circle(cx, cy, rad + 1);
                SDL_SetRenderDrawColor(sdl, is_black ? 30 : 215, is_black ? 30 : 215, is_black ? 30 : 215, 230);
                fill_circle(cx, cy, rad);
                {
                    int sc = 1;
                    if (rad >= 14) sc = 2;
                    if (rad >= 20) sc = 3;
                    int tw = text_width_px("1", sc), th = 7 * sc;
                    SDL_Color tc = is_black ? SDL_Color{230,230,230,255} : SDL_Color{40,40,40,255};
                    draw_text(cx - tw/2, cy - th/2, sc, "1", tc);
                }
            } else {
                // Brightness fades with rank: best = 210, each step -25
                Uint8 alpha = (Uint8)std::max(80, 210 - i * 25);
                Uint8 green = (Uint8)std::max(130, 210 - i * 15);

                SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.7f));
                fill_circle(cx, cy, rad + 1);
                SDL_SetRenderDrawColor(sdl, 10, green, 150, alpha);
                fill_circle(cx, cy, rad);

                // Score lead label (points ahead for the player to move)
                if (rad >= 10) {
                    char buf[8];
                    float lead = s.score_lead;
                    if (std::abs(lead) < 10.f)
                        snprintf(buf, sizeof(buf), "%.1f", lead);
                    else
                        snprintf(buf, sizeof(buf), "%d", (int)(lead + (lead >= 0 ? 0.5f : -0.5f)));
                    int tw = text_width_px(buf, 2);
                    draw_text(cx - tw / 2, cy - 7, 2, buf, SDL_Color{240, 240, 240, 255});
                }
            }
        }

        // PV overlay for the hovered suggestion (stones 2..N)
        if (ds.live_hovered_suggestion >= 0 &&
            ds.live_hovered_suggestion < ds.live_suggestion_count) {
            const MoveSuggestion& hs = ds.live_suggestions[ds.live_hovered_suggestion];
            bool black_plays_first = (ds.game.turn_is_black == 1);
            for (int pv = 0; pv < hs.pv_count; pv++) {
                int pr = hs.pv_row[pv];
                int pf = hs.pv_col[pv];
                if (pr < 0 || pr >= n || pf < 0 || pf >= n) continue;
                int pcx = view.offset_x + pf * sq + half;
                int pcy = view.offset_y + pr * sq + half;
                // pv[0] = opponent's response, pv[1] = suggestion player again, etc.
                bool is_black = (pv % 2 == 0) ? !black_plays_first : black_plays_first;
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, 180);
                fill_circle(pcx, pcy, rad + 1);
                SDL_SetRenderDrawColor(sdl, is_black ? 30 : 210, is_black ? 30 : 210, is_black ? 30 : 210, 220);
                fill_circle(pcx, pcy, rad);
                char pbuf[4];
                snprintf(pbuf, sizeof(pbuf), "%d", pv + 2);  // hovered stone = 1, so PV starts at 2
                int ndigits = (int)strlen(pbuf);
                int sc = 1;
                if (rad >= 14 && ndigits <= 2) sc = 2;
                if (rad >= 20 && ndigits == 1)  sc = 3;
                int ptw = text_width_px(pbuf, sc), pth = 7 * sc;
                SDL_Color nc = is_black ? SDL_Color{230,230,230,255} : SDL_Color{40,40,40,255};
                draw_text(pcx - ptw/2, pcy - pth/2, sc, pbuf, nc);
            }
        }
    }

    // Move-number overlay
    if (ds.show_move_numbers) {
        // Build num/col grids from the appropriate source:
        //   analysis mode → App's persistent grids (never touched by captures)
        //   playback      → raw SGF arrays (position-based, also capture-immune)
        int num_grid[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
        int col_grid[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};

        if (ds.analysis_mode && ds.analysis_num_grid && ds.analysis_col_grid) {
            for (int r = 0; r < n; r++)
                for (int f = 0; f < n; f++) {
                    num_grid[r][f] = ds.analysis_num_grid[r][f];
                    col_grid[r][f] = ds.analysis_col_grid[r][f];
                }
        } else if (!ds.analysis_mode && ds.sgf_moves && ds.sgf_colors) {
            for (int i = 0; i < ds.sgf_game_index; i++) {
                const char* mv = ds.sgf_moves[i];
                if (strlen(mv) == 2
                    && mv[0] >= 'a' && mv[0] <= 's'
                    && mv[1] >= 'a' && mv[1] <= 's') {
                    int f = mv[0] - 'a';
                    int r = mv[1] - 'a';
                    num_grid[r][f] = i + 1;
                    col_grid[r][f] = ds.sgf_colors[i];
                }
            }
        }

        int half   = view.square / 2;
        int radius = view.square / 2 - 2;
        for (int r = 0; r < n; r++) {
            for (int f = 0; f < n; f++) {
                if (num_grid[r][f] == 0) continue;
                int  num      = num_grid[r][f];
                int  is_black = col_grid[r][f];
                bool captured = (active_board[r][f] == 0);
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", num);
                int ndigits = (int)strlen(buf);
                int scale   = 1;
                if (radius >= 14 && ndigits <= 2) scale = 2;
                if (radius >= 20 && ndigits == 1) scale = 3;
                int tw = text_width_px(buf, scale);
                int th = 7 * scale;
                int cx = view.offset_x + f * view.square + half;
                int cy = view.offset_y + r * view.square + half;
                int tx = cx - tw / 2;
                int ty = cy - th / 2;
                if (captured) {
                    // Faint ghost circle so the number reads at an empty intersection
                    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
                    Uint8 bv = is_black ? 30 : 200;
                    SDL_SetRenderDrawColor(sdl, bv, bv, bv, 110);
                    fill_circle(cx, cy, radius * 3 / 4);
                }
                SDL_Color tc = captured
                    ? Palette::ACCENT
                    : (is_black ? SDL_Color{230, 230, 230, 255}
                                : SDL_Color{40,  40,  40,  255});
                draw_text(tx, ty, scale, buf, tc);
            }
        }
    }

    // Liberty dots
    render_liberties(view, lib_r, lib_f, lib_count);

    // Stone preview overlay
    if (overlay && overlay->active) {
        int r = -1, f = -1;
        if (screen_to_board(view, (int)overlay->x, (int)overlay->y, r, f))
            draw_stone_circle(view, r, f, overlay->is_black, 128);
    }

    if (!ds.territory_drill && !ds.free_mode) {
        render_game_comment(view, ds);
        render_player_labels(view, ds);
        if (!ds.live_mode) {
            render_speed_label(view, ds.move_delay_ms, ds.speed_message_until);
            render_result_message(view, ds);
            render_game_date(view, ds.game_date);
        }
        render_guess_score(view, ds);
    }
    if (!ds.free_mode) {
        if (ds.live_mode) {
            // Status line in the same top-left spot as mode status. Call sites
            // insert '\n' at logical, readable break points for long messages
            // (button-hint groups, "solved"/"wrong" text, etc.) rather than
            // relying on auto word-wrap, which tended to split awkwardly mid-hint
            // (e.g. between a button glyph and its label). Scale still shrinks
            // as a last resort for anything that overflows despite the breaks
            // (e.g. arbitrary server-provided text with no embedded '\n').
            if (ds.live_status && ds.live_status[0]) {
                std::string status_str(ds.live_status);

                int scale = (view.square >= 30) ? 3 : 2;
                int pad   = (view.square >= 30) ? 16 : 8;
                int avail = view.offset_x - view.margin - pad - 4;

                std::vector<std::string> lines;
                {
                    size_t start = 0;
                    while (start <= status_str.size()) {
                        size_t nl  = status_str.find('\n', start);
                        size_t end = (nl == std::string::npos) ? status_str.size() : nl;
                        lines.push_back(status_str.substr(start, end - start));
                        if (nl == std::string::npos) break;
                        start = nl + 1;
                    }
                }
                auto widest = [&](int s) {
                    int w = 0;
                    for (auto& l : lines) w = std::max(w, text_width_px(l.c_str(), s));
                    return w;
                };
                while (scale > 1 && widest(scale) > avail)
                    scale--;
                int y = view.offset_y - view.margin + pad;
                // Big result banner (local game end): the score in double scale, with
                // the normal-size status line ("PRESS X FOR ANALYSIS") beneath it.
                if (ds.live_result_banner && ds.live_result_banner[0]) {
                    int bscale = scale * 2;
                    while (bscale > scale && text_width_px(ds.live_result_banner, bscale) > avail)
                        bscale--;
                    int bw = text_width_px(ds.live_result_banner, bscale);
                    draw_text(view.offset_x - view.margin - pad - bw, y, bscale,
                              ds.live_result_banner, Palette::ACCENT);
                    y += bscale * 7 + 10;
                }
                SDL_Color col = ds.live_my_turn ? Palette::ACCENT : Palette::TEXT_SECONDARY;
                for (size_t i = 0; i < lines.size(); i++) {
                    int tw = text_width_px(lines[i].c_str(), scale);
                    int x  = view.offset_x - view.margin - pad - tw;
                    draw_text(x, y, scale, lines[i].c_str(), col);
                    if (i + 1 < lines.size()) y += scale * 8 + 3;
                }

                // Result and KataGo projected score below the status (GAME_OVER)
                int next_y = y + scale * 8 + 3;
                int lscale = scale;
                int lright = view.offset_x - view.margin - pad;

                if (!ds.result_message.empty()) {
                    char rbuf[48];
                    const char* rm = ds.result_message.c_str();
                    if ((rm[0] == 'B' || rm[0] == 'W') && rm[1] == '+') {
                        const char* who = (rm[0] == 'B') ? "BLACK" : "WHITE";
                        const char* margin = rm + 2;
                        if (margin[0] == 'R' || margin[0] == 'r') {
                            // who = winner; the *loser* resigned
                            const char* loser = (rm[0] == 'B') ? "WHITE" : "BLACK";
                            snprintf(rbuf, sizeof(rbuf), "%s RESIGNED", loser);
                        } else if (margin[0] == 'T' || margin[0] == 't')
                            snprintf(rbuf, sizeof(rbuf), "%s TIME", who);
                        else
                            snprintf(rbuf, sizeof(rbuf), "%s +%s", who, margin);
                    } else {
                        snprintf(rbuf, sizeof(rbuf), "%s", rm);
                    }
                    // "RESULT:" and the value go on separate lines; the value line
                    // (e.g. "BLACK RESIGNED") tends to overflow the right-aligned
                    // column when combined with the label on one line.
                    draw_text(lright - text_width_px("RESULT:", lscale), next_y, lscale, "RESULT:", Palette::ACCENT);
                    next_y += lscale * 8 + 3;
                    draw_text(lright - text_width_px(rbuf, lscale), next_y, lscale, rbuf, Palette::ACCENT);
                    next_y += lscale * 8 + 3;
                }
                if (ds.live_kata_score_lead != FLT_MAX) {
                    float sl = ds.live_kata_score_lead;
                    const char* who = sl >= 0.f ? "BLACK" : "WHITE";
                    char sbuf[48];
                    snprintf(sbuf, sizeof(sbuf), "PROJECTED: %s +%.1f", who, std::fabsf(sl));
                    draw_text(lright - text_width_px(sbuf, lscale), next_y, lscale, sbuf, Palette::PROJECTED);
                }
            }
        } else {
            render_mode_status(view, ds.analysis_mode, ds.game_mode, ds.guess_mode, ds.territory_drill, false);
        }
    }
    // Letter labels placed during analysis (circle button). On an empty point the
    // letter gets a board-colored backing disc so grid lines don't cross it; on a
    // stone it's drawn directly in the contrasting color.
    for (int i = 0; i < ds.live_label_count; i++) {
        const BoardLabel& l = ds.live_labels[i];
        if (l.r < 0 || l.r >= n || l.f < 0 || l.f >= n) continue;
        int cx = view.offset_x + l.f * view.square + view.square / 2;
        int cy = view.offset_y + l.r * view.square + view.square / 2;
        int cell = active_board[l.r][l.f];
        int scale = (view.square >= 40) ? 3 : 2;
        char buf[2] = { l.ch, '\0' };
        int tw = text_width_px(buf, scale), th = 7 * scale;
        SDL_Color txt;
        if (cell == 0) {
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
            fill_circle(cx, cy, view.square * 2 / 5);
            txt = Palette::ACCENT;  // yellow reads far better on the board than dark gray
        } else {
            txt = (cell == 1) ? SDL_Color{235, 235, 235, 255} : SDL_Color{30, 30, 30, 255};
        }
        draw_text(cx - tw / 2, cy - th / 2, scale, buf, txt);
    }

    render_territory_overlay(view, ds);
    render_who_won_overlay(view, ds);
    render_analysis_tree(view, ds);
    render_score_graph(view, ds);

    render_help_overlay(view, ds.show_help, ds.live_mode);
    render_catalog_overlay(view, ds);
    render_save_input(view, ds);
    // Flash notifications are NOT drawn here. They used to be, which put them inside
    // the cached board texture — so the board coordinates, drawn after the cache is
    // blitted, landed on top of them. render_board() draws them last instead.
    if (ds.quit_confirm) render_quit_confirm(view);
}

// Public entry point: uses a cached texture for the board+HUD so that
// cursor-only frames (mouse movement) are essentially free.
void Renderer::render_board(const BoardView& view, const Overlay* overlay, const DrawState& ds) {
    uint64_t h = compute_cache_hash(ds);

    if (h != cache_hash_) {
        // (Re)create the cache texture if the output size changed
        int out_w = 0, out_h = 0;
        SDL_GetRendererOutputSize(sdl, &out_w, &out_h);
        if (!board_cache_ || cache_w_ != out_w || cache_h_ != out_h) {
            if (board_cache_) SDL_DestroyTexture(board_cache_);
            board_cache_ = SDL_CreateTexture(sdl,
                SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                out_w, out_h);
            cache_w_ = out_w;
            cache_h_ = out_h;
        }
        SDL_SetRenderTarget(sdl, board_cache_);
        render_board_content(view, overlay, ds);
        SDL_SetRenderTarget(sdl, nullptr);
        cache_hash_ = h;
    }

    // Blit cached board, then draw drag-overlay + cursor on top (always, every frame,
    // without touching the cache — so dragging never triggers a cache rebuild)
    SDL_RenderCopy(sdl, board_cache_, nullptr, nullptr);

    // Full-screen overlays (catalog, help) live INSIDE the cache; everything drawn
    // after the blit would land on top of them. Skip the board-level decorations
    // (dim, box selection, live cursor) while one of those overlays is up.
    bool board_on_screen = !ds.catalog.active && !ds.show_help;

    if (board_on_screen) render_box_selection(view, ds);

    // Bullseye rings under the aiming point — below the cursor indicators drawn
    // further down, so the cursor always stays readable on top of them.
    if (board_on_screen) render_guess_heatmap(view, ds);

    // Captured stones blinking out — over the stones, under every marker/cursor.
    render_capture_anim(view, ds, board_on_screen);

    // Colored point markers (joseki continuation dots) — filled circle with a
    // dark rim so every category color reads against the board background
    if (board_on_screen && ds.live_markers && ds.live_marker_count > 0) {
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        int rad = std::max(3, view.square * 3 / 10);
        for (int i = 0; i < ds.live_marker_count; i++) {
            const PointMarker& m = ds.live_markers[i];
            int cx = view.offset_x + m.f * view.square + view.square / 2;
            int cy = view.offset_y + m.r * view.square + view.square / 2;
            SDL_SetRenderDrawColor(sdl, m.color.r, m.color.g, m.color.b,
                                   m.color.a ? m.color.a : 255);
            fill_circle(cx, cy, rad);
            SDL_SetRenderDrawColor(sdl, 20, 20, 20, 200);
            draw_circle(cx, cy, rad);
        }
    }

    // Live board cursor — yellow X drawn at the grid intersection (45° rotated, bold).
    if (board_on_screen && ds.live_mode && ds.live_cursor_r >= 0 && ds.live_cursor_f >= 0) {
        int cx = view.offset_x + ds.live_cursor_f * view.square + view.square / 2;
        int cy = view.offset_y + ds.live_cursor_r * view.square + view.square / 2;
        int arm = view.square * 2 / 5;
        int gap = std::max(2, view.square / 9);
        int thick = std::max(2, view.square / 10);  // boldness
        Uint8 alpha = ds.live_my_turn ? 230 : 130;
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        if (ds.live_cursor_ko)
            SDL_SetRenderDrawColor(sdl, 255, 60, 60, alpha);
        else
            SDL_SetRenderDrawColor(sdl, 255, 220, 0, alpha);
        // Draw each of the 4 diagonal arms with thickness via parallel offset lines
        for (int d = -thick/2; d <= thick/2; d++) {
            // top-left arm
            SDL_RenderDrawLine(sdl, cx - gap - d, cy - gap + d, cx - arm - d, cy - arm + d);
            // top-right arm
            SDL_RenderDrawLine(sdl, cx + gap + d, cy - gap + d, cx + arm + d, cy - arm + d);
            // bottom-left arm
            SDL_RenderDrawLine(sdl, cx - gap - d, cy + gap - d, cx - arm - d, cy + arm - d);
            // bottom-right arm
            SDL_RenderDrawLine(sdl, cx + gap + d, cy + gap - d, cx + arm + d, cy + arm - d);
        }
    }

    render_board_coordinates(view, ds);

    // Chalk sits above everything on the board — it's drawn on the glass, so it
    // ignores which position is showing — but below the popup, so a menu opened
    // over a scribbled board stays readable.
    if (annot_layer_ && annot_any_)
        SDL_RenderCopy(sdl, annot_layer_, nullptr, nullptr);
    // Drawing mode used to announce itself with a text label up here, but it shared
    // the top-left with the status line and the two overlapped. The cursor carries
    // it instead: in drawing mode it becomes a crosshair tinted with the armed chalk
    // colour (see cursor_color), which says both "a click draws" and "in this
    // colour" right where the eye already is, and can't collide with anything.

    if (ds.popup_items && ds.popup_count > 0)
        draw_popup_menu(ds.popup_title, ds.popup_items, ds.popup_count, ds.popup_index,
                        ds.popup_styles);
    // Notifications go on top of everything except the cursor — above the board
    // coordinates (which otherwise overdrew them), the chalk layer and the popup.
    // Drawing them outside the cached texture also means a flash appearing or
    // expiring no longer rebuilds the whole board cache.
    render_flash_message(view, ds);
    render_software_cursor(view, ds);
    if (!ds.suppress_present)
        SDL_RenderPresent(sdl);
}

// ---------------------------------------------------------------------------
// Board-edge coordinate labels (toggled by RT during live play)

void Renderer::render_board_coordinates(const BoardView& view, const DrawState& ds) {
    if (!ds.live_show_coords) return;
    // Only draw when the board is actually what's on screen — not behind a
    // full-screen overlay like the catalog or help screen.
    if (ds.catalog.active || ds.show_help) return;
    int n = ds.active_board_size;
    if (n <= 0) return;

    // Standard Go lettering skips 'I' (looks too much like '1').
    auto col_letter = [](int f) -> char {
        char c = (char)('A' + f);
        if (c >= 'I') c++;
        return c;
    };

    int sq    = view.square;
    int scale = std::max(1, sq / 16);
    int th    = 7 * scale;
    SDL_Color dim    = {150, 165, 180, 230};
    SDL_Color yellow = {255, 220, 0, 255};

    int cur_r = ds.live_cursor_r, cur_f = ds.live_cursor_f;

    // Top & bottom edges: column letters
    for (int f = 0; f < n; f++) {
        char buf[2] = { col_letter(f), '\0' };
        int  tw  = text_width_px(buf, scale);
        int  cx  = view.offset_x + f * sq + sq / 2;
        bool hi  = (f == cur_f && cur_r >= 0);
        SDL_Color col = hi ? yellow : dim;
        draw_text(cx - tw / 2, view.offset_y - view.margin + (view.margin - th) / 2, scale, buf, col);
        draw_text(cx - tw / 2, view.offset_y + n * sq   + (view.margin - th) / 2, scale, buf, col);
    }

    // Left & right edges: row numbers — size at the top, 1 at the bottom (Go convention)
    for (int r = 0; r < n; r++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", n - r);
        int  tw  = text_width_px(buf, scale);
        int  cy  = view.offset_y + r * sq + sq / 2;
        bool hi  = (r == cur_r && cur_f >= 0);
        SDL_Color col = hi ? yellow : dim;
        draw_text(view.offset_x - view.margin + (view.margin - tw) / 2, cy - th / 2, scale, buf, col);
        draw_text(view.offset_x + n * sq   + (view.margin - tw) / 2, cy - th / 2, scale, buf, col);
    }
}

// ---------------------------------------------------------------------------
// Software cursor (drawn directly in renderer — no OS scaling involved)

void Renderer::draw_stone_at_px(int cx, int cy, int radius, int is_black, Uint8 alpha) {
    shade_stone(cx, cy, radius, is_black, alpha);
}

// Bullseye heatmap: the scoring rings painted onto the board around the point the
// player is aiming at, so the value of a guess is visible before committing to it.
// Drawn outside the board cache with the other board decorations — it follows the
// aim every frame and must never trigger a cache rebuild.
void Renderer::render_guess_heatmap(const BoardView& view, const DrawState& ds) {
    int r = ds.guess_heat_r, f = ds.guess_heat_f;
    if (view.square <= 0) return;
    if (r < 0 || r >= view.active_size || f < 0 || f >= view.active_size) return;

    int sq = view.square;
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);

    // Outermost ring first so the inner, hotter shells paint over its edges.
    static const Uint8 ring_alpha[3] = {150, 105, 60};
    for (int d = 2; d >= 0; d--) {
        SDL_Color c = GUESS_RING_COLOR[d];
        SDL_SetRenderDrawColor(sdl, c.r, c.g, c.b, ring_alpha[d]);
        for (int rr = r - d; rr <= r + d; rr++)
            for (int ff = f - d; ff <= f + d; ff++) {
                // Only the shell at exactly this distance — inner cells belong to a
                // hotter ring and are painted by a later pass.
                if (guess_ring_dist(rr, ff, r, f) != d) continue;
                if (rr < 0 || rr >= view.active_size || ff < 0 || ff >= view.active_size)
                    continue;
                // 1px inset keeps the grid visible between cells, so the block reads
                // as discrete scoring points rather than one smeared blob.
                SDL_Rect cell = {view.offset_x + ff * sq + 1, view.offset_y + rr * sq + 1,
                                 sq - 2, sq - 2};
                SDL_RenderFillRect(sdl, &cell);
            }
    }

    // Bullseye outline — the one cell worth full marks, called out so the centre is
    // unmistakable once a stone or cursor is drawn over it.
    SDL_SetRenderDrawColor(sdl, 255, 255, 255, 220);
    SDL_Rect centre = {view.offset_x + f * sq + 1, view.offset_y + r * sq + 1, sq - 2, sq - 2};
    SDL_RenderDrawRect(sdl, &centre);
}

void Renderer::render_software_cursor(const BoardView& view, const DrawState& ds) {
    if (ds.cursor_type == 0 || ds.cursor_x < 0 || ds.cursor_y < 0) return;
    int cx = ds.cursor_x, cy = ds.cursor_y;
    int sq = view.square > 0 ? view.square : 32;

    // While aiming a guess the stone sits exactly on the bullseye rather than
    // floating at the raw pointer, so the cell that scores is never ambiguous.
    if (ds.cursor_snap_heat && ds.guess_heat_r >= 0 && ds.guess_heat_f >= 0
        && view.square > 0) {
        cx = view.offset_x + ds.guess_heat_f * view.square + view.square / 2;
        cy = view.offset_y + ds.guess_heat_r * view.square + view.square / 2;
    }

    if (ds.cursor_type >= 2) {
        // Stone cursor: filled circle with a yellow border ring
        int is_black = (ds.cursor_type == 3);
        int radius   = sq / 2 - 2;
        if (radius < 2) radius = 2;
        draw_stone_at_px(cx, cy, radius, is_black, 255);
    } else {
        // Crosshair cursor: accent (or the caller's tint) with a contrasting
        // shadow, centre gap, 3/4-length arms
        int arm = sq * 3 / 8;            // half-arm length from centre
        int gap = std::max(1, sq / 12);  // gap around the hotspot
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        SDL_Color tint = (ds.cursor_color.a == 0) ? Palette::ACCENT : ds.cursor_color;
        // The shadow exists so the cursor stays visible against whatever it's over.
        // A dark shadow does nothing for a dark cursor, so flip it to a light halo
        // when the tint itself is dark (the near-black chalk cursor).
        bool tint_is_dark = (tint.r * 299 + tint.g * 587 + tint.b * 114) / 1000 < 110;
        const struct { int ox, oy; Uint8 r, g, b, a; } passes[2] = {
            {1, 1, Uint8(tint_is_dark ? 255 : 0), Uint8(tint_is_dark ? 255 : 0),
                   Uint8(tint_is_dark ? 255 : 0), Uint8(tint_is_dark ? 170 : 140)},
            {0, 0, tint.r, tint.g, tint.b, 255},
        };
        for (auto& p : passes) {
            SDL_Color col = {p.r, p.g, p.b, p.a};
            // left arm
            draw_thick_line(cx+p.ox-arm, cy+p.oy, cx+p.ox-gap-1, cy+p.oy, 2, col);
            // right arm
            draw_thick_line(cx+p.ox+gap, cy+p.oy, cx+p.ox+arm,   cy+p.oy, 2, col);
            // top arm
            draw_thick_line(cx+p.ox, cy+p.oy-arm, cx+p.ox, cy+p.oy-gap-1, 2, col);
            // bottom arm
            draw_thick_line(cx+p.ox, cy+p.oy+gap, cx+p.ox, cy+p.oy+arm,   2, col);
        }
    }
}
