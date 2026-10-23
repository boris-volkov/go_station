#include "sound.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <vector>

static const float PI2 = 6.28318530718f;

bool Sound::init() {
    if (dev_) return true;
    if (!(SDL_WasInit(SDL_INIT_AUDIO)) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;

    SDL_AudioSpec want{}, have{};
    want.freq     = rate_;
    want.format   = AUDIO_F32SYS;
    want.channels = 1;
    want.samples  = 1024;
    want.callback = nullptr;  // queued mode — we push samples with SDL_QueueAudio

    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) return false;
    rate_ = have.freq;
    SDL_PauseAudioDevice(dev_, 0);  // start playback (silent until something queues)
    return true;
}

void Sound::shutdown() {
    if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
}

void Sound::queue(const float* samples, int count) {
    if (!dev_ || count <= 0) return;
    SDL_QueueAudio(dev_, samples, (Uint32)count * sizeof(float));
}

// Rising two-tone chime: C5 then G5, each a sine with a soft attack and
// exponential decay so it doesn't click at the edges.
void Sound::play_game_start() {
    if (!dev_) return;
    const float vol = 0.22f;
    struct Note { float freq, dur; };
    const Note notes[] = { {523.25f, 0.11f}, {783.99f, 0.18f} };

    std::vector<float> buf;
    for (const Note& nt : notes) {
        int n = (int)(nt.dur * rate_);
        for (int i = 0; i < n; i++) {
            float t   = (float)i / rate_;
            float env = std::exp(-t * 14.0f) * std::min(1.0f, t * 400.0f);
            buf.push_back(vol * env * std::sin(PI2 * nt.freq * t));
        }
    }
    queue(buf.data(), (int)buf.size());
}

