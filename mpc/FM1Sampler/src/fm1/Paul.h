// Extreme time stretch ("Paulstretch"): overlapping windows of the source are turned into spectra, the phases are randomised, and the
// result is overlap-added, so the sound is smeared into an endless pad. One stream per voice.
//
// MPC port of engine/dsp/Paul.h. The algorithm and its sound are the phone's (N = 4096, hop N/2, sine window, random phases).
// What changed is WHERE it runs: a hop costs four 4096-point FFTs, about 3 ms on the Cortex-A17 of an MPC One, i.e. a whole
// audio block. So the spectra are computed on a producer thread of the plugin (another core) a few hops ahead, and the audio
// thread only reads the finished frames from a lock-free FIFO, resampling them by the note's pitch as before.
//
//   audio thread     start() / stop() post a command (seqlock); beginRead(), next(), endRead() read the FIFO.
//   producer thread  produce() takes the latest command and writes at most one hop into the FIFO.
// The producer reads the source buffer given to start(). Before such a buffer is freed, the owner must wait until every
// stream has acknowledged the commands sent so far (commandSeq() / ackSeq()), which proves no hop still reads it.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class PaulStream {
public:
    static constexpr int N = 4096, HOP = N / 2;
    static constexpr uint32_t kFifo = (uint32_t)N * 4;     // frames
    static constexpr uint32_t kAhead = (uint32_t)N * 2;    // frames the producer keeps ready (4 hops, ~190 ms at the original pitch)

    PaulStream();
    ~PaulStream();

    // ---- audio thread ----
    void start(const float* src, int frames, double from, double to, float stretch);
    void stop();
    void beginRead();                                  // once per render call
    void next(double rate, float& l, float& r);        // silence until the first hop exists or if the producer fell behind
    void endRead();                                    // once per render call
    bool active() const { return active_; }

    // ---- producer thread ----
    bool produce();                                    // true if it computed a hop

    // ---- any thread ----
    uint32_t commandSeq() const { const uint32_t s = cmdSeq_.load(std::memory_order_acquire); return (s + 1u) & ~1u; }
    uint32_t ackSeq() const { return ackSeq_.load(std::memory_order_acquire); }

private:
    void post(const float* src, int frames, double from, double to, float stretch, bool run);
    void hop();

    // command: written by the audio thread under a seqlock (odd = being written)
    std::atomic<uint32_t> cmdSeq_{0};
    std::atomic<const float*> cSrc_{nullptr}; std::atomic<int> cFrames_{0}; std::atomic<double> cFrom_{0}, cTo_{0}; std::atomic<float> cStretch_{8.f}; std::atomic<bool> cRun_{false};
    std::atomic<uint32_t> ackSeq_{0};

    // producer state
    struct Fft; std::unique_ptr<Fft> fft_;
    uint32_t curSeq_ = 0; bool running_ = false;
    const float* src_ = nullptr; int frames_ = 0; double from_ = 0, to_ = 0, srcPos_ = 0; float stretch_ = 8.f;
    std::vector<float> ola_, win_, tin_, hopOut_, spec_;
    uint32_t rng_ = 1;

    // FIFO (frame counters wrap modulo 2^32)
    std::vector<float> fifo_;
    std::atomic<uint32_t> written_{0}, consumed_{0};
    std::atomic<uint64_t> genStart_{0};                // (command seq << 32) | first frame of that note

    // consumer state
    uint32_t mySeq_ = 0; bool active_ = false, synced_ = false;
    uint32_t readBase_ = 0, availEnd_ = 0; double readPos_ = 0;
};
