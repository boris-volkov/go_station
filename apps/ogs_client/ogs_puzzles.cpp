#include "ogs_puzzles.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <curl/curl.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

using json = nlohmann::json;

// ── HTTP GET → string (mirrors ogs_net.cpp's curl setup) ─────────────────────

static std::string puzzles_ca_bundle() {
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* last_sep = strrchr(exe, '\\');
    if (!last_sep) last_sep = strrchr(exe, '/');
    if (last_sep) {
        strcpy(last_sep + 1, "ca-bundle.crt");
        return exe;
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        std::string path(buf, n);
        auto slash = path.find_last_of('/');
        if (slash != std::string::npos)
            return path.substr(0, slash + 1) + "ca-bundle.crt";
    }
#endif
    return "ca-bundle.crt";
}

static size_t puzzles_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = reinterpret_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static bool http_get_json(const std::string& url, json& out) {
    std::string data;
    long code = 0;

    CURL* c = curl_easy_init();
    if (!c) return false;
    static std::string ca = puzzles_ca_bundle();
    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  puzzles_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &data);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO,         ca.c_str());
    CURLcode res = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || code != 200) return false;
    try {
        out = json::parse(data);
    } catch (...) {
        return false;
    }
    return true;
}

// ── Parsers ───────────────────────────────────────────────────────────────────

static void parse_move_tree(const json& j, PuzzleMoveNode& node) {
    node.x       = j.value("x", -1);
    node.y       = j.value("y", -1);
    node.correct = j.value("correct_answer", false);
    node.wrong   = j.value("wrong_answer",   false);
    node.text    = j.value("text", std::string());
    // Author annotations: goban serializes marks as [{x, y, marks:{letter|cross|
    // circle|triangle|square…}}]. Letters keep their first character; shape marks
    // are reduced to a character (triangle/square use the PS-glyph shapes our
    // bitmap font already has).
    if (j.contains("marks") && j["marks"].is_array()) {
        for (const auto& m : j["marks"]) {
            if (!m.is_object() || !m.contains("marks") || !m["marks"].is_object())
                continue;
            PuzzleMark pm;
            pm.x = m.value("x", -1);
            pm.y = m.value("y", -1);
            if (pm.x < 0 || pm.y < 0) continue;
            const auto& mk = m["marks"];
            // Mark values vary between bools and strings across goban versions
            auto truthy = [&mk](const char* key) {
                if (!mk.contains(key)) return false;
                const auto& v = mk[key];
                if (v.is_boolean()) return v.get<bool>();
                if (v.is_string())  return !v.get<std::string>().empty();
                return !v.is_null();
            };
            std::string letter;
            if (mk.contains("letter") && mk["letter"].is_string())
                letter = mk["letter"].get<std::string>();
            if (!letter.empty())
                pm.ch = (char)toupper((unsigned char)letter[0]);
            else if (truthy("cross"))    pm.ch = 'X';
            else if (truthy("circle"))   pm.ch = 'O';
            else if (truthy("triangle")) pm.ch = '^';   // triangle glyph
            else if (truthy("square"))   pm.ch = '#';   // square glyph
            else continue;
            node.marks.push_back(pm);
        }
    }
    if (j.contains("branches") && j["branches"].is_array()) {
        for (const auto& b : j["branches"]) {
            node.branches.emplace_back();
            parse_move_tree(b, node.branches.back());
        }
    }
}

