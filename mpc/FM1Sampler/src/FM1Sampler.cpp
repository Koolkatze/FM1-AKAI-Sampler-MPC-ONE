// FM1Sampler.cpp - the FM1 Sampler (engine/ of Koolkatze/fm1-sampler) as an Akai MPC instrument plugin.
//
// One instance = one FM1 track: a sample played by the FM1 sampler engine with its six algorithms (MELODIC, ONE-SHOT,
// LOOPED, KIT/SLICE, GRANULAR, PAULSTRETCH), its envelope, LFO, arpeggiator, scale / chord keyboard, tape colour and its
// two send effects. Several instances on several MPC tracks give the FM1's multi-track setup; the MPC itself provides
// what the Android app did around the engine (sequencer, looper tracks, scenes, song, metronome, sampling, mixer).
//
// Signal flow per sub-block of at most 64 frames (as engine/session/Session.cpp does per 256-frame block):
//   MIDI -> [FM-1 control surface, optional] -> KeyMap (scale / chord / octave) -> Arp (MPC tempo)
//   -> Sampler (8 voices) -> LFO low-pass (only while the LFO targets the filter) -> TapeInsert (exciter / lo-fi / wobble)
//   -> volume x LFO gain, constant-power pan -> out
//                                      \-> post-fader sends -> Reverb (return 0.8) + TapeDelay (return 0.7) -> out
//   -> master soft clip tanh(0.9 x)
//
// Threads (the MPC host contract: process() must never allocate, lock or touch files):
//   audio   process(): reads the parameters once per sub-block, takes finished objects from three mailboxes (sample,
//           stretched copy, slices) and hands the objects it no longer uses back through a lock-free ring.
//   worker  a thread owned by the plugin: scans the sample folders, loads and normalises WAVs, finds onsets and pitch,
//           cuts the KIT slices, runs the offline WSOLA stretch / pitch shift (SPEED and PITCH), deletes retired
//           objects (once the Paul producer no longer reads them) and tells the host about parameters changed from an FM-1.
//   paul    a second thread: computes the spectra of the PAULSTRETCH voices a few hops ahead (fm1/Paul.h).
//   UI      host calls: parameter names and value texts (the sample names come from the library under a mutex), state.
//
// Sample folders, in the order of the Folder parameter: the factory sounds of the FM1 (Content/Samples/<group>), then
// on the drive the plugin is installed on (and on the MPC's internal drive): "FM1 Samples" (and its sub-folders),
// "LooperX/Loops" (loops exported by the LooperX Looper) and "LooperX/Samples".
//
// Origin and licences: see mpc/README.md (engine: Koolkatze/fm1-sampler, a fork of ferluht/fm1-sampler; kissfft BSD-3-Clause).

#include "mpc_plugin.h"

#include "fm1/ArpRT.h"
#include "fm1/Common.h"
#include "fm1/Filter.h"
#include "fm1/KeyMap.h"
#include "fm1/Pitch.h"
#include "fm1/Reverb.h"
#include "fm1/Sampler.h"
#include "fm1/Slicer.h"
#include "fm1/Stretch.h"
#include "fm1/TapeDelay.h"
#include "fm1/TapeInsert.h"
#include "fm1/Wav.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Objects made by the worker and used by the audio thread. The audio thread never deletes them: it pushes them on the
// retire ring and the worker deletes them.
// ---------------------------------------------------------------------------------------------------------------------
struct Retirable { virtual ~Retirable() = default; };

struct SampleCore {                         // shared by the worker (slicing, stretching) and the published SampleSet
    SampleBuf buf;
    std::vector<Onset> onsets;
    std::string path, label;
    float detectedRoot = -1.f;              // MIDI note (with cents) found by YIN, -1 = no clear pitch
};
struct SampleSet : Retirable {
    std::shared_ptr<const SampleCore> core;
    uint32_t id = 0;
    int slices[Sampler::kMaxSlices] = {}; int nSlices = 0; double sliceEnd = 0;
};
struct StretchSet : Retirable {
    SampleBuf buf; bool none = true; float a = 0, b = 1; uint32_t sampleId = 0;
};
struct SliceSet : Retirable {
    int starts[Sampler::kMaxSlices] = {}; int n = 0; double end = 0; uint32_t sampleId = 0;
};

template <class T> class Mailbox {          // single slot, newest wins: the worker posts, the audio thread takes
public:
    T* post(T* v) { return slot_.exchange(v, std::memory_order_acq_rel); }   // returns what was never taken (worker deletes it)
    T* take() { return slot_.exchange(nullptr, std::memory_order_acq_rel); }
    ~Mailbox() { delete slot_.load(); }
private:
    std::atomic<T*> slot_{nullptr};
};

template <class T, int N> class SpscRing {  // one producer, one consumer, lock free
public:
    bool push(T v) {
        const int w = w_.load(std::memory_order_relaxed), next = (w + 1) % N;
        if (next == r_.load(std::memory_order_acquire)) return false;
        buf_[w] = v; w_.store(next, std::memory_order_release); return true;
    }
    bool pop(T& v) {
        const int r = r_.load(std::memory_order_relaxed);
        if (r == w_.load(std::memory_order_acquire)) return false;
        v = buf_[r]; r_.store((r + 1) % N, std::memory_order_release); return true;
    }
    int space() const { const int w = w_.load(std::memory_order_acquire), r = r_.load(std::memory_order_acquire); return (r - w - 1 + N) % N; }
private:
    T buf_[N] = {};
    std::atomic<int> w_{0}, r_{0};
};

// ---------------------------------------------------------------------------------------------------------------------
// Sample library
// ---------------------------------------------------------------------------------------------------------------------
struct LibEntry { std::string label, path; };
struct LibFolder { std::string name; std::vector<LibEntry> files; };

