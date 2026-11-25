// Tonality of the FM1: scale, root, octave and KEY / CHORD mode (the GLO page of the FM-1).
//
// On the FM-1 the firmware turns key k into scale degree k; on the MPC the pads and keys already send chromatic notes,
// so a note outside the scale is moved to the nearest degree (down on a tie), then shifted by the octave. CHORD mode
// plays the triad of the degree (d, d + 2, d + 4), as the FM-1 firmware does. CHROM keeps every note as it is.
// Scale tables: engine/app/SamplerApp.cpp (kScaleN, kScaleStep). Realtime safe: no allocation.
#pragma once
#include <algorithm>

class KeyMap {
public:
    static constexpr int kScales = 7;   // CHROM MIX MAJ MIN HMIN PHRYG MIN+
    void set(int scale, int root, int octave, bool chord) {
        scale_ = std::min(kScales - 1, std::max(0, scale)); root_ = ((root % 12) + 12) % 12; octave_ = std::min(3, std::max(-3, octave)); chord_ = chord;
    }
    // maps one played note to up to three notes; returns how many
    int map(int note, int out[3]) const {
        const int n = kN[scale_];
        int pc = ((note - root_) % 12 + 12) % 12, off = 0;
        if (index(pc) < 0) for (int k = 1; k < 7; k++) { if (index((pc - k + 12) % 12) >= 0) { off = -k; break; } if (index((pc + k) % 12) >= 0) { off = k; break; } }
        const int q = note + off;
        const int rel = q - root_, o = rel >= 0 ? rel / 12 : -((-rel + 11) / 12);
        const int d = o * n + index(((rel % 12) + 12) % 12);
        int count = 0;
        const int degs = chord_ ? 3 : 1;
        for (int i = 0; i < degs; i++) {
            const int m = degreeNote(d + 2 * i) + 12 * octave_;
            if (m >= 0 && m <= 127) out[count++] = m;
        }
        return count;
    }
private:
    int index(int pc) const { for (int i = 0; i < kN[scale_]; i++) if (kStep[scale_][i] == pc) return i; return -1; }
    int degreeNote(int d) const { const int n = kN[scale_], o = d >= 0 ? d / n : -((-d + n - 1) / n); return root_ + 12 * o + kStep[scale_][d - o * n]; }
    static constexpr int kN[7] = {12, 7, 7, 7, 7, 7, 8};
    static constexpr int kStep[7][12] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, {0, 2, 4, 5, 7, 9, 10}, {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10},
                                         {0, 2, 3, 5, 7, 8, 11}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 8, 10, 11}};
    int scale_ = 0, root_ = 0, octave_ = 0; bool chord_ = false;
};
