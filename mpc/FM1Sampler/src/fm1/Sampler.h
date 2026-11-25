// The sampler instrument: one sample, up to eight voices, six playback algorithms.
//   MELODIC  pitched by the note, ADSR, plays the window once
//   ONESHOT  pitched by the note, always plays the whole window (note off is ignored)
//   LOOPED   pitched by the note, ADSR, the window loops while the key is held
//   KIT      the sample is cut at its hits into 16 slices; a note plays one slice unpitched
//   GRAIN    a cloud of short overlapping grains picked at random inside the window
//   PAUL     the window smeared into an endless pad (spectral time stretch)
//
// MPC port of engine/dsp/Sampler.h / .cpp. The voice rendering is the phone's, line for line, except:
//   * the sample, its stretched copy and its slices are owned by the plugin and handed in as plain pointers:
//     loading, slicing (Slicer.h) and the offline stretch (Stretch.h) run on the plugin's worker thread, so nothing
//     here allocates or frees memory (the MPC's audio thread must never do either);
//   * KIT: the slice is (note - kit note) mod 16, so any 16 consecutive MPC pads play the 16 slices in order
//     (the phone used note - 60);
//   * GRAIN: the same grains, spawned at the same samples with the same random numbers, but each grain is rendered in its own
//     tight loop over the block (its end is known in advance) and its Hann window is a cosine recurrence instead of a cos()
//     per grain per sample; only the order in which the grains are summed differs;
//   * PAUL: the spectral streams are computed on the plugin's producer thread (see Paul.h); the voices only read them;
//   * releaseAll(): all notes off releases the voices through their envelopes (the MPC sends it on STOP) instead of cutting.
#pragma once
#include "Adsr.h"
#include "Common.h"
#include "Paul.h"
#include <memory>
#include <vector>

struct SampleBuf {
    std::vector<float> d;   // interleaved stereo
    int frames = 0, sr = SAMPLERATE;
};

class Sampler {
public:
    enum Mode { MELODIC, ONESHOT, LOOPED, KIT, GRAIN, PAUL, kModes };
    static constexpr int kVoices = 8, kMaxSlices = 16, kPaulVoices = 4;

    explicit Sampler(int engineRate);
    ~Sampler();

    // ---- audio thread; nothing below allocates ----
    void setSource(const SampleBuf* orig);                        // the loaded sample (or null): cuts every voice
    void setProcessed(const SampleBuf* p, float a, float b);      // the offline stretched / pitched copy of [a, b] (null: play the original); cuts every voice, as the phone does
    void setSlices(const int* starts, int n, double end);         // KIT: the slice starts inside the window and where the last one ends (frames of the original)
    void setMode(int m);
    void setEnv(float attack, float decay, float sustain, float release);
    void setRange(float a, float b);   // 0..1 of the original sample
    void setBend(float semitones) { bend_ = semitones; }
    void setRoot(float note) { root_ = note; }   // the note at which the sample sounds as recorded (60 = C4)
    void setKitPitch(float semis) { kitSemis_ = semis; }
    void setKitBase(int note) { kitBase_ = note; }
    void setGrain(int count, float scatter) { grainN_ = std::min(12, std::max(1, count)); grainScatter_ = std::min(1.f, std::max(0.f, scatter)); }
    void setPaul(float stretch, float width) { paulStretch_ = std::min(100.f, std::max(1.5f, stretch)); paulWidth_ = std::min(2.f, std::max(0.f, width)); }
    int mode() const { return mode_; }
    void noteOn(int note, int vel);
    void noteOff(int note);
    void allOff();      // cut every voice now
    void releaseAll();  // every voice to its release stage
    void render(float* out /*stereo, added*/, int n);
    bool hasVoices() const;
    void setPaulStreams(PaulStream* const* streams) { for (int i = 0; i < kVoices; i++) paul_[i] = streams[i]; }   // owned by the plugin

private:
    struct Grain { bool on = false; double pos = 0, inc = 0; int age = 0, len = 1; double c0 = 1, c1 = 1, k = 2; };   // c0, c1: cos recurrence of the window
    struct Voice {
        bool on = false, gate = false; int note = 0, slice = 0; float vel = 1.f; uint32_t born = 0;
        double pos = 0, rate = 1.0, ws = 0, we = 0;   // window of the played buffer, frames
        const SampleBuf* buf = nullptr;
        Adsr env; Grain grain[12]; double spawn = 0; int paulIdx = -1;
    };
    void resolveWindow(Voice& v, const SampleBuf*& buf, double& ws, double& we) const;
    void renderVoice(Voice& v, float* out, int n);
    void renderGrains(Voice& v, float* out, int n, double rate, float gain);
    int grainLife(const Grain& g, int frames) const;
    float rnd() { rng_ = rng_ * 1664525u + 1013904223u; return (rng_ >> 8) / 16777216.f; }

    int sr_;
    const SampleBuf* orig_ = nullptr; const SampleBuf* proc_ = nullptr;
    float procA_ = 0, procB_ = 1, rangeA_ = 0, rangeB_ = 1, bend_ = 0, atk_ = 0.01f, dec_ = 0.3f, sus_ = 0.8f, rel_ = 0.3f, root_ = 60.f;
    int mode_ = MELODIC, kitBase_ = 36;
    int slices_[kMaxSlices] = {}; int nSlices_ = 0; double sliceEnd_ = 0;
    Voice voices_[kVoices]; PaulStream* paul_[kVoices] = {};
    float kitSemis_ = 0.f, grainLen_ = 0.12f, grainScatter_ = 1.f, paulStretch_ = 8.f, paulWidth_ = 1.f; int grainN_ = 7;
    uint32_t bornCounter_ = 0, rng_ = 7;
};
