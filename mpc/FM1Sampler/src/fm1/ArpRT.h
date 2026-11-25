// Arpeggiator: the held notes are played one by one on a tempo grid. Free-running clock, sample accurate events.
//
// MPC port of engine/dsp/Arp.h with the same patterns, rates and timing. The phone version keeps the held notes in a
// std::set and builds a std::vector on every step; this one uses fixed arrays so that nothing allocates on the MPC's
// audio thread. The tempo is the MPC's (transport BPM).
#pragma once
#include "Common.h"

struct ArpEv { int offset, note, vel; };   // vel 0 = note off

// A fixed-capacity list of events for one block (never allocates; extra events are dropped).
struct ArpEvList {
    static constexpr int kMax = 256;
    ArpEv ev[kMax]; int n = 0;
    void clear() { n = 0; }
    void push(int offset, int note, int vel) { if (n < kMax) ev[n++] = {offset, note, vel}; }
    void sortByOffset() {   // stable insertion sort: few, nearly sorted events
        for (int i = 1; i < n; i++) { const ArpEv k = ev[i]; int j = i - 1; while (j >= 0 && ev[j].offset > k.offset) { ev[j + 1] = ev[j]; --j; } ev[j + 1] = k; }
    }
};

class ArpRT {
public:
    enum Pattern { UP, DOWN, UPDOWN, CONVERGE, RANDOM };
    static constexpr int kMaxHeld = 32;

    // rate 0..11: 1, 2, 2T, 4, 4T, 8, 8T, 16, 16T, 32, 32T, 64 (note values; T = triplet)
    void setup(int rate, int pattern, int octaves, float gate) {
        static const float beats[12] = {4.f, 2.f, 4.f / 3.f, 1.f, 2.f / 3.f, 0.5f, 1.f / 3.f, 0.25f, 1.f / 6.f, 0.125f, 1.f / 12.f, 0.0625f};
        beats_ = beats[std::min(11, std::max(0, rate))]; pattern_ = pattern; octaves_ = std::min(4, std::max(1, octaves)); gate_ = clampf(gate, 0.05f, 0.95f);
    }
    void noteOn(int note) {   // the first note starts at once
        if (nHeld_ == 0) { clock_ = 1e9; step_ = 0; }
        int i = 0; while (i < nHeld_ && held_[i] < note) i++;
        if (i < nHeld_ && held_[i] == note) return;          // already held (a set)
        if (nHeld_ >= kMaxHeld) return;
        for (int j = nHeld_; j > i; j--) held_[j] = held_[j - 1];
        held_[i] = note; nHeld_++;
    }
    void noteOff(int note) {
        for (int i = 0; i < nHeld_; i++) if (held_[i] == note) { for (int j = i; j + 1 < nHeld_; j++) held_[j] = held_[j + 1]; nHeld_--; return; }
    }
    void clear(ArpEvList& out) { nHeld_ = 0; if (playing_ >= 0) { out.push(0, playing_, 0); playing_ = -1; } }

    // advance `n` samples; events are appended with their offset in the block (`base` is added to every offset)
    void run(int n, double bpm, int sr, ArpEvList& out, int base = 0) {
        const double stepLen = beats_ * 60.0 / std::max(20.0, bpm) * sr;
        for (int i = 0; i < n; i++) {
            if (playing_ >= 0 && gateLeft_-- <= 0) { out.push(base + i, playing_, 0); playing_ = -1; }
            if (clock_ >= stepLen) {
                clock_ -= stepLen; if (clock_ >= stepLen) clock_ = 0;
                if (playing_ >= 0) { out.push(base + i, playing_, 0); playing_ = -1; }
                if (nHeld_ > 0) {
                    playing_ = nextNote();
                    out.push(base + i, playing_, 100);
                    gateLeft_ = (int)(stepLen * gate_);
                }
            }
            clock_ += 1.0;
        }
        if (nHeld_ == 0 && playing_ >= 0) { out.push(base + (n > 0 ? n - 1 : 0), playing_, 0); playing_ = -1; }
    }
private:
    int nextNote() {
        const int N = nHeld_ * octaves_;   // ascending held notes, octave after octave
        int idx = 0;
        switch (pattern_) {
            case UP: idx = step_ % N; break;
            case DOWN: idx = N - 1 - (step_ % N); break;
            case UPDOWN: { const int period = std::max(1, 2 * N - 2), k = step_ % period; idx = k < N ? k : period - k; break; }
            case CONVERGE: { const int k = step_ % N; idx = (k % 2 == 0) ? k / 2 : N - 1 - k / 2; break; }
            default: rng_ = rng_ * 1664525u + 1013904223u; idx = (int)((rng_ >> 8) % (uint32_t)N); break;
        }
        step_++;
        return std::min(127, held_[idx % nHeld_] + 12 * (idx / nHeld_));
    }
    float beats_ = 0.25f, gate_ = 0.7f; int pattern_ = 0, octaves_ = 1, step_ = 0, playing_ = -1, gateLeft_ = 0;
    double clock_ = 0; uint32_t rng_ = 99;
    int held_[kMaxHeld] = {}; int nHeld_ = 0;
};
