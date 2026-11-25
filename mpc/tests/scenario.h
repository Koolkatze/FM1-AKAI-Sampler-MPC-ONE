// The scenario both fidelity programs render (equivalence_ref.cpp with the phone engine, equivalence_port.cpp with the port).
//   poly = 0: three notes (one hit and a second one in KIT), held two seconds then released
//   poly = 1: polyphony and voice stealing of 0.42.7 - 70 notes in MELODIC/ONE-SHOT/LOOPED with a 5 s release (more than the
//             64 voices, so the oldest are stolen), 40 overlapping hits in KIT (32), 20 notes in GRANULAR (16)
#pragma once
#include <cstring>
#include <vector>
template <class S> std::vector<float> renderScenario(S& s, int mode, int poly) {
    s.setMode(mode); s.setGrain(7, 1.f); s.setRange(0.f, 1.f);
    if (poly) s.setEnv(0.002f, 0.1f, 0.8f, 5.f); else s.setEnv(0.03f, 0.35f, 0.8f, 0.5f);
    const int count = mode == 3 ? 40 : mode == 4 ? 20 : 70;
    std::vector<float> buf; float blk[128]; int notes[3] = {60, 64, 67};
    for (int b = 0; b < 44100 * 3 / 64; b++) {
        if (!poly) {
            if (b == 0) for (int k = 0; k < (mode == 3 ? 1 : 3); k++) s.noteOn(notes[k], 100);
            if (mode == 3 && b == 300) s.noteOn(61, 100);
            if (b == 44100 * 2 / 64) for (int k = 0; k < 3; k++) s.noteOff(notes[k]);
        } else if (b < count * 4 && b % 4 == 0) {
            const int note = 40 + (b / 4 * 7) % 48;
            s.noteOn(note, 60 + (b / 4) % 60);
            if (b % 8 == 0) s.noteOff(note);   // half released at once, half held
        }
        std::memset(blk, 0, sizeof blk); s.render(blk, 64); buf.insert(buf.end(), blk, blk + 128);
    }
    return buf;
}
