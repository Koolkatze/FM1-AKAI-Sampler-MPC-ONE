// fm1_test.cpp - tests and CPU measurements of the FM1 Sampler plugin through a minimal VST2 host that calls it like
// the MPC does (same ABI header as the SDK). Uses the native build made by the SDK's build.py.
//
//   c++ -std=c++17 -O2 -I<sdk>/framework mpc/tests/fm1_test.cpp -ldl -lpthread -o /tmp/fm1_test
//   FM1_SAMPLES_ROOT=/tmp/fm1root /tmp/fm1_test <sdk>/plugins/FM1Sampler/build/native/fm1-sampler.so [wav-out-dir]
//
// Every scenario selects one of the plugin's programs, waits for the worker thread to load the sample, plays notes
// and reports the output level and the time spent per 128-frame block. Desktop timings are only a guide: the SDK
// suggests multiplying them by 15-25 for the Cortex-A17 of an MPC One.
#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "mpc_vst2_abi.h"

namespace {
double g_bpm = 120.0; bool g_playing = false; double g_samplePos = 0; int g_automate = 0;
VstTimeInfo g_ti;
VstIntPtr hostCb(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void* ptr, float) {
    switch (op) {
        case audioMasterVersion: return 2400;
        case audioMasterGetTime:
            std::memset(&g_ti, 0, sizeof g_ti); g_ti.sampleRate = 44100; g_ti.samplePos = g_samplePos; g_ti.tempo = g_bpm;
            g_ti.ppqPos = g_samplePos / 44100.0 * g_bpm / 60.0; g_ti.timeSigNumerator = 4; g_ti.timeSigDenominator = 4;
            g_ti.flags = kVstTempoValid | kVstPpqPosValid | kVstTimeSigValid | (g_playing ? kVstTransportPlaying : 0);
            return (VstIntPtr)&g_ti;
        case audioMasterAutomate: ++g_automate; return 0;
        case audioMasterGetSampleRate: return 44100;
        case audioMasterGetBlockSize: return 128;
        case audioMasterGetVendorString: if (ptr) std::strcpy((char*)ptr, "Akai"); return 1;
        case audioMasterGetProductString: if (ptr) std::strcpy((char*)ptr, "MPC 3"); return 1;
        default: return 0;
    }
}

int g_fail = 0;
void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++g_fail; }

struct Plug {
    void* lib = nullptr; AEffect* e = nullptr;
    explicit Plug(const char* path) {
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!lib) { std::printf("dlopen: %s\n", dlerror()); return; }
        auto fn = (AEffect* (*)(audioMasterCallback))dlsym(lib, "VSTPluginMain");
        e = fn ? fn(hostCb) : nullptr;
        if (!e) return;
        d(effSetSampleRate, 0, 0, nullptr, 44100.f); d(effSetBlockSize, 0, 128); d(effOpen);
        d(effMainsChanged, 0, 1); d(effStartProcess);
    }
    ~Plug() { if (e) { d(effStopProcess); d(effMainsChanged, 0, 0); d(effClose); } }   // the library stays loaded (static data of other instances)
    VstIntPtr d(VstInt32 op, VstInt32 idx = 0, VstIntPtr v = 0, void* p = nullptr, float f = 0.f) { return e->dispatcher(e, op, idx, v, p, f); }
    void program(int i) { d(effSetProgram, 0, i); }
    void setParam(int i, float norm) { e->setParameter(e, i, norm); }
    float getParam(int i) { return e->getParameter(e, i); }
    std::string display(int i) { char b[256] = {}; d(effGetParamDisplay, i, 0, b); return b; }
    std::string programName(int i) { char b[256] = {}; d(effGetProgramNameIndexed, i, 0, b); return b; }
    std::vector<uint8_t> chunk() { void* p = nullptr; const VstIntPtr n = d(effGetChunk, 0, 0, &p); return std::vector<uint8_t>((uint8_t*)p, (uint8_t*)p + n); }
    void setChunk(std::vector<uint8_t> c) { d(effSetChunk, 0, (VstIntPtr)c.size(), c.data()); }
};