bool isDir(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
bool isWav(const std::string& name) {
    if (name.size() < 5 || name[0] == '.') return false;   // also skips macOS "._" files
    std::string e = name.substr(name.size() - 4);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".wav";
}
bool lessNoCase(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
        [](char x, char y) { return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
}
void listDir(const std::string& dir, std::vector<std::string>& files, std::vector<std::string>& dirs) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == ".." || n.empty() || n[0] == '.') continue;
        const std::string full = dir + "/" + n;
        if (isDir(full)) dirs.push_back(n); else if (isWav(n)) files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end(), lessNoCase); std::sort(dirs.begin(), dirs.end(), lessNoCase);
}
std::string stem(const std::string& file) { const size_t dot = file.rfind('.'); return dot == std::string::npos ? file : file.substr(0, dot); }
void addFolder(std::vector<LibFolder>& lib, const std::string& name, const std::string& dir, const std::vector<std::string>& files, const char* strip = nullptr) {
    if (files.empty()) return;
    LibFolder f; f.name = name;
    for (const auto& n : files) {
        std::string label = stem(n);
        if (strip && label.rfind(strip, 0) == 0) label = label.substr(std::strlen(strip));
        f.files.push_back({label, dir + "/" + n});
    }
    lib.push_back(std::move(f));
}
// a folder with WAVs at its root ("<tag>") and in its sub-folders ("<tag>/<sub>"), one level deep
void addTree(std::vector<LibFolder>& lib, const std::string& tag, const std::string& root, const char* strip = nullptr) {
    if (!isDir(root)) return;
    std::vector<std::string> files, dirs; listDir(root, files, dirs);
    addFolder(lib, tag, root, files, strip);
    for (const auto& sub : dirs) { std::vector<std::string> f2, d2; listDir(root + "/" + sub, f2, d2); addFolder(lib, tag + "/" + sub, root + "/" + sub, f2, strip); }
}
// the drive the plugin is installed on: ".../<drive>/VST2 Plugins/<this plugin>"
std::string pluginDriveRoot() {
    const std::string f = mpc::pluginFolder();
    const std::string parent = f.substr(0, f.find_last_of('/'));
    if (parent.size() >= 12 && parent.compare(parent.size() - 12, 12, "VST2 Plugins") == 0) return parent.substr(0, parent.find_last_of('/'));
    return "";
}
// the MPC's internal drive: a mount under /media whose name contains "internal" (as LooperX finds it)
std::string internalDriveRoot() {
    std::ifstream m("/proc/mounts");
    std::string dev, mnt, rest;
    while (m >> dev >> mnt && std::getline(m, rest))
        if (mnt.rfind("/media/", 0) == 0 && mnt.find("internal") != std::string::npos) return mnt;
    if (isDir("/media/az01-internal")) return "/media/az01-internal";
    return "";
}
std::vector<LibFolder> scanLibrary() {
    std::vector<LibFolder> lib;
    const std::string factory = mpc::contentPath("Samples");
    std::vector<std::string> files, dirs; listDir(factory, files, dirs);
    addFolder(lib, "FM1", factory, files);
    for (const auto& g : dirs) { std::vector<std::string> f2, d2; listDir(factory + "/" + g, f2, d2); addFolder(lib, g, factory + "/" + g, f2); }
    auto drive = [&](const std::string& root, const char* tag) {
        addTree(lib, std::string(tag) + "FM1", root + "/FM1 Samples");
        addTree(lib, std::string(tag) + "LX Loops", root + "/LooperX/Loops", "LooperX_");
        addTree(lib, std::string(tag) + "LX Smp", root + "/LooperX/Samples");
    };
    if (const char* env = std::getenv("FM1_SAMPLES_ROOT")) drive(env, "PC:");   // tests on a computer
    const std::string d = pluginDriveRoot(), in = internalDriveRoot();
    if (!d.empty()) drive(d, "SD:");
    if (!in.empty() && in != d) drive(in, "INT:");
    return lib;
}

// MPC note names: 60 = C3 (as LooperX shows them)
std::string noteName(int n) {
    static const char* k[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char b[16]; std::snprintf(b, sizeof b, "%s%d", k[((n % 12) + 12) % 12], (n >= 0 ? n / 12 : (n - 11) / 12) - 2); return b;
}

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

constexpr int kSub = 64;                     // control-rate sub-block (LFO, filter, gains)
constexpr float kMaxSampleSeconds = 32.f;    // longer files are cut (memory: every instance has its own copy)
constexpr float kMaxStretchSeconds = 60.f;   // longest offline-stretched copy

}  // namespace

