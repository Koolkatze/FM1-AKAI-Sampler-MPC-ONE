// The MPC port (mpc/FM1Sampler/src/fm1/Sampler) rendering scenario.h with the reference's normalised sample and slices.
//   equivalence_port <mode> <poly 0|1> <out.raw> <dump.bin>
#include "fm1/Sampler.h"
#include "scenario.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
int main(int, char** argv) {
    const int mode = atoi(argv[1]), poly = atoi(argv[2]); const char* out = argv[3]; const char* dump = argv[4];
    FILE* f = fopen(dump, "rb"); int hdr[2]; if (fread(hdr, 4, 2, f) != 2) return 1;
    SampleBuf b; b.frames = hdr[0]; b.sr = hdr[1]; b.d.resize((size_t)b.frames * 2); if (fread(b.d.data(), 4, b.d.size(), f) != b.d.size()) return 1;
    int n = 0; if (fread(&n, 4, 1, f) != 1) return 1;
    std::vector<float> fr((size_t)n); if (fread(fr.data(), 4, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    std::vector<int> st; for (int i = 0; i + 1 < n; i++) st.push_back((int)std::lround(fr[(size_t)i] * b.frames));
    Sampler s(44100); PaulStream* none[Sampler::kPaulVoices] = {}; s.setPaulStreams(none);
    s.setSource(&b); s.setSlices(st.data(), (int)st.size(), fr.back() * b.frames); s.setKitBase(60);
    std::vector<float> buf = renderScenario(s, mode, poly);
    f = fopen(out, "wb"); fwrite(buf.data(), 4, buf.size(), f); fclose(f);
    return 0;
}
