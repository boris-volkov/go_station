#pragma once
#include <string>
#include <vector>
#include <utility>

// ── Correspondence games ───────────────────────────────────────────────────
// One row in the "MY GAMES" list, built from the OGS ui/overview response. This
// is a lightweight summary only — opening a row sends game/connect and the full
// board arrives through the usual GAME_CONNECTED path, same as a live game.
struct CorrGameSummary {
    int         id         = 0;
    std::string opp;                 // opponent's username (the side that isn't me)
    int         board_size = 19;
    bool        my_move    = false;  // true = it's my turn (clock.current_player == me)
};

// A friend, from GET /api/v1/me/friends/ — the opponent picker for a new challenge.
struct FriendSummary {
    int         id = 0;
    std::string username;
};

// Parameters for a challenge we are issuing (see OgsNet::send_challenge).
struct ChallengeRequest {
    int         opponent_id    = 0;        // 0 = open challenge (anyone may accept)
    int         board_size     = 19;
    bool        ranked         = true;
    // Correspondence Fischer clock, in seconds.
    int         initial_time   = 259200;   // 3 days
    int         time_increment = 259200;   // +3 days per move
    int         max_time       = 604800;   // 7 day cap
    std::string color          = "automatic";  // automatic | black | white
};

// One row in the incoming-challenges list, from GET /api/v1/me/challenges/.
struct ChallengeSummary {
    int         id            = 0;      // challenge id — for accept/decline
    std::string challenger;             // who is challenging me
    int         board_size    = 19;
    bool        ranked        = false;
    bool        correspondence = true;  // false = a live challenge
    std::string time_desc;              // short human summary of the time control
};

// ── Inbound: network thread → main thread ──────────────────────────────────

enum class NetMsgType {
    AUTH_OK,         // authenticated; player info set in OgsNet
    AUTH_FAIL,       // login or socket auth failed; check msg.text for reason
    MATCH_FOUND,     // automatch start; game_id valid; we auto-send game/connect
    CORR_LIST_UPDATED,  // active_game events processed; corr_games holds the list
    CHALLENGES_UPDATED, // me/challenges fetch finished; challenges holds the list
    CHALLENGE_RESULT,   // an accept/decline/send finished; text = result message
    FRIENDS_UPDATED,    // me/friends fetch finished; friends holds the list
    GAME_CONNECTED,  // gamedata received; board state, names, clocks populated
    OPPONENT_MOVE,   // opponent played; col/row valid (-1/-1 = pass)
    CLOCK_UPDATE,    // black_secs / white_secs updated
    STONE_REMOVAL,   // game entered stone removal phase; text holds server's dead-stone payload
    GAME_OVER,       // phase changed to "finished"; text holds the result string
    UNDO_REQUESTED,  // opponent requested undo; undo_move_number holds the move to undo
    RESUME_PLAY,     // stone removal cancelled; game returned to play phase
    DISCONNECTED,    // connection lost unexpectedly; the net thread is already retrying
    RECONNECTED,     // socket is back up; a live game's GAME_CONNECTED rebuild follows
};

struct NetMsg {
    NetMsgType type = NetMsgType::DISCONNECTED;

    // MATCH_FOUND / GAME_CONNECTED / OPPONENT_MOVE / CLOCK_UPDATE / GAME_OVER
    int game_id = 0;

    // GAME_CONNECTED
    int board_size      = 19;
    int my_color        = 1;    // 1 = black, 0 = white
    int my_player_id    = 0;
    int opp_player_id   = 0;
    std::string black_name, white_name;
    std::string black_rank, white_rank;
    // Handicap info
    int   handicap      = 0;    // number of handicap stones
    bool  free_handicap = false; // true → first `handicap` move events are all black placements
    int  initial_player = 1;    // who plays the first real move: 1=black, 0=white
    // Pre-placed stones from initial_state (non-free handicap only); always black
    std::vector<std::pair<int,int>> initial_handicap_stones;
    // moves already played (reconnect / non-fresh game); [col,row] 0-indexed
    std::vector<std::pair<int,int>> initial_moves;

    // OPPONENT_MOVE
    int col         = -1;   // -1 = pass
    int row         = -1;
    int move_number = 0;

    // CLOCK_UPDATE / GAME_CONNECTED
    int black_secs        = -1;  // thinking_time (main time or current byo-yomi period)
    int white_secs        = -1;
    int black_periods     = -1;  // byo-yomi periods remaining (-1 = not byo-yomi)
    int white_periods     = -1;
    int black_period_secs = -1;  // seconds per byo-yomi period
    int white_period_secs = -1;
    // Main time exhausted — the player is living on byo-yomi periods. Determined at
    // parse time (period_time_left present, or thinking_time 0 with periods left);
    // the display values alone can't distinguish byo-yomi from low main time.
    bool black_in_byo     = false;
    bool white_in_byo     = false;
    // clock.current_player — the OGS player id whose clock is running. The server's
    // own answer to "whose move is it", used to detect a live move that never
    // arrived. 0 = the event carried no clock.
    int  current_player   = 0;

    // UNDO_REQUESTED: move number the opponent wants to undo
    int undo_move_number = 0;

    // GAME_OVER / AUTH_FAIL / DISCONNECTED
    std::string text;

    // STONE_REMOVAL: raw JSON ownership array (2D, [row][col]: 1=black, -1=white, 0=neutral)
    std::string ownership_json;

    // CORR_LIST_UPDATED: the user's ongoing correspondence games (whose-turn included)
    std::vector<CorrGameSummary> corr_games;

    // CHALLENGES_UPDATED: challenges awaiting the user's response
    std::vector<ChallengeSummary> challenges;

    // FRIENDS_UPDATED: the user's friends, as challenge opponents
    std::vector<FriendSummary> friends;
};

// ── Match preferences (used to build automatch payload) ───────────────────

struct MatchPrefs {
    // Board sizes — index 0=9x9, 1=13x13, 2=19x19
    bool sizes[3]  = {false, false, true};
    // Speeds    — index 0=blitz (fast), 1=rapid (medium), 2=live (slow)  (OGS mode only)
    bool speeds[3] = {false, true, true};
    // Local play vs KataGo human SL model
    // No katago_mode flag: OGS and local settings are separate screens (MATCH
    // SETTINGS / KATAGO SETTINGS) and which game you start is chosen explicitly
    // in the popup, so there is no persistent "mode" to remember.
    int  katago_str  = 8;   // index into strength table (0=20k … 7=9d, 8=adaptive)
    // Board size for local play, kept apart from `sizes` above: that one is a
    // multi-select widening the OGS automatch pool, this is the single size one
    // local game is played on. Sharing them meant picking 9x9 for KataGo silently
    // narrowed OGS automatch to 9x9-only. Index: 0=9x9, 1=13x13, 2=19x19.
    int  katago_size = 2;
};

// ── Outbound: main thread → network thread ─────────────────────────────────

enum class CmdType {
    FIND_MATCH,
    CANCEL_MATCH,
    SEND_MOVE,    // col + row set
    SEND_PASS,
    SEND_RESIGN,
};

struct NetCmd {
    CmdType type    = CmdType::FIND_MATCH;
    int     game_id = 0;
    int     col     = 0;
    int     row     = 0;
};
