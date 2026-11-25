// Fidelity reference: the phone's engine/dsp/Sampler of FM1 Sampler 0.42.7, unmodified (copied in mpc/tests/reference/), at 44.1 kHz.
// Renders scenario.h and dumps its normalised sample + slices so equivalence_port.cpp can render the same with the MPC port.
//   equivalence_ref <wav> <mode> <poly 0|1> <out.raw> <dump.bin>
#include "Sampler.h"
#include "scenario.h"
#include <cstdio>
#include <cstdlib>
int main(int, char** argv) {
    const char* wav = argv[1]; const int mode = atoi(argv[2]), poly = atoi(argv[3]); const char* out = argv[4]; const char* dump = argv[5];
    Sampler s(44100);
    if (!s.load(wav)) { printf("load failed\n"); return 1; }
    auto o = s.original();
    FILE* f = fopen(dump, "wb"); int hdr[2] = {o->frames, o->sr}; fwrite(hdr, 4, 2, f); fwrite(o->d.data(), 4, o->d.size(), f);
    std::vector<float> fr = s.sliceFractions(); int n = (int)fr.size(); fwrite(&n, 4, 1, f); fwrite(fr.data(), 4, n, f); fclose(f);
    std::vector<float> buf = renderScenario(s, mode, poly);
    f = fopen(out, "wb"); fwrite(buf.data(), 4, buf.size(), f); fclose(f);
    return 0;
}