bool ogs_fetch_puzzle(int puzzle_id, OgsPuzzle& out) {
    json j;
    if (!http_get_json("https://online-go.com/api/v1/puzzles/"
                       + std::to_string(puzzle_id), j))
        return false;
    if (!j.contains("puzzle") || !j["puzzle"].is_object()) return false;

    try {
        out = OgsPuzzle{};
        out.id   = j.value("id", puzzle_id);
        out.rank = j.value("rank", 0);
        const auto& p = j["puzzle"];
        out.name        = p.value("name", std::string());
        out.description = p.value("puzzle_description", std::string());
        out.type        = p.value("puzzle_type", std::string());
        out.width       = p.value("width", 19);
        out.height      = p.value("height", 19);
        if (p.contains("initial_state") && p["initial_state"].is_object()) {
            out.initial_black = p["initial_state"].value("black", std::string());
            out.initial_white = p["initial_state"].value("white", std::string());
        }
        out.black_to_play = p.value("initial_player", std::string("black")) == "black";
        out.solver_black  = out.black_to_play;   // real OGS puzzles: solver always moves first
        out.opponent_auto = p.value("puzzle_opponent_move_mode", std::string("automatic"))
                            == "automatic";
        if (j.contains("collection") && j["collection"].is_object()) {
            out.collection_id   = j["collection"].value("id", 0);
            out.collection_name = j["collection"].value("name", std::string());
        }
        if (p.contains("move_tree") && p["move_tree"].is_object())
            parse_move_tree(p["move_tree"], out.tree);
    } catch (...) {
        return false;
    }
    return true;
}

bool ogs_fetch_puzzle_collections(int page, int page_size,
                                  std::vector<OgsPuzzleCollection>& out,
                                  int& total_out) {
    json j;
    // Most-viewed first — page 1 starts with the classics (Cho Chikun etc.)
    // instead of the API's arbitrary default order.
    if (!http_get_json("https://online-go.com/api/v1/puzzles/collections/?page="
                       + std::to_string(page) + "&page_size=" + std::to_string(page_size)
                       + "&ordering=-view_count", j))
        return false;
    if (!j.contains("results") || !j["results"].is_array()) return false;

    try {
        total_out = j.value("count", 0);
        out.clear();
        for (const auto& r : j["results"]) {
            OgsPuzzleCollection c;
            c.id           = r.value("id", 0);
            c.name         = r.value("name", std::string());
            if (r.contains("owner") && r["owner"].is_object())
                c.owner = r["owner"].value("username", std::string());
            c.puzzle_count = r.value("puzzle_count", 0);
            c.min_rank     = r.value("min_rank", 0);
            c.max_rank     = r.value("max_rank", 0);
            c.rating       = r.value("rating", 0.f);
            c.rating_count = r.value("rating_count", 0);
            if (r.contains("starting_puzzle") && r["starting_puzzle"].is_object())
                c.starting_puzzle_id = r["starting_puzzle"].value("id", 0);
            out.push_back(std::move(c));
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool ogs_fetch_collection_puzzles(int puzzle_id,
                                  std::vector<std::pair<int, std::string>>& out) {
    json j;
    if (!http_get_json("https://online-go.com/api/v1/puzzles/"
                       + std::to_string(puzzle_id) + "/collection_summary", j))
        return false;
    if (!j.is_array()) return false;

    try {
        out.clear();
        for (const auto& e : j)
            out.push_back({e.value("id", 0), e.value("name", std::string())});
    } catch (...) {
        return false;
    }
    return true;
}

// ── goproblems.com ────────────────────────────────────────────────────────────

bool gp_load_index(const std::string& path, std::vector<GpIndexEntry>& out) {
    out.clear();
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    // id \t level \t genre — header line first
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* fields[3] = {};
        int n = 0;
        for (char* s = line; n < 3 && s; n++) {
            fields[n] = s;
            s = strchr(s, '\t');
            if (s) *s++ = '\0';
        }
        if (n < 3) continue;
        GpIndexEntry e;
        e.gp_id = atoi(fields[0]);
        if (e.gp_id <= 0) continue;   // header row, or junk
        e.level = atoi(fields[1]);
        e.genre = fields[2];
        while (!e.genre.empty() && (e.genre.back() == '\n' || e.genre.back() == '\r'))
            e.genre.pop_back();
        out.push_back(std::move(e));
    }
    fclose(f);
    return !out.empty();
}

// Generic SGF tree: each node's properties in file order plus its children. A
// sequence's main line is chained as children too, so variations are just
// sibling children and the whole file is one uniform tree. Kept separate from
// main.cpp's drill parser, which is strict about the subset our own writer
// emits — goproblems files are 25 years of assorted editors, and a bad line in
// one should cost that line, not the whole problem.
struct GpSgfNode {
    std::vector<std::pair<std::string, std::vector<std::string>>> props;
    std::vector<GpSgfNode> children;

    // Every value of property id — repeated properties (AB[..]AB[..]) are common
    std::vector<std::string> all(const char* id) const {
        std::vector<std::string> v;
        for (const auto& p : props)
            if (p.first == id) v.insert(v.end(), p.second.begin(), p.second.end());
        return v;
    }
    bool has_move() const {
        for (const auto& p : props)
            if (p.first == "B" || p.first == "W") return true;
        return false;
    }
};

static constexpr int GP_MAX_DEPTH = 400;

// Parse one GameTree (p at its '(') and hang its sequence under parent.
static bool gp_parse_tree(const char*& p, GpSgfNode& parent, int depth, std::string& err) {
    p++;   // '('
    GpSgfNode* cur = &parent;
    while (*p) {
        char c = *p;
        if (isspace((unsigned char)c)) { p++; continue; }
        if (c == ')') { p++; return true; }
        if (c == '(') {
            if (!gp_parse_tree(p, *cur, depth, err)) return false;
            continue;
        }
        if (c != ';') { err = "MALFORMED SGF"; return false; }
        p++;
        if (++depth > GP_MAX_DEPTH) { err = "SGF TOO DEEP"; return false; }
        cur->children.emplace_back();
        cur = &cur->children.back();
        for (;;) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!isalpha((unsigned char)*p)) break;
            // FF[3] long names ("AddBlack") reduce to their capitals ("AB")
            std::string ident;
            for (; isalpha((unsigned char)*p); p++)
                if (isupper((unsigned char)*p)) ident += *p;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p != '[') { err = "MALFORMED SGF"; return false; }
            std::vector<std::string> vals;
            while (*p == '[') {
                std::string v;
                for (p++; *p && *p != ']'; p++) {
                    if (*p == '\\' && p[1]) p++;
                    v += *p;
                }
                if (*p != ']') { err = "UNTERMINATED SGF"; return false; }
                p++;
                vals.push_back(std::move(v));
                // Stray ']' from unescaped brackets in comments ("C[[note]]")
                while (*p == ']' || (*p && isspace((unsigned char)*p))) p++;
            }
            cur->props.emplace_back(std::move(ident), std::move(vals));
        }
    }
    err = "UNTERMINATED SGF";
    return false;
}