class FM1Sampler : public mpc::Plugin
{
public:
    // Parameter order and ids are permanent (projects store them). New parameters go at the end.
    enum Params {
        kVolume,                                                                          // 0: harmless when the host nudges it
        kFolder, kSample, kAlgo, kStart, kEnd, kSpeed, kPitch,                            // SAMPLE (FM1 HOME page)
        kThreshold, kKitNote, kGrains, kScatter, kStretch, kWidth, kRoot, kAutoRoot,      // per-algorithm knobs, tuning
        kAttack, kDecay, kSustain, kRelease,                                              // ENV
        kLfoRate, kLfoDepth, kLfoTarget, kLfoShape,                                       // LFO
        kArpRate, kArpPattern, kArpOctaves, kArpLength,                                   // ARP
        kExciter, kLofi, kWobble,                                                         // TAPE (FM1 SEQ page)
        kRevSend, kRevSize, kDelSend, kDelTime, kDelSync, kDelFeedback,                   // FX
        kPan, kScale, kKeyRoot, kOctave, kKeyMode, kBendRange, kFm1Ctrl,                  // KEY (FM1 GLO page) and setup
        kNumParams
    };
    enum Algo { A_MELODIC, A_ONESHOT, A_LOOPED, A_KIT, A_GRAIN, A_PAUL };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "FM1 Sampler";
        i.vendor = "FM1";
        i.uniqueId = MPC_FOURCC('F', 'm', '1', 'S');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        i.tailSamples = 44100 * 6;                      // release up to 5 s plus reverb / delay
        return i;
    }

    FM1Sampler() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("volume", "Volume", 0.f, 100.f, 80.f, "%", 0));
        addParam(intParam("folder", "Folder", 1, 100, 3));
        addParam(intParam("sample", "Sample", 1, 128, 6));
        addParam(choiceParam("algo", "Algorithm", {"Melodic", "One-Shot", "Looped", "Kit/Slice", "Granular", "Paulstr."}, 0));
        addParam(floatParam("start", "Start", 0.f, 100.f, 0.f, "%", 1));
        addParam(floatParam("end", "End", 0.f, 100.f, 100.f, "%", 1));
        addParam(logParam("speed", "Speed", 0.25f, 4.f, 1.f, "x", 2));
        addParam(intParam("pitch", "Pitch", -24, 24, 0, "st"));
        addParam(floatParam("threshold", "Threshold", 0.f, 100.f, 22.f, "%", 0));
        addParam(intParam("kitnote", "Kit Note", 0, 112, 36));
        addParam(intParam("grains", "Grains", 1, 12, 7));
        addParam(floatParam("scatter", "Scatter", 0.f, 100.f, 100.f, "%", 0));
        addParam(logParam("stretch", "Stretch", 2.f, 64.f, 8.f, "x", 0));
        addParam(floatParam("width", "Width", 0.f, 200.f, 100.f, "%", 0));
        addParam(intParam("root", "Root Note", 24, 96, 60));
        addParam(boolParam("autoroot", "Auto Root", false));
        addParam(floatParam("attack", "Attack", 0.f, 3.f, 0.03f, "s", 2, 0.5f));     // FM1: 3 v^2 s
        addParam(floatParam("decay", "Decay", 0.f, 1.f, 0.35f, "s", 2));
        addParam(floatParam("sustain", "Sustain", 0.f, 100.f, 80.f, "%", 0));
        addParam(floatParam("release", "Release", 0.f, 5.f, 2.f, "s", 2, 0.5f));    // FM1: 5 v^2 s
        addParam(logParam("lforate", "LFO Rate", 0.1f, 20.f, 0.49f, "Hz", 2));     // FM1: 0.1 * 200^v
        addParam(floatParam("lfodepth", "LFO Depth", 0.f, 100.f, 0.f, "%", 0));
        addParam(choiceParam("lfotarget", "LFO Target", {"Pitch", "Filter", "Volume", "Pan"}, 0));
        addParam(choiceParam("lfoshape", "LFO Shape", {"Sine", "Tri", "Square", "S&H"}, 0));
        addParam(choiceParam("arprate", "Arp Rate", {"Off", "1", "2", "2T", "4", "4T", "8", "8T", "16", "16T", "32", "32T", "64"}, 0));
        addParam(choiceParam("arppattern", "Arp Pattern", {"Up", "Down", "UpDn", "Conv", "Rand"}, 0));
        addParam(intParam("arpoctaves", "Arp Octaves", 1, 4, 1));
        addParam(floatParam("arplength", "Arp Length", 5.f, 95.f, 70.f, "%", 0));
        addParam(floatParam("exciter", "Exciter", 0.f, 100.f, 0.f, "%", 0));
        addParam(floatParam("lofi", "Lo-Fi", 0.f, 100.f, 0.f, "%", 0));
        addParam(floatParam("wobble", "Wobble", 0.f, 100.f, 0.f, "%", 0));
        addParam(floatParam("revsend", "Reverb Send", 0.f, 100.f, 30.f, "%", 0));
        addParam(floatParam("revsize", "Reverb Size", 0.f, 100.f, 70.f, "%", 0));
        addParam(floatParam("delsend", "Delay Send", 0.f, 100.f, 20.f, "%", 0));
        addParam(floatParam("deltime", "Delay Time", 0.05f, 1.5f, 0.63f, "s", 2));   // FM1: 0.05 + 1.45 v
        addParam(choiceParam("delsync", "Delay Sync", {"Free", "1/16", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/1"}, 0));
        addParam(floatParam("delfb", "Delay FB", 0.f, 95.f, 43.f, "%", 0));          // FM1: 0.45 * 0.95
        addParam(floatParam("pan", "Pan", -100.f, 100.f, 0.f, "", 0));
        addParam(choiceParam("scale", "Scale", {"Chrom", "Mixolyd", "Major", "Minor", "H.Minor", "Phrygian", "Minor+"}, 0));
        addParam(choiceParam("keyroot", "Key Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0));
        addParam(intParam("octave", "Octave", -3, 3, 0));
        addParam(choiceParam("keymode", "Key Mode", {"Key", "Chord"}, 0));
        addParam(intParam("bend", "Bend Range", 0, 24, 2, "st"));
        addParam(boolParam("fm1ctrl", "FM-1 Ctrl", false));

        // Built-in programs: the factory sounds of the FM1 (folders: 1 Bass, 2 Drums, 3 Keys, 4 Pads; files sorted by name)
        preset("Tape Key",       {{kFolder, 3}, {kSample, 6}});
        preset("Acoustic Bass",  {{kFolder, 1}, {kSample, 1}, {kRelease, 0.4f}, {kRevSend, 10}});
        preset("Shiny Keys",     {{kFolder, 3}, {kSample, 5}, {kRevSend, 45}});
        preset("Trap Kit",       {{kFolder, 2}, {kSample, 3}, {kAlgo, A_KIT}, {kAttack, 0.f}, {kSustain, 100}, {kRelease, 0.3f}, {kRevSend, 10}, {kDelSend, 0}});
        preset("707 Kit",        {{kFolder, 2}, {kSample, 1}, {kAlgo, A_KIT}, {kAttack, 0.f}, {kSustain, 100}, {kRelease, 0.3f}, {kRevSend, 15}, {kDelSend, 0}});
        preset("OP Drums",       {{kFolder, 2}, {kSample, 2}, {kAlgo, A_KIT}, {kAttack, 0.f}, {kSustain, 100}, {kRelease, 0.3f}, {kRevSend, 10}, {kDelSend, 0}});
        preset("Glow Grains",    {{kFolder, 3}, {kSample, 3}, {kAlgo, A_GRAIN}, {kAttack, 0.4f}, {kRevSend, 50}});
        preset("Light Paul",     {{kFolder, 4}, {kSample, 2}, {kAlgo, A_PAUL}, {kAttack, 0.6f}, {kRelease, 3.f}, {kRevSend, 40}});
        preset("Classic Loop",   {{kFolder, 4}, {kSample, 1}, {kAlgo, A_LOOPED}, {kAttack, 0.3f}});
        preset("Koto Arp",       {{kFolder, 3}, {kSample, 4}, {kArpRate, 8}, {kArpPattern, 2}, {kArpOctaves, 2}, {kDelSend, 35}, {kDelSync, 4}, {kRelease, 0.5f}});
        preset("Fuzz Lo-Fi",     {{kFolder, 1}, {kSample, 4}, {kLofi, 50}, {kWobble, 30}, {kRelease, 0.4f}});
        preset("Phonk Bass",     {{kFolder, 1}, {kSample, 5}, {kExciter, 20}, {kRelease, 0.3f}, {kRevSend, 5}});

        // The library is scanned here (the UI thread may do I/O) so the value texts are right from the start.
        { std::vector<LibFolder> lib = scanLibrary(); publishCounts(lib); std::lock_guard<std::mutex> l(libMtx_); lib_ = std::move(lib); lastScan_ = Clock::now(); }
        for (auto& s : paul_) s.reset(new PaulStream);
        createEngine(44100.0);
        // The default sound is loaded here too, so a new instance plays from its first block; a project or a preset
        // that names another sample replaces it from the worker.
        const std::string first = resolve(getInt(kFolder), getInt(kSample));
        if (!first.empty()) load(first);
        worker_ = std::thread([this] { workerLoop(); });
        paulThread_ = std::thread([this] { paulLoop(); });
    }

    ~FM1Sampler() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        if (paulThread_.joinable()) paulThread_.join();
        Retirable* r = nullptr;
        while (retire_.pop(r)) delete r;
        for (auto& p : pendingDelete_) delete p.obj;
        delete curSample_; delete curStretch_; delete curSlices_;
    }

    // ---------------------------------------------------------------------------------------------------------------
    void prepare(double sampleRate, int) override
    {
        if ((int)sampleRate != engineRate_) createEngine(sampleRate);   // the MPC is always 44.1 kHz
    }

    void reset() override
    {
        sampler_->allOff();
        revQuiet_ = delQuiet_ = 0;
        ArpEvList dummy; arp_.clear(dummy);
        std::memset(heldCount_, 0, sizeof heldCount_);
        lp_.reset(); filtOn_ = false;
    }

    // ---------------------------------------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        takeMailboxes();
        readSettings(ctx);

        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        const int n = ctx.numFrames;
        const double bpm = ctx.transport.tempoValid && ctx.transport.bpm > 1.0 ? ctx.transport.bpm : 120.0;
        int ev = 0;
        for (int s = 0; s < n; s += kSub) {
            const int m = std::min(kSub, n - s);
            evs_.clear();
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset < s + m) { handleMidi(ctx.midi[ev], ctx.midi[ev].sampleOffset - s); ++ev; }
            if (arpOn_) arp_.run(m, bpm, engineRate_, evs_);
            evs_.sortByOffset();
            renderSub(outL + s, outR + s, m);
        }
        while (ev < ctx.numMidiEvents) { evs_.clear(); handleMidi(ctx.midi[ev++], 0); for (int i = 0; i < evs_.n; i++) applyNote(evs_.ev[i]); }
    }

    // ---------------------------------------------------------------------------------------------------------------
    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[64];
        switch (index) {
            case kFolder: case kSample: {
                std::lock_guard<std::mutex> l(libMtx_);
                if (lib_.empty()) { out = "--"; return true; }
                const LibFolder& f = lib_[(size_t)(((int)std::lround(index == kFolder ? plain : get(kFolder)) - 1) % (int)lib_.size())];
                if (index == kFolder) { std::snprintf(b, sizeof b, "%d %s", (int)std::lround(plain), f.name.c_str()); out = b; return true; }
                if (f.files.empty()) { out = "--"; return true; }
                const LibEntry& e = f.files[(size_t)(((int)std::lround(plain) - 1) % (int)f.files.size())];
                std::snprintf(b, sizeof b, "%d %s", (int)std::lround(plain), e.label.c_str()); out = b; return true;
            }
            case kPitch: std::snprintf(b, sizeof b, "%+d st", (int)std::lround(plain)); out = b; return true;
            case kOctave: std::snprintf(b, sizeof b, "%+d", (int)std::lround(plain)); out = b; return true;
            case kKitNote: case kRoot: std::snprintf(b, sizeof b, "%s %d", noteName((int)std::lround(plain)).c_str(), (int)std::lround(plain)); out = b; return true;
            case kSpeed: std::snprintf(b, sizeof b, "%.2fx", plain); out = b; return true;
            case kStretch: std::snprintf(b, sizeof b, "%.0fx", plain); out = b; return true;
            case kPan: { const int p = (int)std::lround(plain); if (p == 0) out = "C"; else { std::snprintf(b, sizeof b, "%s%d", p < 0 ? "L" : "R", std::abs(p)); out = b; } return true; }
            default: return false;
        }
    }

    // State beyond the parameters: the path of the sample, so a project finds its sound even if folders change.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::string path; { std::lock_guard<std::mutex> l(stateMtx_); path = loadedPath_; }
        out.push_back(1);   // version
        const uint32_t len = (uint32_t)std::min<size_t>(path.size(), 4096);
        for (int i = 0; i < 4; i++) out.push_back((uint8_t)(len >> (8 * i)));
        out.insert(out.end(), path.begin(), path.begin() + len);
    }
    void loadExtraState(const uint8_t* d, size_t n) override
    {
        if (n < 5 || d[0] != 1) return;
        uint32_t len = 0; for (int i = 0; i < 4; i++) len |= (uint32_t)d[1 + i] << (8 * i);
        if (len == 0 || 5 + (size_t)len > n) return;
        std::lock_guard<std::mutex> l(stateMtx_);
        pendingPath_.assign((const char*)d + 5, len); pendingPathSet_ = true;
    }

