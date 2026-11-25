#include "Paul.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "../kissfft/kiss_fftr.h"

namespace {
// 4096 unit phasors: the random phase of a bin is picked from this table (the phone build calls cos() and sin() per bin)
struct PhaseTable {
    float c[4096], s[4096];
    PhaseTable() { for (int i = 0; i < 4096; i++) { const double a = 6.283185307179586 * (i + 0.5) / 4096.0; c[i] = (float)std::cos(a); s[i] = (float)std::sin(a); } }
};
const PhaseTable& phases() { static const PhaseTable t; return t; }
}  // namespace

struct PaulStream::Fft {
    kiss_fftr_cfg fwd = nullptr, inv = nullptr;
    Fft() { fwd = kiss_fftr_alloc(PaulStream::N, 0, nullptr, nullptr); inv = kiss_fftr_alloc(PaulStream::N, 1, nullptr, nullptr); }
    ~Fft() { free(fwd); free(inv); }
};

PaulStream::PaulStream()
    : fft_(new Fft), ola_((size_t)HOP * 2, 0.f), win_((size_t)N), tin_((size_t)N, 0.f), hopOut_((size_t)HOP * 2, 0.f),
      spec_((size_t)(N / 2 + 1) * 2, 0.f), fifo_((size_t)kFifo * 2, 0.f) {
    for (int i = 0; i < N; i++) win_[(size_t)i] = std::sin(3.14159265f * (i + 0.5f) / N);   // sine window: its square sums to 1 at 50 % overlap
    (void)phases();   // build the table here, never on a real-time thread
}
PaulStream::~PaulStream() = default;

// ------------------------------------------------------------------------------------------------- audio thread
void PaulStream::post(const float* src, int frames, double from, double to, float stretch, bool run) {
    const uint32_t s = cmdSeq_.load(std::memory_order_relaxed);
    cmdSeq_.store(s + 1, std::memory_order_relaxed);               // odd: being written
    std::atomic_thread_fence(std::memory_order_release);
    cSrc_.store(src, std::memory_order_relaxed); cFrames_.store(frames, std::memory_order_relaxed);
    cFrom_.store(from, std::memory_order_relaxed); cTo_.store(to, std::memory_order_relaxed);
    cStretch_.store(stretch, std::memory_order_relaxed); cRun_.store(run, std::memory_order_relaxed);
    cmdSeq_.store(s + 2, std::memory_order_release);               // even: complete
    mySeq_ = s + 2;
}

void PaulStream::start(const float* src, int frames, double from, double to, float stretch) {
    from = std::max(0.0, from); to = std::min((double)frames, to);
    if (to - from < N) { from = std::max(0.0, std::min(from, (double)frames - N)); to = std::min((double)frames, from + N); }
    if (!src || to <= from + 1) { stop(); return; }
    post(src, frames, from, to, stretch, true);
    active_ = true; synced_ = false; readPos_ = 0;
}

void PaulStream::stop() {
    if (!active_) return;
    post(nullptr, 0, 0, 0, 8.f, false);
    active_ = false; synced_ = false;
}

void PaulStream::beginRead() {
    if (!active_) return;
    if (!synced_) {
        const uint64_t gs = genStart_.load(std::memory_order_acquire);
        if ((uint32_t)(gs >> 32) != mySeq_) return;                 // the producer has not taken this note yet
        readBase_ = (uint32_t)gs; readPos_ = 0; synced_ = true;
    }
    availEnd_ = written_.load(std::memory_order_acquire);
}

void PaulStream::next(double rate, float& l, float& r) {
    l = r = 0.f;
    if (!synced_) return;
    const uint32_t avail = availEnd_ - readBase_;
    const uint32_t i0 = (uint32_t)readPos_;
    if (i0 + 2 > avail) return;                                      // the producer fell behind: silence, the position holds
    const float f = (float)(readPos_ - i0);
    const size_t a = (size_t)((readBase_ + i0) % kFifo), b = (size_t)((readBase_ + i0 + 1) % kFifo);
    l = fifo_[a * 2] + (fifo_[b * 2] - fifo_[a * 2]) * f; r = fifo_[a * 2 + 1] + (fifo_[b * 2 + 1] - fifo_[a * 2 + 1]) * f;
    readPos_ += rate;
    if (readPos_ >= 1.0) {   // never step past what the producer has written (it would then wait for us forever)
        const uint32_t drop = std::min((uint32_t)readPos_, avail - 1u);
        readBase_ += drop; readPos_ -= (double)drop;
    }
}