static bool gp_coord(const std::string& v, int bs, int& x, int& y) {
    if (v.size() != 2) return false;
    x = v[0] - 'a';
    y = v[1] - 'a';
    return x >= 0 && x < bs && y >= 0 && y < bs;
}

static std::string gp_trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

// Append AB/AW-style point values to a 2-char coordinate string, expanding
// FF[4] compressed rectangles ("aa:cc").
static void gp_add_points(const std::vector<std::string>& vals, int bs, std::string& dst) {
    for (const auto& v : vals) {
        int x0, y0, x1, y1;
        if (v.size() == 5 && v[2] == ':' &&
            gp_coord(v.substr(0, 2), bs, x0, y0) && gp_coord(v.substr(3, 2), bs, x1, y1)) {
            for (int y = std::min(y0, y1); y <= std::max(y0, y1); y++)
                for (int x = std::min(x0, x1); x <= std::max(x0, x1); x++) {
                    dst += (char)('a' + x);
                    dst += (char)('a' + y);
                }
        } else if (gp_coord(v, bs, x0, y0)) {
            dst += v;
        }
    }
}

// A node's board markup: letter labels as-is, shapes reduced to the glyphs the
// OGS parser uses. Setup nodes carry it too — "kill the marked stones" problems
// triangle their targets in the root.
static void gp_add_marks(const GpSgfNode& n, int bs, std::vector<PuzzleMark>& out) {
    int x, y;
    for (const auto& v : n.all("LB"))
        if (v.size() >= 4 && v[2] == ':' && gp_coord(v.substr(0, 2), bs, x, y))
            out.push_back({x, y, (char)toupper((unsigned char)v[3])});
    const std::pair<const char*, char> shapes[] = {
        {"TR", '^'}, {"SQ", '#'}, {"CR", 'O'}, {"MA", 'X'}};
    for (const auto& s : shapes) {
        std::string pts;
        gp_add_points(n.all(s.first), bs, pts);   // expands "aa:cc" rectangles
        for (size_t i = 0; i + 1 < pts.size(); i += 2)
            out.push_back({pts[i] - 'a', pts[i + 1] - 'a', s.second});
    }
}

