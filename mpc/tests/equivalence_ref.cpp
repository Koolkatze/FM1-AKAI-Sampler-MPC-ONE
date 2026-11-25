// Fidelity reference: the UNMODIFIED engine/dsp/Sampler of this repository (the phone build) at 44.1 kHz.
// Renders a scenario and dumps its normalised sample + slices so equivalence_port.cpp can render the same with the MPC port.
#include "Sampler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    const char* wav = argv[1]; int mode = atoi(argv[2]); const char* out = argv[3]; const char* dump = argv[4];
    Sampler s(44100);
    if (!s.load(wav)) { printf("load failed\n"); return 1; }
    auto o = s.original();
    FILE* f = fopen(dump, "wb"); int hdr[2] = {o->frames, o->sr}; fwrite(hdr, 4, 2, f); fwrite(o->d.data(), 4, o->d.size(), f);
    std::vector<float> fr = s.sliceFractions(); int n = (int)fr.size(); fwrite(&n, 4, 1, f); fwrite(fr.data(), 4, n, f); fclose(f);
    s.setMode(mode); s.setEnv(0.03f, 0.35f, 0.8f, 0.5f); s.setGrain(7, 1.f); s.setRange(0.f, 1.f);
    std::vector<float> buf;
    float blk[128];
    int notes[3] = {60, 64, 67};
    for (int b = 0; b < 44100 * 3 / 64; b++) {
        if (b == 0) for (int k = 0; k < (mode == 3 ? 1 : 3); k++) s.noteOn(notes[k], 100);
        if (mode == 3 && b == 300) s.noteOn(61, 100);
        if (b == 44100 * 2 / 64) for (int k = 0; k < 3; k++) s.noteOff(notes[k]);
        memset(blk, 0, sizeof blk); s.render(blk, 64); buf.insert(buf.end(), blk, blk + 128);
    }
    f = fopen(out, "wb"); fwrite(buf.data(), 4, buf.size(), f); fclose(f);
    return 0;
}
