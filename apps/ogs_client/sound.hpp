#pragma once
#include <SDL2/SDL.h>

// Tiny synthesized-audio helper on top of SDL2's queued audio API — no files,
// no mixer library. Sounds are generated as sample buffers on demand and handed
// to SDL_QueueAudio. Queued sounds play back-to-back rather than mixed, which is
// fine for short one-shot clicks and chimes.
//
// All methods are safe no-ops if init() failed (e.g. no audio device present).
class Sound {
public:
    bool init();        // opens the default audio device; call once at startup
    void shutdown();

    void play_game_start();          // rising two-tone chime
    // Stone "clack", modelled on the first reference recording (see sound.cpp).
    // Played for each stone placed in a game, and when guess mode steps onto the answer.
    void play_stone(bool mine);
    // Captured stones thrown back into the wooden bowl, modelled on the matching
    // recording — more stones, more clicks (by the handful, capped at 20). Queue
    // it right after play_stone and it follows the placing sound.
    void play_capture(int count);

private:
    SDL_AudioDeviceID dev_  = 0;
    int               rate_ = 48000;

    // One clack's parameters, as multipliers on the reference-fit model
    // (vol and lpf are absolute). random_shape() gives the per-strike variation.
    struct ClackShape {
        float vol = 0.32f, fs = 1, blen = 1, bdec = 1, cut = 1, ring = 1, flvl = 1,
              tilt = 0, lpf = 5600.0f;
    };
    static ClackShape random_shape(unsigned& rng);
    void render_clack(float* out, int n, const ClackShape& c, unsigned& rng) const;  // adds into out

    struct Contact { float t, amp; };  // one touch of stone on stone/board
    // Excitation for a train of contacts, added into e (and the first contact
    // alone into first, if given).
    void build_contacts(float* e, float* first, int n, const Contact* c, int nc,
                        unsigned& rng) const;
    void limit(float* x, int n) const;  // lookahead peak limiter — mixed sounds never clip

    void queue(const float* samples, int count);
};