// Graft n's move children onto dst as solution branches. expect = the color
// whose turn it is (1 = black). goproblems' convention: a line is correct
// where a comment contains "RIGHT"; every other line end is wrong. Moves out
// of turn or passes can't be answered on the board, so such a line is dropped;
// move-less nodes (comment/markup only) are looked through. Transpositions to
// the same point merge into one branch.
static void gp_add_moves(const GpSgfNode& n, PuzzleMoveNode& dst, int expect, int bs) {
    for (const auto& ch : n.children) {
        std::vector<std::string> b = ch.all("B"), w = ch.all("W");
        if (b.empty() && w.empty()) { gp_add_moves(ch, dst, expect, bs); continue; }
        if (!b.empty() && !w.empty()) continue;
        int color = b.empty() ? 0 : 1;
        int x, y;
        if (color != expect || !gp_coord(color ? b[0] : w[0], bs, x, y)) continue;

        PuzzleMoveNode* node = nullptr;
        for (auto& e : dst.branches)
            if (e.x == x && e.y == y) { node = &e; break; }
        if (!node) {
            dst.branches.emplace_back();
            node = &dst.branches.back();
            node->x = x;
            node->y = y;
        }
        std::string text;
        for (const auto& c : ch.all("C")) text += c;
        size_t right = text.find("RIGHT");
        if (right != std::string::npos) {
            node->correct = true;
            text.erase(right, 5);
        }
        size_t choice = text.find("CHOICE");   // site-editor marker, not commentary
        if (choice != std::string::npos) text.erase(choice, 6);
        text = gp_trim(text);
        if (node->text.empty()) node->text = text;

        gp_add_marks(ch, bs, node->marks);
        gp_add_moves(ch, *node, 1 - expect, bs);
    }
}

static bool gp_tree_has_correct(const PuzzleMoveNode& n) {
    if (n.correct) return true;
    for (const auto& b : n.branches)
        if (gp_tree_has_correct(b)) return true;
    return false;
}