struct Ev { int frame; uint8_t s, a, b; };
struct Result { double rms = 0, peak = 0, avgUs = 0, maxUs = 0; bool finite = true; std::vector<float> l, r; };

bool g_realtime = false;   // pace the render like the MPC's audio clock (needed by the Paul producer thread)
Result render(Plug& p, double seconds, std::vector<Ev> evs, int block = 128) {
    Result res;
    const auto start = std::chrono::steady_clock::now();
    const int total = (int)(seconds * 44100);
    std::vector<float> L((size_t)block), R((size_t)block);
    std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
    size_t ei = 0; double sum = 0, tsum = 0; int blocks = 0;
    for (int pos = 0; pos < total; pos += block) {
        const int n = std::min(block, total - pos);
        std::vector<VstMidiEvent> me;
        while (ei < evs.size() && evs[ei].frame < pos + n) {
            VstMidiEvent m{}; m.type = kVstMidiType; m.byteSize = sizeof m; m.deltaFrames = evs[ei].frame - pos;
            m.midiData[0] = (char)evs[ei].s; m.midiData[1] = (char)evs[ei].a; m.midiData[2] = (char)evs[ei].b; me.push_back(m); ++ei;
        }
        std::vector<char> buf(sizeof(VstEvents) + sizeof(VstEvent*) * (me.size() + 2));
        VstEvents* ve = (VstEvents*)buf.data(); ve->numEvents = (VstInt32)me.size();
        for (size_t i = 0; i < me.size(); i++) ve->events[i] = (VstEvent*)&me[i];
        p.d(effProcessEvents, 0, 0, ve);
        float* outs[2] = {L.data(), R.data()};
        const auto t0 = std::chrono::steady_clock::now();
        p.e->processReplacing(p.e, outs, outs, n);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        tsum += us; res.maxUs = std::max(res.maxUs, us); ++blocks;
        g_samplePos += n;
        if (g_realtime) std::this_thread::sleep_until(start + std::chrono::microseconds((long long)((pos + n) * 1e6 / 44100.0)));
        for (int i = 0; i < n; i++) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) res.finite = false;
            sum += (double)L[i] * L[i] + (double)R[i] * R[i]; res.peak = std::max(res.peak, (double)std::max(std::fabs(L[i]), std::fabs(R[i])));
            res.l.push_back(L[i]); res.r.push_back(R[i]);
        }
    }
    res.rms = std::sqrt(sum / (2.0 * std::max(1, total)));
    res.avgUs = tsum / std::max(1, blocks);
    return res;
}
double db(double v) { return 20.0 * std::log10(std::max(1e-10, v)); }
void writeWav(const std::string& path, const Result& r) {
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    const uint32_t n = (uint32_t)r.l.size(), bytes = n * 4, riff = 36 + bytes, sr = 44100, br = sr * 4, sixteen = 16; const uint16_t fmt = 1, ch = 2, ba = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&sixteen, 4, 1, f);
    std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&sr, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
    for (uint32_t i = 0; i < n; i++) { int16_t a = (int16_t)std::lrint(std::clamp(r.l[i], -1.f, 1.f) * 32767), b = (int16_t)std::lrint(std::clamp(r.r[i], -1.f, 1.f) * 32767); std::fwrite(&a, 2, 1, f); std::fwrite(&b, 2, 1, f); }
    std::fclose(f);
}
std::vector<Ev> chord(std::initializer_list<int> notes, int on, int off, int vel = 100) {
    std::vector<Ev> v; for (int n : notes) { v.push_back({on, 0x90, (uint8_t)n, (uint8_t)vel}); if (off > on) v.push_back({off, 0x80, (uint8_t)n, 0}); } return v;
}
void writeSine(const std::string& path, double hz) {
    Result r; for (int i = 0; i < 44100; i++) { const float v = 0.5f * (float)std::sin(6.283185307 * hz * i / 44100.0); r.l.push_back(v); r.r.push_back(v); }
    writeWav(path, r);
}
void wait(double ms) { std::this_thread::sleep_for(std::chrono::milliseconds((int)ms)); }
int findProgram(Plug& p, const char* name) { for (int i = 0; i < p.e->numPrograms; i++) if (p.programName(i) == name) return i; return -1; }
// zero crossings of the left channel between two times -> rough fundamental, for pitch checks
double zcHz(const Result& r, double t0, double t1) {
    int c = 0; const size_t a = (size_t)(t0 * 44100), b = std::min(r.l.size(), (size_t)(t1 * 44100));
    for (size_t i = a + 1; i < b; i++) if ((r.l[i - 1] < 0) != (r.l[i] < 0)) c++;
    return c / 2.0 / (t1 - t0);
}
// seconds until the level falls below -50 dB for good (end of the sound)
double soundLength(const Result& r) {
    const size_t win = 441; size_t last = 0;
    for (size_t s = 0; s + win <= r.l.size(); s += win) { double e = 0; for (size_t i = s; i < s + win; i++) e += r.l[i] * r.l[i]; if (db(std::sqrt(e / win)) > -50) last = s + win; }
    return last / 44100.0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: fm1_test <plugin.so> [wav-dir]\n"); return 1; }
    const std::string wavDir = argc > 2 ? argv[2] : "";
    enum { kVolume, kFolder, kSample, kAlgo, kStart, kEnd, kSpeed, kPitch, kThreshold, kKitNote, kGrains, kScatter, kStretch, kWidth, kRoot, kAutoRoot,
           kAttack, kDecay, kSustain, kRelease, kLfoRate, kLfoDepth, kLfoTarget, kLfoShape, kArpRate, kArpPattern, kArpOctaves, kArpLength,
           kExciter, kLofi, kWobble, kRevSend, kRevSize, kDelSend, kDelTime, kDelSync, kDelFeedback, kPan, kScale, kKeyRoot, kOctave, kKeyMode, kBendRange, kFm1Ctrl };

    if (const char* root = std::getenv("FM1_SAMPLES_ROOT")) {   // a fake drive: FM1 Samples and a LooperX loop
        const std::string r = root;
        std::system(("rm -rf '" + r + "' && mkdir -p '" + r + "/FM1 Samples' '" + r + "/LooperX/Loops'").c_str());
        writeSine(r + "/FM1 Samples/b_sine.wav", 440.0);
        writeSine(r + "/LooperX/Loops/LooperX_Loop01.wav", 330.0);
    }
    Plug p(argv[1]);
    if (!p.e) { std::printf("FAIL  could not load the plugin\n"); return 2; }
    check(p.e->numParams == 44, "44 parameters");
    { std::string s; for (int f = 0; f < 4; f++) { p.setParam(kFolder, f / 99.f); s += p.display(kFolder) + (f < 3 ? " | " : ""); } std::printf("INFO  folders: %s\n", s.c_str()); }

    // ---- scenarios: every algorithm through the built-in programs ----
    struct Sc { const char* program; std::vector<Ev> evs; double seconds; };
    std::vector<Ev> kit; for (int i = 0; i < 16; i++) { kit.push_back({i * 5512, 0x90, (uint8_t)(36 + i), 110}); kit.push_back({i * 5512 + 3000, 0x80, (uint8_t)(36 + i), 0}); }
    std::vector<Ev> eight = chord({48, 52, 55, 60, 64, 67, 71, 72}, 0, 3 * 44100);
    std::vector<Ev> paul4 = chord({60, 67, 72, 79}, 0, 4 * 44100);
    std::vector<Sc> sc = {
        {"Tape Key", eight, 4.5}, {"Trap Kit", kit, 2.5}, {"Glow Grains", eight, 4.5}, {"Light Paul", paul4, 5.0},
        {"Classic Loop", chord({60, 64, 67}, 0, 3 * 44100), 4.0}, {"Koto Arp", chord({60, 64, 67}, 0, 3 * 44100), 4.0}, {"Fuzz Lo-Fi", eight, 4.0},
    };
    std::printf("\n%-14s %8s %8s %10s %10s %14s\n", "program", "rms dB", "peak dB", "avg us", "max us", "est. A17 avg");
    for (auto& s : sc) {
        const int pi = findProgram(p, s.program);
        check(pi >= 0, (std::string("program ") + s.program).c_str());
        if (pi < 0) continue;
        p.program(pi); wait(700);   // the worker loads the sample (and the stretch, if any)
        g_realtime = std::string(s.program) == "Light Paul";   // its spectra come from the producer thread at the audio clock
        Result r = render(p, s.seconds, s.evs);
        g_realtime = false;
        std::printf("%-14s %8.1f %8.1f %10.1f %10.1f %12.1f %%\n", s.program, db(r.rms), db(r.peak), r.avgUs, r.maxUs, r.avgUs / 2902.0 * 100.0 * 20.0);
        check(r.finite, "  finite output");
        check(db(r.rms) > -40.0, "  audible output");
        check(r.peak < 1.0, "  no clipping (master soft clip)");
        if (!wavDir.empty()) writeWav(wavDir + "/" + s.program + ".wav", r);
        render(p, 2.0, {{0, 0xB0, 120, 0}});   // all sound off and let the tails go
    }

    // ---- idle cost: no notes, tails finished ----
    { p.program(0); wait(500); render(p, 12.0, {}); Result r = render(p, 2.0, {});
      std::printf("INFO  idle (silent, tails done): avg %.1f us per block\n", r.avgUs); check(r.rms < 1e-6, "idle is silent"); }

    // ---- pitch: MELODIC follows the note (root 60), SPEED 0.5 doubles the length (offline stretch) ----
    {
        const int pi = findProgram(p, "Tape Key"); p.program(pi);
        p.setParam(kRevSend, 0); p.setParam(kDelSend, 0); p.setParam(kRelease, 0.1f); wait(600);
        Result a = render(p, 3.0, chord({60}, 0, 44100)), b = render(p, 3.0, chord({72}, 0, 44100));
        const double ha = zcHz(a, 0.2, 0.8), hb = zcHz(b, 0.2, 0.8);
        std::printf("INFO  zero-crossing rate: note 60 %.1f Hz, note 72 %.1f Hz (ratio %.2f)\n", ha, hb, hb / std::max(1.0, ha));
        check(hb / std::max(1.0, ha) > 1.8 && hb / std::max(1.0, ha) < 2.2, "an octave up plays twice as fast");
        p.setParam(kAlgo, 1 / 5.f);   // ONE-SHOT: the whole window
        p.setParam(kEnd, 0.3f);       // a 2 s window, so that twice as long still fits in the render
        wait(400);
        Result n1 = render(p, 8.0, chord({60}, 0, 100));
        p.setParam(kSpeed, 0.25f);   // log 0.25..4: 0.25 -> 0.5x
        wait(1500);
        std::printf("INFO  speed display: %s\n", p.display(kSpeed).c_str());
        Result n2 = render(p, 8.0, chord({60}, 0, 100));
        std::printf("INFO  one-shot length: speed 1.00x %.2f s, speed 0.50x %.2f s\n", soundLength(n1), soundLength(n2));
        check(soundLength(n2) > 1.7 * soundLength(n1), "SPEED 0.5x makes the sample about twice as long (offline WSOLA)");
        p.setParam(kSpeed, 0.5f); p.setParam(kAlgo, 0.f); p.setParam(kEnd, 1.f); wait(600);
    }

    // ---- state: a project restores its sample ----
    {
        p.program(findProgram(p, "Phonk Bass")); wait(700);
        const std::vector<uint8_t> c = p.chunk();
        Plug q(argv[1]);
        q.setChunk(c); wait(800);
        std::printf("INFO  restored: %s / %s\n", q.display(kFolder).c_str(), q.display(kSample).c_str());
        check(q.display(kSample).find("Phonk") != std::string::npos, "a project restores its sample");
        Result r = render(q, 1.0, chord({48}, 0, 30000));
        check(db(r.rms) > -40.0, "restored instance plays");
    }

    // ---- user folders (FM1 Samples, LooperX loops) and restore by path when the folder changed ----
    if (const char* root = std::getenv("FM1_SAMPLES_ROOT")) {
        int userFolder = -1; bool loops = false;
        for (int f = 1; f <= 12; f++) {
            p.setParam(kFolder, (f - 1) / 99.f); const std::string s = p.display(kFolder);
            if (s.find("PC:FM1") != std::string::npos && userFolder < 0) userFolder = f;
            if (s.find("LX Loops") != std::string::npos) loops = true;
        }
        check(userFolder > 0, "user folder 'FM1 Samples' listed");
        check(loops, "LooperX/Loops listed");
        if (userFolder > 0) {
            p.setParam(kFolder, (userFolder - 1) / 99.f); p.setParam(kSample, 0.f); wait(700);
            std::printf("INFO  user sample: %s\n", p.display(kSample).c_str());
            const std::vector<uint8_t> c = p.chunk();
            writeSine(std::string(root) + "/FM1 Samples/a_first.wav", 220.0);   // now sorts before the chosen file
            Plug q(argv[1]);
            q.setChunk(c); wait(800);
            std::printf("INFO  restored after a new file was added: %s\n", q.display(kSample).c_str());
            check(q.display(kSample).find("b_sine") != std::string::npos, "the project finds its sample by path, not by number");
        }
    }

    // ---- KIT: 16 consecutive notes from Kit Note play 16 different slices ----
    {
        p.program(findProgram(p, "Trap Kit")); p.setParam(kRevSend, 0); p.setParam(kDelSend, 0); wait(700);
        double e0 = 0, e1 = 0;
        Result a = render(p, 0.5, chord({36}, 0, 10000)); render(p, 0.5, {{0, 0xB0, 120, 0}});
        Result b = render(p, 0.5, chord({37}, 0, 10000)); render(p, 0.5, {{0, 0xB0, 120, 0}});
        for (size_t i = 0; i < 4000; i++) { e0 += a.l[i] * a.l[i]; e1 += (a.l[i] - b.l[i]) * (a.l[i] - b.l[i]); }
        check(e1 > 0.1 * e0, "pads 1 and 2 play different slices");
    }

    // ---- FM-1 control surface: encoders move parameters and the host is told ----
    {
        p.program(0); wait(300);
        p.setParam(kFm1Ctrl, 1.f);
        const float before = p.getParam(kStart);
        const int automateBefore = g_automate;
        render(p, 0.1, {{0, 0x90, 8, 100}, {10, 0xB0, 19, 70}});   // HOME, then K1 (START) +6
        wait(100);
        check(p.getParam(kStart) > before, "FM-1 K1 on HOME moves START");
        check(g_automate > automateBefore, "the host is told (audioMasterAutomate from the worker)");
        Result r = render(p, 0.2, {{0, 0x90, 5, 100}});   // a button note must not sound
        check(r.rms < 0.05, "FM-1 button notes do not play");
        p.setParam(kFm1Ctrl, 0.f);
    }

    // ---- block sizes the MPC can use ----
    for (int bs : {32, 96, 512}) { p.program(0); wait(300); Result r = render(p, 1.0, chord({60, 64}, 0, 30000), bs); char t[64]; std::snprintf(t, sizeof t, "block size %d", bs); check(r.finite && db(r.rms) > -40, t); }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
