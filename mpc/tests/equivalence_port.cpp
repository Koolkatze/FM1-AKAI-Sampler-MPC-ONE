// The MPC port (mpc/FM1Sampler/src/fm1/Sampler) rendering the scenario of equivalence_ref.cpp with the same sample and slices.
#include "fm1/Sampler.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    int mode = atoi(argv[1]); const char* out = argv[2]; const char* dump = argv[3];
    FILE* f = fopen(dump, "rb"); int hdr[2]; fread(hdr, 4, 2, f);
    SampleBuf b; b.frames = hdr[0]; b.sr = hdr[1]; b.d.resize((size_t)b.frames * 2); fread(b.d.data(), 4, b.d.size(), f);
    int n; fread(&n, 4, 1, f); std::vector<float> fr((size_t)n); fread(fr.data(), 4, n, f); fclose(f);
    std::vector<int> st; for (int i = 0; i + 1 < n; i++) st.push_back((int)std::lround(fr[i] * b.frames));
    Sampler s(44100); PaulStream* none[8] = {}; s.setPaulStreams(none);
    s.setSource(&b); s.setSlices(st.data(), (int)st.size(), fr.back() * b.frames); s.setKitBase(60);
    s.setMode(mode); s.setEnv(0.03f, 0.35f, 0.8f, 0.5f); s.setGrain(7, 1.f); s.setRange(0.f, 1.f);
    std::vector<float> buf; float blk[128]; int notes[3] = {60, 64, 67};
    for (int k = 0; k < 44100 * 3 / 64; k++) {
        if (k == 0) for (int j = 0; j < (mode == 3 ? 1 : 3); j++) s.noteOn(notes[j], 100);
        if (mode == 3 && k == 300) s.noteOn(61, 100);
        if (k == 44100 * 2 / 64) for (int j = 0; j < 3; j++) s.noteOff(notes[j]);
        memset(blk, 0, sizeof blk); s.render(blk, 64); buf.insert(buf.end(), blk, blk + 128);
    }
    f = fopen(out, "wb"); fwrite(buf.data(), 4, buf.size(), f); fclose(f);
    return 0;
}