void PaulStream::endRead() {
    if (synced_) consumed_.store(readBase_, std::memory_order_release);
}

// ------------------------------------------------------------------------------------------------- producer thread
bool PaulStream::produce() {
    const uint32_t s1 = cmdSeq_.load(std::memory_order_acquire);
    if ((s1 & 1u) == 0 && s1 != curSeq_) {
        const float* src = cSrc_.load(std::memory_order_relaxed); const int frames = cFrames_.load(std::memory_order_relaxed);
        const double from = cFrom_.load(std::memory_order_relaxed), to = cTo_.load(std::memory_order_relaxed);
        const float stretch = cStretch_.load(std::memory_order_relaxed); const bool run = cRun_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (cmdSeq_.load(std::memory_order_relaxed) == s1) {         // a consistent command
            curSeq_ = s1;
            running_ = run && src != nullptr;
            if (running_) {
                src_ = src; frames_ = frames; from_ = from; to_ = to; stretch_ = stretch; srcPos_ = from_;
                std::fill(ola_.begin(), ola_.end(), 0.f);
                rng_ = rng_ * 747796405u + 2891336453u;
                genStart_.store(((uint64_t)s1 << 32) | written_.load(std::memory_order_relaxed), std::memory_order_release);
            } else src_ = nullptr;
            ackSeq_.store(s1, std::memory_order_release);           // the previous source is no longer read
        }
    }
    if (!running_) return false;
    const uint32_t fill = written_.load(std::memory_order_relaxed) - consumed_.load(std::memory_order_acquire);
    if (fill >= kAhead || fill + (uint32_t)HOP > kFifo) return false;
    hop();
    return true;
}

// one hop: for each channel a windowed spectrum with random phases, overlap-added (engine/dsp/Paul.cpp, makeHop)
void PaulStream::hop() {
    kiss_fft_cpx* spec = reinterpret_cast<kiss_fft_cpx*>(spec_.data());
    float* tin = tin_.data();
    const PhaseTable& t = phases();
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < N; i++) {
            double p = srcPos_ + i; while (p >= to_) p -= (to_ - from_);
            const int i0 = std::min(frames_ - 1, (int)p);
            tin[i] = src_[(size_t)i0 * 2 + (size_t)c] * win_[(size_t)i];
        }
        kiss_fftr(fft_->fwd, tin, spec);
        for (int k = 0; k <= N / 2; k++) {
            const float mag = std::sqrt(spec[k].r * spec[k].r + spec[k].i * spec[k].i);
            rng_ = rng_ * 1664525u + 1013904223u;
            const uint32_t idx = rng_ >> 20;   // 12 bits
            spec[k].r = mag * t.c[idx]; spec[k].i = mag * t.s[idx];
        }
        spec[0].i = 0; spec[N / 2].i = 0;
        kiss_fftri(fft_->inv, spec, tin);
        float* ola = ola_.data() + (size_t)c * HOP;
        float* out = hopOut_.data() + (size_t)c * HOP;
        for (int i = 0; i < N; i++) tin[i] *= win_[(size_t)i] * (1.f / N);
        for (int i = 0; i < HOP; i++) { out[i] = tin[i] + ola[i]; ola[i] = tin[i + HOP]; }
    }
    const uint32_t w = written_.load(std::memory_order_relaxed);
    for (int i = 0; i < HOP; i++) {
        const size_t k = (size_t)((w + (uint32_t)i) % kFifo) * 2;
        fifo_[k] = hopOut_[(size_t)i]; fifo_[k + 1] = hopOut_[(size_t)HOP + (size_t)i];
    }
    written_.store(w + (uint32_t)HOP, std::memory_order_release);
    srcPos_ += HOP / stretch_; while (srcPos_ >= to_) srcPos_ -= (to_ - from_);
}