// SGF → out's board, setup, rank, genre and solution tree. The side to move
// comes from PL[] when present (B/W, or 1/2 in old files), else the first move.
static bool gp_parse_problem(const std::string& sgf, OgsPuzzle& out, std::string& err) {
    GpSgfNode top;
    const char* p = sgf.c_str();
    while (*p && *p != '(') p++;
    if (!*p) { err = "NOT AN SGF"; return false; }
    if (!gp_parse_tree(p, top, 0, err)) return false;
    if (top.children.empty()) { err = "EMPTY SGF"; return false; }

    const GpSgfNode* node = &top.children[0];
    std::vector<std::string> sz = node->all("SZ");
    int bs = sz.empty() ? 19 : atoi(sz[0].c_str());
    if (bs < 2 || bs > 25) { err = "UNSUPPORTED BOARD SIZE"; return false; }
    out.width = out.height = bs;

    // DI[12k] / DI[3d] → the OGS rank scale (29 = 1k, 30 = 1d); 30k clamps to 1
    std::vector<std::string> di = node->all("DI");
    if (!di.empty()) {
        int v = atoi(di[0].c_str());
        char unit = 0;
        for (char c : di[0]) if (isalpha((unsigned char)c)) { unit = (char)tolower((unsigned char)c); break; }
        out.rank = unit == 'k' ? std::max(1, 30 - v) : unit == 'd' ? 29 + v : 0;
    }
    std::vector<std::string> ge = node->all("GE");
    if (!ge.empty()) out.type = gp_trim(ge[0]);

    // Setup: the root plus any move-less nodes chained straight below it. A move
    // inside a setup node (a few old files do this) is just another stone.
    int player = -1;
    std::string comment;
    for (;;) {
        gp_add_points(node->all("AB"), bs, out.initial_black);
        gp_add_points(node->all("AW"), bs, out.initial_white);
        gp_add_points(node->all("B"),  bs, out.initial_black);
        gp_add_points(node->all("W"),  bs, out.initial_white);
        for (const auto& c : node->all("C")) comment += c;
        gp_add_marks(*node, bs, out.tree.marks);   // shown on the starting position
        std::vector<std::string> pl = node->all("PL");
        if (player < 0 && !pl.empty() && !pl[0].empty())
            player = (pl[0][0] == 'B' || pl[0][0] == 'b' || pl[0][0] == '1') ? 1 : 0;
        if (node->children.size() != 1 || node->children[0].has_move()) break;
        node = &node->children[0];
    }
    if (player < 0) {
        // First move anywhere below decides it
        std::vector<const GpSgfNode*> stack{node};
        while (!stack.empty() && player < 0) {
            const GpSgfNode* n = stack.back();
            stack.pop_back();
            for (const auto& ch : n->children) {
                if (!ch.all("B").empty()) { player = 1; break; }
                if (!ch.all("W").empty()) { player = 0; break; }
                stack.push_back(&ch);
            }
        }
        if (player < 0) player = 1;
    }

    gp_add_moves(*node, out.tree, player, bs);
    if (out.tree.branches.empty()) {
        // PL[] occasionally disagrees with the moves the file actually holds
        player = 1 - player;
        gp_add_moves(*node, out.tree, player, bs);
    }
    if (out.tree.branches.empty()) { err = "NO PLAYABLE MOVES"; return false; }
    out.tree.correct = false;
    if (!gp_tree_has_correct(out.tree)) { err = "NO MARKED SOLUTION"; return false; }

    out.black_to_play = out.solver_black = (player == 1);
    out.description   = gp_trim(comment);
    return true;
}

bool gp_load_problem(const std::string& path, int gp_id, OgsPuzzle& out, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "FILE MISSING"; return false; }
    std::string sgf;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && sgf.size() < 8 * 1024 * 1024)
        sgf.append(buf, n);
    fclose(f);

    OgsPuzzle pz;
    pz.id              = GP_ID_BASE + gp_id;
    pz.name            = "GOPROBLEMS " + std::to_string(gp_id);
    pz.collection_id   = GP_COLLECTION_ID;
    pz.collection_name = "GOPROBLEMS.COM";
    pz.opponent_auto   = true;
    if (!gp_parse_problem(sgf, pz, err)) return false;
    if (pz.description.empty())
        pz.description = pz.solver_black ? "BLACK TO PLAY" : "WHITE TO PLAY";
    out = std::move(pz);
    return true;
}

bool ogs_fetch_joseki(const std::string& node_id, JosekiPosition& out) {
    json j;
    if (!http_get_json("https://online-go.com/oje/position?id=" + node_id, j))
        return false;
    if (!j.is_object()) return false;

    try {
        out = JosekiPosition{};
        // node_id arrives as a string for "root" fetches and a number elsewhere
        if (j.contains("node_id")) {
            const auto& n = j["node_id"];
            out.node_id = n.is_string() ? n.get<std::string>()
                                        : std::to_string(n.get<long long>());
        }
        out.placement   = j.value("placement", std::string());
        out.category    = j.value("category", std::string());
        out.child_count = j.value("child_count", 0);
        if (j.contains("description") && j["description"].is_string())
            out.description = j["description"].get<std::string>();
        if (j.contains("joseki_source") && j["joseki_source"].is_object())
            out.source_desc = j["joseki_source"].value("description", std::string());
        if (j.contains("next_moves") && j["next_moves"].is_array()) {
            for (const auto& m : j["next_moves"]) {
                JosekiNextMove nm;
                nm.placement       = m.value("placement", std::string());
                nm.category        = m.value("category", std::string());
                nm.variation_label = m.value("variation_label", std::string());
                const auto& n = m["node_id"];
                nm.node_id = n.is_string() ? n.get<std::string>()
                                           : std::to_string(n.get<long long>());
                out.next_moves.push_back(std::move(nm));
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}