namespace {

// Two-pole bandpass resonator (zeros at DC and Nyquist). `k` is the amplitude
// decay rate in 1/s, so a small k rings long and a large k is a broad, short
// colouring band. Input is scaled by (1-r) to keep gains comparable across k.
struct Resonator {
    float g, b0, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    Resonator(float freq, float k, float gain, int rate) {
        float r = std::exp(-k / rate);
        g  = gain;
        b0 = 1.0f - r;
        a1 = 2.0f * r * std::cos(PI2 * freq / rate);
        a2 = -r * r;
    }
    float step(float x) {
        float y = b0 * (x - x2) + a1 * y1 + a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return g * y;
    }
};

// RBJ Butterworth lowpass biquad (direct form I).
struct Lowpass {
    float b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    Lowpass(float fc, int rate) {
        float w = PI2 * fc / rate, al = std::sin(w) / (2.0f * 0.7071f), c = std::cos(w);
        float a0 = 1.0f + al;
        b0 = (1.0f - c) / 2.0f / a0; b1 = (1.0f - c) / a0; b2 = b0;
        a1 = -2.0f * c / a0;         a2 = (1.0f - al) / a0;
    }
    float step(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

} // namespace

static float frand(unsigned& rng) {  // uniform in [-1, 1]
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 8388608.0f - 1.0f;
}
static float urand(unsigned& rng, float lo, float hi) {  // uniform in [lo, hi]
    return lo + (hi - lo) * (0.5f + 0.5f * frand(rng));
}

static unsigned fresh_seed() {
    static unsigned seq = 0;
    return ((unsigned)SDL_GetTicks() + ++seq * 7919u) * 2654435761u + 12345u;
}

// ── Capture model (fit to removal.mp3) ───────────────────────────────────────
//
// A modal synthesis fit offline to Boris's own recording of stones thrown back
// into the bowl. A click is a *contact* — a sharp 3-sample tick plus a ~1ms
// burst of rattle — driving a bank of two-pole resonators; the gains were solved
// by least squares against the recording's band energies in short time windows,
// to within ~5 dB per band. (Placement uses the earlier clack model further
// down — a fit of stones.mp3 came out sounding too muted.)

struct Mode { float freq, k, gain; };  // k: amplitude decay rate, 1/s

// One stone hitting the pile in the bowl: a short ping, brightest around 7-9 kHz.
// The fit was made on isolated clicks; a whole throw built from it came out
// muted next to the recording — boxy at 300-1000 Hz (the bowl stacks on top),
// ~5.5 dB short of the 1-4 kHz "knock", and softer on the attack. The gains
// below carry that correction (638 Hz down, 1.5-4.8 kHz and the top two up),
// which puts a full throw within ~1.6 dB of the recording in every band.
static const Mode CLICK[] = {
    {  638, 300,  6.981f}, { 1500, 300, 16.357f}, { 2197, 300, 11.033f}, { 2906, 300, 11.700f},
    { 3800, 300, 14.735f}, { 4781, 300, 11.397f}, { 6000, 300, 12.529f},
    { 7461, 300, 15.101f}, { 7461, 800, 6.577f}, { 7461, 2000, 2.844f},
    { 9111, 300, 20.240f}, { 9111, 2000, 4.770f},
    {10464, 300, 13.704f}, {10464, 800, 2.359f}, {10464, 2000, 3.853f},
    {12817, 300, 11.802f}, {12817, 5000, 4.632f}, {14499, 300, 9.006f}, {14499, 5000, 3.740f},
};
static const float CLICK_NORM = 1.0f / 1.373f;
// The wooden bowl's low, hollow ring (measured after the throws, decaying ~29/s),
// fed by every click — so a big throw builds up a body.
static const Mode BOWL[] = {
    {155, 25, 1}, {231, 28, 1}, {310, 30, 1}, {474, 32, 1}, {506, 32, 1},
    {536, 32, 1}, {691, 36, 1}, {770, 38, 1}, {832, 40, 1}, {896, 42, 1},
};
static const float BOWL_GAIN   = 6.19f;   // sits ~3 dB under the clicks' low end
static const float WASH_GAIN   = 0.0335f; // diffuse wash between clicks, ~-45 dB
static const float WASH_DECAY  = 0.025f;  // s, per click

// Contact excitation: a 3-sample Hann tick (full strength on the first contact,
// half on the rest) plus a decaying rattle burst (0.6ms time constant on the
// first contact, 1ms after). The burst is kept low (0.15 of the tick; the fit
// used 0.35) — more of it smeared the attack and dulled the clicks.
void Sound::build_contacts(float* e, float* first, int n, const Contact* c, int nc,
                           unsigned& rng) const {
    static const float TICK_W[3] = {0.5f, 1.0f, 0.5f};
    for (int j = 0; j < nc; j++) {
        int   i0  = (int)(c[j].t * rate_);
        float a   = c[j].amp;
        float tau = (j == 0 ? 0.0006f : 0.0010f) * rate_;
        int   L   = std::min((int)(5.0f * tau), n - i0);
        for (int m = 0; m < 3 && i0 + m < n; m++) {
            float v = a * TICK_W[m] * frand(rng) * (j == 0 ? 1.0f : 0.5f);
            e[i0 + m] += v;
            if (first && j == 0) first[i0 + m] += v;
        }
        for (int m = 0; m < L; m++) {
            float v = a * 0.15f * frand(rng) * std::exp(-(float)m / tau);
            e[i0 + m] += v;
            if (first && j == 0) first[i0 + m] += v;
        }
    }
}

// Per-strike variation of one clack. One "force" value drives the dimensions
// that go together physically: a harder strike is louder, a touch brighter and a
// touch shorter; the rest wander independently. Besides fresh noise every time,
// this is what keeps the 300th clack of a game from sounding like a loop.
Sound::ClackShape Sound::random_shape(unsigned& rng) {
    ClackShape c;
    const float force = frand(rng);                    // -1 soft .. +1 hard
    c.vol  = 0.32f * (1.0f + 0.12f * force);           // loudness ±12%
    c.tilt = 0.12f * force + 0.08f * frand(rng);       // brightness (gain slope)
    c.fs   = 1.0f + 0.03f * frand(rng);                // overall pitch ±3%
    c.blen = 1.0f - 0.06f * force + 0.08f * frand(rng);// burst length
    c.bdec = 1.0f + 0.15f * frand(rng);                // burst decay ±15%
    c.cut  = 1.0f + 0.20f * frand(rng);                // gate slam ±20%
    c.ring = 1.0f + 0.15f * frand(rng);                // ring decay ±15%
    c.flvl = 1.0f + 0.25f * frand(rng);                // floor level ±25%
    c.lpf  = 5600.0f * (1.0f + 0.04f * c.tilt / 0.2f); // cutoff follows brightness
    return c;
}

// Go-stone "clack", modelled on a reference recording Boris picked (fit offline
// against its time-frequency band energies). Three parts:
//   * burst: ~8.5ms of noise, gated off sharply, driven through broad resonators
//     that give the impact its colour (strongest around 3.4 kHz and 2.5 kHz);
//   * ring: the same burst excites narrow resonators that keep sounding after the
//     gate closes — the stone/board modes, gone in ~40ms;
//   * floor: a separate, quieter noise stream decaying with the ring, filling the
//     gaps between modes the way the board's diffuse resonance does.
// Everything goes through a steep lowpass (5.6 kHz by default) — the reference
// has nothing above that, and the missing fizz is a big part of why it sounds
// soft and woody. Mixed (added) into out[0..n) — never overwrites.
void Sound::render_clack(float* out, int n, const ClackShape& c, unsigned& rng) const {
    struct Mode { float freq, k, gain; };
    static const Mode BURST[] = {   // broad: colour the impact
        { 160, 500, 1.561f}, { 480, 1200, 1.071f}, {1650, 1200, 1.708f}, {2450, 1000, 2.562f},
        {2900, 1000, 1.685f}, {3450, 900, 5.983f}, {4700, 1000, 2.614f},
    };
    static const Mode RING[] = {    // narrow: ring on after the burst
        { 938, 100, 0.940f}, {1549, 100, 1.197f}, {2078, 100, 1.050f}, {2452, 100, 1.568f},
        {3493, 100, 2.255f}, {4557, 100, 1.987f}, {5172, 100, 3.507f},
    };
    static const Mode FLOOR[] = {   // broad bands over the decaying floor noise
        { 450, 2500, 0.357f}, {3000, 2500, 0.192f}, {4800, 2500, 0.895f},
    };
    const float nyq = 0.45f * rate_;  // pitched-up clinks must stay below Nyquist

    // Per-mode: each resonance lands slightly differently (±1.5% pitch, ±20%
    // level), like striking a different spot on the stone and the board.
    auto mode_gain = [&](float freq, float g) {
        return g * std::pow(freq / 2500.0f, c.tilt) * (1.0f + 0.20f * frand(rng));
    };
    auto mode_freq = [&](float freq) {
        return std::min(nyq, freq * c.fs * (1.0f + 0.015f * frand(rng)));
    };
    std::vector<Resonator> exc_bank, floor_bank;
    for (const Mode& m : BURST)
        exc_bank.emplace_back(mode_freq(m.freq), m.k, mode_gain(m.freq, m.gain), rate_);
    for (const Mode& m : RING)
        exc_bank.emplace_back(mode_freq(m.freq), m.k * c.ring, mode_gain(m.freq, m.gain), rate_);
    for (const Mode& m : FLOOR)
        floor_bank.emplace_back(std::min(nyq, m.freq * c.fs), m.k,
                                c.flvl * mode_gain(m.freq, m.gain), rate_);
    const float lpf = std::min(nyq, c.lpf);
    Lowpass lp1(lpf, rate_), lp2(lpf, rate_);  // cascaded: 4th order

    const float BURST_LEN = 0.0085f * c.blen;  // gate open
    const float BURST_K   = 130.0f  * c.bdec;  // decay while open
    const float CUT_K     = 1300.0f * c.cut;   // how fast the gate slams shut
    const float RING_K    = 100.0f  * c.ring;  // floor-noise decay, matched to the ring
    const float NORM      = 1.0f / 1.2f;       // unit render peaks around 1.1-1.25
    const float gate_at_cut = std::exp(-BURST_K * BURST_LEN);
    for (int i = 0; i < n; i++) {
        float t    = (float)i / rate_;
        float att  = std::min(1.0f, t / 0.0004f);   // 0.4ms rise — no click at the start
        float gate = (t < BURST_LEN) ? std::exp(-BURST_K * t)
                                     : gate_at_cut * std::exp(-CUT_K * (t - BURST_LEN));
        float exc  = frand(rng) * att * gate;
        float flr  = frand(rng) * att * std::exp(-RING_K * t);
        float s = 0.0f;
        for (Resonator& r : exc_bank)   s += r.step(exc);
        for (Resonator& r : floor_bank) s += r.step(flr);
        out[i] += lp2.step(lp1.step(s)) * NORM * c.vol;
    }
}

void Sound::play_stone(bool mine) {
    if (!dev_) return;
    (void)mine;  // both sides click alike, per Boris — param kept for easy re-split
    unsigned rng = fresh_seed();
    const int n = (int)(0.07f * rate_);
    std::vector<float> buf((size_t)n, 0.0f);
    render_clack(buf.data(), n, random_shape(rng), rng);
    limit(buf.data(), n);
    queue(buf.data(), n);
}

// Captured stones thrown back into the bowl. As in the recording:
//   * each stone sets off a short cluster of clicks as it lands on the pile —
//     2-5 of them, typically ~8ms apart (never closer than 3ms), the first one
//     or two strong and the rest fading — plus a couple of faint micro-clicks;
//   * stones in a handful land ~25-45ms after one another, overlapping; past 5
//     stones the next handful follows 60-150ms after;
//   * every click varies in pitch (±12%) and brightness — each is a different
//     pair of stones;
//   * the bowl's low ring builds up under it all, and a faint diffuse wash
//     (following how busy the clicking is) fills the gaps the way the bowl and
//     room do — without it the clicks sound pasted onto silence.
// Everything is mixed into one buffer and run through limit(), so however many
// clicks overlap, the output never clips.
void Sound::play_capture(int count) {
    if (!dev_ || count <= 0) return;
    // A backlog means stones are landing faster than we can play them (e.g. a
    // replay racing ahead) — skip rather than let the audio lag further behind.
    if (SDL_GetQueuedAudioSize(dev_) > (Uint32)(rate_ * sizeof(float)) / 2) return;
    unsigned rng = fresh_seed();

    const int   stones   = std::min(count, 20);
    const int   per_hand = 5;
    const float lead_in  = 0.12f;   // the hand reaching over after the placing sound
    const float vol      = 0.30f;

    std::vector<Contact> hits;
    float hand_t = lead_in, st = lead_in;
    for (int s = 0; s < stones; s++) {
        if (s && s % per_hand == 0) hand_t = st + urand(rng, 0.06f, 0.15f);
        st = hand_t + (s % per_hand) * urand(rng, 0.025f, 0.045f);
        float t = st, a = urand(rng, 0.6f, 1.0f);
        int   nc = 2 + (int)urand(rng, 0.0f, 3.999f);
        for (int c = 0; c < nc; c++) {
            hits.push_back({t, a});
            float gap = -0.008f * std::log(std::max(1e-6f, urand(rng, 0.0f, 1.0f)));  // exponential, mean 8ms
            t += std::max(0.003f, gap);
            a *= c ? urand(rng, 0.35f, 0.9f) : urand(rng, 0.7f, 1.05f);
            if (c < 2) a = std::max(a, urand(rng, 0.12f, 0.3f));
        }
        for (int m = (int)urand(rng, 0.0f, 2.999f); m > 0; m--)
            hits.push_back({st + urand(rng, 0.0f, 0.08f), urand(rng, 0.04f, 0.12f)});
    }
    float end_t = 0.0f;
    for (const Contact& h : hits) end_t = std::max(end_t, h.t);
    const int n       = (int)((end_t + 0.40f) * rate_);  // room for the bowl to ring out
    const int click_n = (int)(0.025f * rate_);
    const float nyq   = 0.45f * rate_;
    std::vector<float> buf((size_t)n, 0.0f), bowl_in((size_t)n, 0.0f), dens((size_t)n, 0.0f);
    std::vector<float> e((size_t)click_n);

    for (const Contact& h : hits) {
        int at = (int)(h.t * rate_);
        if (at >= n) continue;
        int m = std::min(click_n, n - at);
        std::fill(e.begin(), e.end(), 0.0f);
        Contact one = {0.0f, 1.0f};
        build_contacts(e.data(), nullptr, m, &one, 1, rng);
        const float fs   = 1.0f + 0.12f * frand(rng);
        const float tilt = 0.25f * frand(rng);
        std::vector<Resonator> bank;
        for (const Mode& md : CLICK)
            bank.emplace_back(std::min(nyq, md.freq * fs), md.k,
                              md.gain * std::pow(md.freq / 7000.0f, tilt), rate_);
        Lowpass lp1(std::min(nyq, 17500.0f), rate_), lp2(std::min(nyq, 17500.0f), rate_);
        for (int i = 0; i < m; i++) {
            float s = 0.0f;
            for (Resonator& r : bank) s += r.step(e[(size_t)i]);
            buf[(size_t)(at + i)]     += h.amp * lp2.step(lp1.step(s)) * CLICK_NORM;
            bowl_in[(size_t)(at + i)] += h.amp * e[(size_t)i];
        }
        dens[(size_t)at] += h.amp;
    }

    // Bowl ring and the diffuse wash, over the whole throw
    std::vector<Resonator> bowl, wash;
    for (const Mode& md : BOWL) bowl.emplace_back(md.freq, md.k, md.gain, rate_);
    for (float fc : {4000.0f, 7000.0f, 10500.0f})
        wash.emplace_back(std::min(nyq, fc), 0.9f * fc, 1.0f, rate_);
    Lowpass wlp1(std::min(nyq, 17500.0f), rate_), wlp2(std::min(nyq, 17500.0f), rate_);
    const float wr   = std::exp(-1.0f / (WASH_DECAY * rate_));
    const float wsc  = WASH_DECAY * rate_;
    float level = 0.0f;
    for (int i = 0; i < n; i++) {
        level = level * wr + (1.0f - wr) * dens[(size_t)i];
        float d = std::min(level * wsc, 2.0f);
        float b = 0.0f, w = 0.0f, src = frand(rng) * d;
        for (Resonator& r : bowl) b += r.step(bowl_in[(size_t)i]);
        for (Resonator& r : wash) w += r.step(src);
        buf[(size_t)i] = vol * (buf[(size_t)i] + BOWL_GAIN * b + WASH_GAIN * wlp2.step(wlp1.step(w)));
    }

    limit(buf.data(), n);
    queue(buf.data(), n);
}

// Peak limiter. For each sample, the gain that keeps it under the ceiling; then
//   1. hold: the minimum of that over the next 2ms (lookahead),
//   2. smooth: a 2ms moving average of the held gain — every value averaged at a
//      peak already includes the peak's own limit, so the gain is fully down by
//      the time it arrives, yet it ramps there instead of stepping (no click),
//   3. release: recover toward unity over ~60ms rather than snapping back.
// Only the moments that would overshoot are turned down; a sound that never
// gets near the ceiling (a single clack) passes through untouched.
void Sound::limit(float* x, int n) const {
    const float CEIL = 0.60f;
    const int   look = std::max(1, (int)(0.002f * rate_));
    const float rel  = std::exp(-1.0f / (0.060f * rate_));
    std::vector<float> need((size_t)n), held((size_t)n);
    bool any = false;
    for (int i = 0; i < n; i++) {
        float a = std::fabs(x[i]);
        need[(size_t)i] = (a > CEIL) ? CEIL / a : 1.0f;
        any |= (a > CEIL);
    }
    if (!any) return;
    for (int i = 0; i < n; i++) {
        float m = 1.0f;
        for (int j = i; j <= i + look && j < n; j++) m = std::min(m, need[(size_t)j]);
        held[(size_t)i] = m;
    }
    float sum = 0.0f, env = 1.0f;
    for (int i = 0; i < n; i++) {
        sum += held[(size_t)i];
        if (i >= look) sum -= held[(size_t)(i - look)];
        // Before the window fills, pad with unity (the gain before the sound starts)
        float avg = (i < look) ? (sum + (float)(look - i - 1)) / (float)look
                               : sum / (float)look;
        env = (avg < env) ? avg : avg + (env - avg) * rel;
        x[i] *= env;
    }
}