private:
    // ---------------------------------------------------------------------------------------------------------------
    // Construction helpers
    // ---------------------------------------------------------------------------------------------------------------
    void preset(const char* name, std::initializer_list<std::pair<int, float>> overrides)
    {
        std::vector<float> v((size_t)numParams());
        for (int i = 0; i < numParams(); i++) v[(size_t)i] = param(i).def;
        for (const auto& o : overrides) v[(size_t)o.first] = o.second;
        addProgram(name, std::move(v));
    }
    void createEngine(double sr)
    {
        engineRate_ = (int)sr;
        if (sampler_) sampler_->allOff();
        sampler_.reset(new Sampler(engineRate_));
        PaulStream* streams[Sampler::kVoices];
        for (int i = 0; i < Sampler::kVoices; i++) streams[i] = paul_[(size_t)i].get();
        sampler_->setPaulStreams(streams);
        tape_.reset(new TapeInsert(engineRate_));
        delay_.reset(new TapeDelay(engineRate_));
        reverb_.reset(new Reverb(engineRate_));
        if (curSample_) { sampler_->setSource(&curSample_->core->buf); sampler_->setSlices(curSample_->slices, curSample_->nSlices, curSample_->sliceEnd); }
        if (curSlices_) sampler_->setSlices(curSlices_->starts, curSlices_->n, curSlices_->end);
        if (curStretch_ && !curStretch_->none) sampler_->setProcessed(&curStretch_->buf, curStretch_->a, curStretch_->b);
    }
    void publishCounts(const std::vector<LibFolder>& lib)
    {
        nFolders_.store((int)lib.size());
        int counts[100] = {};
        for (size_t i = 0; i < lib.size() && i < 100; i++) counts[i] = (int)lib[i].files.size();
        for (int i = 0; i < 100; i++) folderCount_[i].store(counts[i]);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Audio thread
    // ---------------------------------------------------------------------------------------------------------------
    void retire(Retirable* r) { if (r) retire_.push(r); }   // the callers checked there is room

    void takeMailboxes()
    {
        if (retire_.space() < 6) return;   // up to five objects retire below; the worker has not caught up: try next block
        if (SampleSet* s = sampleBox_.take()) {
            sampler_->setSource(&s->core->buf);   // cuts the voices and stops their Paul streams first
            sampler_->setSlices(s->slices, s->nSlices, s->sliceEnd);
            retire(curSample_); retire(curStretch_); retire(curSlices_);
            curSample_ = s; curStretch_ = nullptr; curSlices_ = nullptr;
            ArpEvList dummy; arp_.clear(dummy); std::memset(heldCount_, 0, sizeof heldCount_);
        }
        if (StretchSet* st = stretchBox_.take()) {
            if (curSample_ && st->sampleId == curSample_->id) {
                sampler_->setProcessed(st->none ? nullptr : &st->buf, st->a, st->b);   // cuts the voices first
                retire(curStretch_); curStretch_ = st;
            } else retire(st);
        }
        if (SliceSet* sl = sliceBox_.take()) {
            if (curSample_ && sl->sampleId == curSample_->id) {
                retire(curSlices_); curSlices_ = sl;
                sampler_->setSlices(sl->starts, sl->n, sl->end);
            } else retire(sl);
        }
    }

    void readSettings(const mpc::ProcessContext& ctx)
    {
        Sampler& sp = *sampler_;
        const int algo = getInt(kAlgo);
        sp.setMode(algo);
        sp.setEnv(get(kAttack), get(kDecay), get(kSustain) / 100.f, get(kRelease));
        float a = get(kStart) / 100.f, b = get(kEnd) / 100.f;   // the FM1 keeps START at least 2 % before END
        if (a > 0.98f) a = 0.98f;
        if (b < a + 0.02f) b = a + 0.02f;
        sp.setRange(a, b);
        sp.setKitPitch((float)getInt(kPitch));
        sp.setKitBase(getInt(kKitNote));
        sp.setGrain(getInt(kGrains), get(kScatter) / 100.f);
        sp.setPaul(get(kStretch), get(kWidth) / 100.f);
        const float detected = curSample_ ? curSample_->core->detectedRoot : -1.f;
        sp.setRoot(getBool(kAutoRoot) && detected > 0.f ? detected : (float)getInt(kRoot));

        keys_.set(getInt(kScale), getInt(kKeyRoot), getInt(kOctave), getInt(kKeyMode) == 1);
        bendRange_ = (float)getInt(kBendRange);
        fm1_ = getBool(kFm1Ctrl);

        const int arpRate = getInt(kArpRate);
        const bool arpOn = arpRate > 0;
        arp_.setup(std::max(0, arpRate - 1), getInt(kArpPattern), getInt(kArpOctaves), get(kArpLength) / 100.f);
        if (arpOn != arpOn_) { ArpEvList dummy; arp_.clear(dummy); sp.allOff(); arpOn_ = arpOn; }

        // TAPE: the FM1's SEQ page drives the cassette insert this way (SamplerApp::applySeq)
        const float exc = get(kExciter) / 100.f, lofi = get(kLofi) / 100.f, wob = get(kWobble) / 100.f;
        if (exc != tapeExc_ || lofi != tapeLofi_ || wob != tapeWob_) {
            tapeExc_ = exc; tapeLofi_ = lofi; tapeWob_ = wob;
            tape_->setParams(exc, 1.f - 0.4f * lofi, wob, 0.25f * lofi, clampf(std::max(exc * 3.f, std::max(wob, lofi)), 0.f, 1.f), 1.f - 0.6f * lofi);
        }

        reverb_->setSize(get(kRevSize) / 100.f);
        static const float kSyncBeats[10] = {0.f, 0.25f, 1.f / 3.f, 0.5f, 0.75f, 2.f / 3.f, 1.f, 1.5f, 2.f, 4.f};
        const int sync = getInt(kDelSync);
        const double bpm = ctx.transport.tempoValid && ctx.transport.bpm > 1.0 ? ctx.transport.bpm : 120.0;
        const float delaySec = sync > 0 ? (float)(kSyncBeats[std::min(9, sync)] * 60.0 / bpm) : get(kDelTime);
        delay_->setTime(clampf(delaySec, 0.02f, 3.f));
        delay_->setFeedback(get(kDelFeedback) / 100.f);

        vol_ = get(kVolume) / 100.f * 1.2f;   // FM1: track volume x 1.2
        pan_ = get(kPan) / 100.f;
        revSend_ = get(kRevSend) / 100.f; delSend_ = get(kDelSend) / 100.f;
        lfoRate_ = get(kLfoRate); lfoDepth_ = get(kLfoDepth) / 100.f; lfoTarget_ = getInt(kLfoTarget); lfoShape_ = getInt(kLfoShape);
    }

    void releaseAll() { ArpEvList dummy; arp_.clear(dummy); sampler_->releaseAll(); std::memset(heldCount_, 0, sizeof heldCount_); }

    // a note that reached the engine side of the keyboard (after scale / chord): to the arpeggiator or straight to the sampler
    void liveNote(int note, int vel, int offset)
    {
        if (arpOn_) { if (vel) arp_.noteOn(note); else arp_.noteOff(note); }
        else evs_.push(offset, note, vel);
    }

    void handleMidi(const mpc::MidiEvent& e, int offset)
    {
        if (fm1_ && fm1Event(e)) return;
        if (e.isController() && e.controller() == 120) { releaseAll(); sampler_->allOff(); return; }   // all sound off: cut
        if (e.isAllNotesOff()) { releaseAll(); return; }                                                 // all notes off (STOP): release
        if (e.isPitchBend()) { bendSemis_ = (float)e.pitchBend() / 8192.f * bendRange_; return; }
        if (e.isNoteOn()) {
            const int in = e.note();
            if (heldCount_[in]) for (int i = 0; i < heldCount_[in]; i++) liveNote(held_[in][i], 0, offset);   // retrigger of a held key
            heldCount_[in] = keys_.map(in, held_[in]);
            for (int i = 0; i < heldCount_[in]; i++) liveNote(held_[in][i], e.velocity(), offset);
        } else if (e.isNoteOff()) {
            const int in = e.note();
            for (int i = 0; i < heldCount_[in]; i++) liveNote(held_[in][i], 0, offset);
            heldCount_[in] = 0;
        }
    }

    void applyNote(const ArpEv& e) { if (e.vel) sampler_->noteOn(e.note, e.vel); else sampler_->noteOff(e.note); }

    void renderSub(float* outL, float* outR, int m)
    {
        // LFO: one value per sub-block (engine/session/Session.cpp, block())
        float lfoGain = 1.f, lfoPan = 0.f, bend = 0.f; bool filterOn = false;
        if (lfoDepth_ > 0.001f) {
            lfoPhase_ += lfoRate_ * m / (float)engineRate_;
            if (lfoPhase_ >= 1.f) { lfoPhase_ -= std::floor(lfoPhase_); rng_ = rng_ * 1664525u + 1013904223u; lfoSH_ = (float)((int)(rng_ >> 9) % 2001 - 1000) / 1000.f; }
            const float ph = lfoPhase_;
            const float v = lfoShape_ == 0 ? std::sin(2.f * kPi * ph) : lfoShape_ == 1 ? (ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph) : lfoShape_ == 2 ? (ph < 0.5f ? 1.f : -1.f) : lfoSH_;
            if (lfoTarget_ == 0) bend = v * lfoDepth_ * lfoDepth_ * 12.f;   // +-12 semitones at full depth
            else if (lfoTarget_ == 1) { filterOn = true; lp_.set(20.f * std::pow(1000.f, clampf(0.5f + 0.5f * v * lfoDepth_, 0.f, 1.f)), (float)engineRate_); }
            else if (lfoTarget_ == 2) lfoGain = 1.f - lfoDepth_ * (0.5f - 0.5f * v);
            else lfoPan = v * lfoDepth_;
        }
        if (filterOn != filtOn_) { filtOn_ = filterOn; lp_.reset(); }

        float* bus = bus_;
        std::memset(bus, 0, sizeof(float) * 2 * (size_t)m);
        Sampler& sp = *sampler_;
        sp.setBend(bend + bendSemis_);
        int cur = 0;
        for (int i = 0; i < evs_.n; i++) {
            const int off = std::min(m, std::max(cur, evs_.ev[i].offset));
            if (off > cur) { sp.render(bus + 2 * cur, off - cur); cur = off; }
            applyNote(evs_.ev[i]);
        }
        if (cur < m) sp.render(bus + 2 * cur, m - cur);
        if (filtOn_) for (int i = 0; i < m; i++) { bus[2 * i] = lp_.process(0, bus[2 * i]); bus[2 * i + 1] = lp_.process(1, bus[2 * i + 1]); }
        tape_->process(bus, m);

        // volume, pan (constant power, as the FM1 mixer), ramped across the sub-block
        const float g = vol_ * lfoGain;
        const float ang = (clampf(pan_ + lfoPan, -1.f, 1.f) * 0.5f + 0.5f) * 1.5707963f;
        const float gl = g * std::cos(ang) * 1.4142f, gr = g * std::sin(ang) * 1.4142f;
        float* s0 = send_[0]; float* s1 = send_[1];
        float peak = 0.f;
        for (int i = 0; i < m; i++) {
            const float t = (float)(i + 1) / m;
            const float l = bus[2 * i] * (gl0_ + (gl - gl0_) * t), r = bus[2 * i + 1] * (gr0_ + (gr - gr0_) * t);
            bus[2 * i] = l; bus[2 * i + 1] = r;
            s0[2 * i] = l * revSend_; s0[2 * i + 1] = r * revSend_;
            s1[2 * i] = l * delSend_; s1[2 * i + 1] = r * delSend_;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
        }
        gl0_ = gl; gr0_ = gr;
        // The two send effects run while something goes in or their tail is still above -100 dB; after that they sleep
        // (their buffers only hold values under -100 dB) until a send has signal again. Idle instances then cost ~nothing.
        const bool revIn = peak * revSend_ > 1e-6f, delIn = peak * delSend_ > 1e-6f;
        if (revIn || revQuiet_ < kRevSleep) {
            reverb_->process(s0, wet_, m);
            float wp = 0.f; for (int i = 0; i < 2 * m; i++) { bus[i] += wet_[i] * 0.8f; wp = std::max(wp, std::fabs(wet_[i])); }
            revQuiet_ = revIn || wp > 1e-5f ? 0 : revQuiet_ + m;
            peak = std::max(peak, wp);
        }
        if (delIn || delQuiet_ < kDelSleep) {
            delay_->process(s1, wet_, m);
            float wp = 0.f; for (int i = 0; i < 2 * m; i++) { bus[i] += wet_[i] * 0.7f; wp = std::max(wp, std::fabs(wet_[i])); }
            delQuiet_ = delIn || wp > 1e-5f ? 0 : delQuiet_ + m;
            peak = std::max(peak, wp);
        }
        if (peak == 0.f) { std::memset(outL, 0, sizeof(float) * (size_t)m); std::memset(outR, 0, sizeof(float) * (size_t)m); return; }
        for (int i = 0; i < m; i++) { outL[i] = ftanh(bus[2 * i] * 0.9f); outR[i] = ftanh(bus[2 * i + 1] * 0.9f); }
    }
    static constexpr int kRevSleep = 8192;           // > twice the longest comb of the reverb
    static constexpr int kDelSleep = 44100 * 7 / 2;  // > the delay line (3.3 s): every echo still in it is below -100 dB

    // ---------------------------------------------------------------------------------------------------------------
    // FM-1 control surface (optional, "FM-1 Ctrl" On): the M-VAVE FM-1 with the FM1 Sampler firmware, plugged into the
    // MPC's USB host port and routed to this track. Its MIDI (PROTOCOL.md): panel buttons are notes 0..13, the seven
    // encoders CC 16..22 relative (64 + delta), the volume knob CC 7. Keys already arrive as scale notes.
    // ---------------------------------------------------------------------------------------------------------------
    enum Fm1Page { F_HOME, F_FX, F_ENV, F_LFO, F_ARP, F_TAPE, F_MIX, F_KEY };
    bool fm1Event(const mpc::MidiEvent& e)
    {
        if ((e.isNoteOn() || e.isNoteOff()) && e.note() <= 13) {
            if (e.isNoteOn()) switch (e.note()) {
                case 0: nudge(kOctave, -1); break;   // OCT- / OCT+ (only if the firmware forwards them)
                case 1: nudge(kOctave, +1); break;
                case 2: fm1Page_ = F_FX; break;   case 4: fm1Page_ = F_ENV; break;  case 5: fm1Page_ = F_LFO; break;
                case 6: fm1Page_ = F_MIX; break;  case 7: fm1Page_ = F_KEY; break;  case 8: fm1Page_ = F_HOME; break;
                case 10: fm1Page_ = F_ARP; break; case 11: fm1Page_ = F_TAPE; break;
                default: break;                   // SAVE, PLAY, REC: the MPC's own transport and sampler do this
            }
            return true;
        }
        if (e.isController()) {
            const int cc = e.controller(), v = e.controllerValue();
            if (cc >= 16 && cc <= 22) { encoder(cc - 16, v - 64); return true; }
            if (cc == 7) { setAndTell(kVolume, param(kVolume).toPlain(v / 127.f)); return true; }
            if (cc == 39 || cc == 24) return true;   // knob LSB, battery
        }
        return false;
    }
    int pageKnob(int k) const   // K1..K4 of the current FM-1 page -> parameter (-1: nothing on the MPC)
    {
        static const int fx[4] = {kRevSend, kRevSize, kDelSend, kDelTime}, env[4] = {kAttack, kDecay, kSustain, kRelease},
                         lfo[4] = {kLfoRate, kLfoDepth, kLfoTarget, kLfoShape}, arp[4] = {kArpRate, kArpPattern, kArpOctaves, kArpLength},
                         tape[4] = {-1, kExciter, kLofi, kWobble}, mix[4] = {kVolume, kPan, kDelFeedback, kDelSync},
                         key[4] = {kScale, kKeyRoot, kOctave, kKeyMode};
        switch (fm1Page_) {
            case F_HOME: {
                if (k == 0) return kStart;
                if (k == 1) return kEnd;
                const int a = getInt(kAlgo);
                if (a == A_KIT) return k == 2 ? kThreshold : kPitch;
                if (a == A_GRAIN) return k == 2 ? kGrains : kScatter;
                if (a == A_PAUL) return k == 2 ? kStretch : kWidth;
                return k == 2 ? kSpeed : kPitch;
            }
            case F_FX: return fx[k];
            case F_ENV: return env[k];
            case F_LFO: return lfo[k];
            case F_ARP: return arp[k];
            case F_TAPE: return tape[k];
            case F_MIX: return mix[k];
            default: return key[k];
        }
    }
    void encoder(int i, int delta)
    {
        if (delta == 0) return;
        if (i == 0) { wrapNudge(kFolder, delta, nFolders_.load()); return; }   // SELECT: folder
        if (i == 1) {                                                         // PRESETS: sample of the folder
            const int nf = std::max(1, nFolders_.load());
            wrapNudge(kSample, delta, folderCount_[std::min(99, (getInt(kFolder) - 1) % nf)].load());
            return;
        }
        if (i == 2) { wrapNudge(kAlgo, delta > 0 ? 1 : -1, 6, 0); return; }   // ALGORITHM
        const int p = pageKnob(i - 3);
        if (p < 0) return;
        if (param(p).kind == mpc::Param::Kind::Float) { setNormalised(p, getNormalised(p) + 0.01f * (float)delta); tell(p); }
        else nudge(p, param(p).kind == mpc::Param::Kind::Int ? delta : (delta > 0 ? 1 : -1));
    }
    void nudge(int p, int steps) { setAndTell(p, get(p) + (float)steps); }
    void wrapNudge(int p, int delta, int count, int base = 1)   // steps through 1..count (or 0..count-1) and wraps
    {
        if (count <= 0) return;
        const int cur = ((getInt(p) - base) % count + count) % count;
        setAndTell(p, (float)(base + ((cur + delta) % count + count) % count));
    }
    void setAndTell(int p, float plain) { set(p, plain); tell(p); }
    void tell(int p) { notify_.push(p); }   // the worker calls the host (not from the audio thread)

    // ---------------------------------------------------------------------------------------------------------------
    // Worker thread
    // ---------------------------------------------------------------------------------------------------------------
    void workerLoop()
    {
        auto selAt = Clock::now(); bool selDirty = true; int lastF = -1, lastS = -1;
        struct { uint32_t id = 0; float a = -1, b = -1, thr = -1; } sliceKey;
        struct { uint32_t id = 0; float a = -1, b = -1, speed = -1; int semis = 999; bool none = true; } stretchKey, wantStretch;
        auto stretchAt = Clock::now();
        while (!quit_.load()) {
            collectGarbage();
            int p = -1;
            while (notify_.pop(p)) notifyHostParameterChanged(p);

            // ---- the sample: from a project / preset path, or from FOLDER + SAMPLE ----
            std::string pending;
            { std::lock_guard<std::mutex> l(stateMtx_); if (pendingPathSet_) { pending = pendingPath_; pendingPathSet_ = false; } }
            const int f = getInt(kFolder), s = getInt(kSample);
            if (!pending.empty()) {
                rescan();
                int fi = -1, si = -1; findPath(pending, fi, si);
                if (fi >= 0) { set(kFolder, (float)(fi + 1)); set(kSample, (float)(si + 1)); }
                lastF = getInt(kFolder); lastS = getInt(kSample);
                struct stat stt;
                if (::stat(pending.c_str(), &stt) == 0) { selDirty = false; if (pending != currentPath()) load(pending); }
                else { selDirty = true; selAt = Clock::now() - std::chrono::seconds(1); }   // the file is gone: FOLDER + SAMPLE decide
            } else {
                if (f != lastF || s != lastS) { lastF = f; lastS = s; selDirty = true; selAt = Clock::now(); }
                if (selDirty && msSince(selAt) > 120.0) {
                    selDirty = false;
                    if (msSince(lastScan_) > 2000.0) rescan();
                    const std::string path = resolve(f, s);
                    if (!path.empty() && path != currentPath()) load(path);
                }
            }

            // ---- KIT slices of the window: pickSlices is cheap, done at once ----
            const float a = clampf(get(kStart) / 100.f, 0.f, 0.98f), b = std::max(a + 0.02f, get(kEnd) / 100.f), thr = get(kThreshold) / 100.f;
            if (core_ && (sliceKey.id != coreId_ || sliceKey.a != a || sliceKey.b != b || sliceKey.thr != thr)) {
                sliceKey.id = coreId_; sliceKey.a = a; sliceKey.b = b; sliceKey.thr = thr;
                SliceSet* sl = new SliceSet; sl->sampleId = coreId_;
                makeSlices(*core_, a, b, thr, sl->starts, sl->n, sl->end);
                delete sliceBox_.post(sl);
            }

            // ---- SPEED / PITCH: the offline stretched copy, once the knobs rest (the FM1 waits 350 ms) ----
            const int algo = getInt(kAlgo);
            const bool plain = algo == A_KIT || algo == A_GRAIN || algo == A_PAUL;
            const float speed = plain ? 1.f : get(kSpeed); const int semis = plain ? 0 : getInt(kPitch);
            if (core_ && (wantStretch.id != coreId_ || wantStretch.a != a || wantStretch.b != b || wantStretch.speed != speed || wantStretch.semis != semis)) {
                wantStretch.id = coreId_; wantStretch.a = a; wantStretch.b = b; wantStretch.speed = speed; wantStretch.semis = semis;
                stretchAt = Clock::now();
            }
            if (core_ && (stretchKey.id != wantStretch.id || stretchKey.a != wantStretch.a || stretchKey.b != wantStretch.b || stretchKey.speed != wantStretch.speed || stretchKey.semis != wantStretch.semis)
                && msSince(stretchAt) > 300.0) {
                const bool none = std::fabs(speed - 1.f) < 0.005f && semis == 0;
                const bool sameSample = stretchKey.id == wantStretch.id;
                const bool wasNone = !sameSample || stretchKey.none;   // a new sample starts without a stretched copy
                stretchKey = wantStretch; stretchKey.none = none;
                if (!(none && wasNone)) {   // nothing to swap: do not cut the voices for nothing
                    StretchSet* st = new StretchSet; st->sampleId = coreId_;
                    makeStretch(*core_, a, b, speed, semis, *st);
                    delete stretchBox_.post(st);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::string currentPath() { std::lock_guard<std::mutex> l(stateMtx_); return loadedPath_; }

    // A retired object may still be read by the Paul producer (a hop in progress). The audio thread stopped the streams
    // before retiring it, so it is safe once every stream has acknowledged the commands sent until now.
    void collectGarbage()
    {
        Retirable* r = nullptr;
        while (retire_.pop(r)) {
            Pending pd; pd.obj = r;
            for (int i = 0; i < Sampler::kVoices; i++) pd.seq[i] = paul_[(size_t)i]->commandSeq();
            pendingDelete_.push_back(pd);
        }
        for (size_t k = 0; k < pendingDelete_.size();) {
            bool safe = true;
            for (int i = 0; i < Sampler::kVoices; i++) if ((int32_t)(paul_[(size_t)i]->ackSeq() - pendingDelete_[k].seq[i]) < 0) { safe = false; break; }
            if (safe) { delete pendingDelete_[k].obj; pendingDelete_[k] = pendingDelete_.back(); pendingDelete_.pop_back(); }
            else ++k;
        }
    }

    // Paul producer thread: keeps every PAULSTRETCH voice a few hops ahead
    void paulLoop()
    {
        while (!quit_.load()) {
            bool worked = false;
            for (auto& s : paul_) worked |= s->produce();
            if (!worked) std::this_thread::sleep_for(std::chrono::microseconds(700));
        }
    }

    void rescan()
    {
        std::vector<LibFolder> lib = scanLibrary();
        publishCounts(lib);
        { std::lock_guard<std::mutex> l(libMtx_); lib_ = std::move(lib); }
        lastScan_ = Clock::now();
    }
    std::string resolve(int f, int s)
    {
        std::lock_guard<std::mutex> l(libMtx_);
        if (lib_.empty()) return "";
        const LibFolder& fo = lib_[(size_t)((std::max(1, f) - 1) % (int)lib_.size())];
        if (fo.files.empty()) return "";
        return fo.files[(size_t)((std::max(1, s) - 1) % (int)fo.files.size())].path;
    }
    void findPath(const std::string& path, int& fi, int& si)
    {
        std::lock_guard<std::mutex> l(libMtx_);
        for (size_t i = 0; i < lib_.size(); i++)
            for (size_t j = 0; j < lib_[i].files.size(); j++)
                if (lib_[i].files[j].path == path) { fi = (int)i; si = (int)j; return; }
    }

    // reads a WAV, normalises it as the FM1 does (Sampler::load), finds its onsets and its pitch
    void load(const std::string& path)
    {
        auto core = std::make_shared<SampleCore>();
        SampleBuf& buf = core->buf;
        if (!readWav(path, buf.d, buf.sr)) return;   // unreadable: keep playing the current sample
        buf.frames = (int)(buf.d.size() / 2);
        const int maxFrames = (int)(kMaxSampleSeconds * buf.sr);
        if (buf.frames > maxFrames) { buf.frames = maxFrames; buf.d.resize((size_t)maxFrames * 2); buf.d.shrink_to_fit(); }
        if (buf.frames < 64) return;
        // loudness normalisation: the RMS of the part that sounds (20 ms blocks within 30 dB of the loudest one) goes to a common level
        float pk = 0.f; for (float v : buf.d) pk = std::max(pk, std::fabs(v));
        if (pk > 1e-4f) {
            const int blk = std::max(64, buf.sr / 50); const int nb = std::max(1, buf.frames / blk);
            std::vector<double> e((size_t)nb, 0.0); double emax = 0;
            for (int bI = 0; bI < nb; bI++) { double sum = 0; for (int i = bI * blk; i < (bI + 1) * blk && i < buf.frames; i++) sum += (double)buf.d[2 * (size_t)i] * buf.d[2 * (size_t)i] + (double)buf.d[2 * (size_t)i + 1] * buf.d[2 * (size_t)i + 1]; e[(size_t)bI] = sum / (2.0 * blk); emax = std::max(emax, e[(size_t)bI]); }
            double sum = 0; int cnt = 0; for (double v : e) if (v > emax * 1e-3) { sum += v; cnt++; }
            const float rms = cnt ? (float)std::sqrt(sum / cnt) : pk;
            const float g = std::min(std::min(3.0f / pk, 0.25f / std::max(rms, 1e-4f)), 16.f);
            for (float& v : buf.d) {
                float x = v * g; const float ax = std::fabs(x);
                if (ax > 0.6f) x = (x < 0 ? -1.f : 1.f) * (0.6f + 0.38f * std::tanh((ax - 0.6f) / 0.38f));
                v = x;
            }
        }
        core->onsets = findOnsets(buf.d, buf.frames, buf.sr);
        const float hz = detectPitchHz(buf);
        core->detectedRoot = hz > 20.f ? 69.f + 12.f * std::log2(hz / 440.f) : -1.f;
        core->path = path;
        const size_t slash = path.find_last_of('/');
        core->label = stem(slash == std::string::npos ? path : path.substr(slash + 1));

        SampleSet* set = new SampleSet;
        set->core = core; set->id = ++coreCounter_;
        const float a = clampf(get(kStart) / 100.f, 0.f, 0.98f), b = std::max(a + 0.02f, get(kEnd) / 100.f);
        makeSlices(*core, a, b, get(kThreshold) / 100.f, set->slices, set->nSlices, set->sliceEnd);
        core_ = core; coreId_ = set->id;
        { std::lock_guard<std::mutex> l(stateMtx_); loadedPath_ = path; }
        delete sampleBox_.post(set);
    }

    static void makeSlices(const SampleCore& c, float a, float b, float thr, int* starts, int& n, double& end)
    {
        const int frames = c.buf.frames;
        const int from = (int)(a * frames), to = std::min(frames, std::max(from + 64, (int)(std::min(1.f, b) * frames)));
        const std::vector<int> s = pickSlices(c.onsets, from, to, c.buf.sr, thr, Sampler::kMaxSlices);
        n = std::min((int)s.size(), (int)Sampler::kMaxSlices);
        for (int i = 0; i < n; i++) starts[i] = s[(size_t)i];
        end = to;
    }

    static void makeStretch(const SampleCore& c, float a, float b, float speed, int semis, StretchSet& st)
    {
        st.none = std::fabs(speed - 1.f) < 0.005f && semis == 0;
        st.a = a; st.b = std::min(1.f, b);
        if (st.none) return;
        const int frames = c.buf.frames;
        const int f0 = (int)(a * frames);
        int f1 = std::max(f0 + 64, (int)(st.b * frames));
        f1 = std::min(f1, frames);
        // memory: at most kMaxStretchSeconds of output (the window is cut, as if END were earlier)
        const int maxIn = (int)(kMaxStretchSeconds * c.buf.sr * std::max(0.25f, speed));
        if (f1 - f0 > maxIn) { f1 = f0 + maxIn; st.b = (float)f1 / (float)frames; }
        std::vector<float> seg(c.buf.d.begin() + 2 * (size_t)f0, c.buf.d.begin() + 2 * (size_t)f1);
        st.buf.d = stretchPitch(seg, c.buf.sr, speed, (double)semis);
        st.buf.frames = (int)(st.buf.d.size() / 2); st.buf.sr = c.buf.sr;
        if (st.buf.frames < 64) { st.none = true; st.buf.d.clear(); st.buf.frames = 0; }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Members
    // ---------------------------------------------------------------------------------------------------------------
    int engineRate_ = 44100;
    std::unique_ptr<Sampler> sampler_;
    std::unique_ptr<TapeInsert> tape_;
    std::unique_ptr<TapeDelay> delay_;
    std::unique_ptr<Reverb> reverb_;
    Lowpass lp_; bool filtOn_ = false;
    ArpRT arp_; bool arpOn_ = false;
    KeyMap keys_;
    ArpEvList evs_;
    int held_[128][3] = {}; int heldCount_[128] = {};

    float bus_[2 * kSub] = {}, send_[2][2 * kSub] = {}, wet_[2 * kSub] = {};
    float vol_ = 0.96f, pan_ = 0.f, revSend_ = 0.3f, delSend_ = 0.2f, gl0_ = 0.f, gr0_ = 0.f;
    float lfoRate_ = 0.5f, lfoDepth_ = 0.f, lfoPhase_ = 0.f, lfoSH_ = 0.f; int lfoTarget_ = 0, lfoShape_ = 0;
    float tapeExc_ = -1.f, tapeLofi_ = -1.f, tapeWob_ = -1.f;
    int revQuiet_ = 0, delQuiet_ = 0;
    float bendSemis_ = 0.f, bendRange_ = 2.f; bool fm1_ = false; int fm1Page_ = F_HOME;
    uint32_t rng_ = 12345;

    // audio-thread side of the objects
    SampleSet* curSample_ = nullptr; StretchSet* curStretch_ = nullptr; SliceSet* curSlices_ = nullptr;
    Mailbox<SampleSet> sampleBox_; Mailbox<StretchSet> stretchBox_; Mailbox<SliceSet> sliceBox_;
    SpscRing<Retirable*, 64> retire_;
    SpscRing<int, 64> notify_;

    // worker side
    std::thread worker_, paulThread_; std::atomic<bool> quit_{false};
    std::unique_ptr<PaulStream> paul_[Sampler::kVoices];
    struct Pending { Retirable* obj = nullptr; uint32_t seq[Sampler::kVoices] = {}; };
    std::vector<Pending> pendingDelete_;
    std::shared_ptr<const SampleCore> core_; uint32_t coreId_ = 0, coreCounter_ = 0;
    Clock::time_point lastScan_;

    // shared, under mutexes (never touched by the audio thread)
    std::mutex libMtx_; std::vector<LibFolder> lib_;
    std::mutex stateMtx_; std::string loadedPath_, pendingPath_; bool pendingPathSet_ = false;
    std::atomic<int> nFolders_{0}; std::atomic<int> folderCount_[100];
};

MPC_PLUGIN_ENTRY(FM1Sampler)
