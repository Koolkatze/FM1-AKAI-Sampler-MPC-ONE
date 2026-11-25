#include "Sampler.h"
#include <cstring>

Sampler::Sampler(int engineRate) : sr_(engineRate) {}
Sampler::~Sampler() = default;

void Sampler::setSource(const SampleBuf* orig) { allOff(); orig_ = orig; proc_ = nullptr; procA_ = 0; procB_ = 1; nSlices_ = 0; }
void Sampler::setProcessed(const SampleBuf* p, float a, float b) { allOff(); proc_ = p; procA_ = a; procB_ = std::max(a + 0.001f, b); }
void Sampler::setSlices(const int* starts, int n, double end) {
    nSlices_ = std::max(0, std::min(kMaxSlices, n));
    for (int i = 0; i < nSlices_; i++) slices_[i] = starts[i];
    sliceEnd_ = end;
}
void Sampler::setMode(int m) { m = std::min(kModes - 1, std::max(0, m)); if (m != mode_) { allOff(); mode_ = m; } }
void Sampler::setEnv(float a, float d, float s, float r) { atk_ = a; dec_ = d; sus_ = s; rel_ = r; }
void Sampler::setRange(float a, float b) { rangeA_ = clampf(a, 0.f, 1.f); rangeB_ = clampf(b, 0.f, 1.f); if (rangeB_ < rangeA_ + 0.005f) rangeB_ = std::min(1.f, rangeA_ + 0.005f); }

void Sampler::resolveWindow(Voice& v, const SampleBuf*& buf, double& ws, double& we) const {
    if (mode_ == KIT || !proc_) {
        buf = orig_;
        if (mode_ == KIT) {
            if (nSlices_ > 0) {
                const int i = v.slice % nSlices_;
                ws = slices_[i]; we = i + 1 < nSlices_ ? slices_[i + 1] : sliceEnd_;
            } else {   // the slices are not computed yet: 16 equal ones, as the Slicer does when it finds no hits
                const double from = rangeA_ * buf->frames, to = rangeB_ * buf->frames;
                ws = from + (to - from) * v.slice / 16.0; we = from + (to - from) * (v.slice + 1) / 16.0;
            }
        } else { ws = rangeA_ * buf->frames; we = rangeB_ * buf->frames; }
    } else {
        buf = proc_;   // a stretched copy of [procA, procB]: the window is the same range inside it
        const double span = procB_ - procA_;
        ws = clampf((float)((rangeA_ - procA_) / span), 0.f, 1.f) * buf->frames; we = clampf((float)((rangeB_ - procA_) / span), 0.f, 1.f) * buf->frames;
    }
    we = std::min<double>(we, buf->frames - 2); ws = std::min(ws, we - 16);
    if (ws < 0) ws = 0;
}

void Sampler::noteOn(int note, int vel) {
    if (!orig_) return;
    // 0.42.7: first free voice of the mode's pool, otherwise the oldest one (mode-specific limits bound the grain / FFT cost)
    const int limit = voiceLimit();
    int vi = -1; uint32_t oldest = 0xffffffffu;
    for (int i = 0; i < limit; i++) if (!voices_[i].on) { vi = i; break; }
    if (vi < 0) for (int i = 0; i < limit; i++) if (voices_[i].born <= oldest) { oldest = voices_[i].born; vi = i; }
    Voice& v = voices_[vi];
    if (vi < kPaulVoices && paul_[vi]) paul_[vi]->stop();
    v = Voice();
    v.on = true; v.gate = true; v.note = note; v.vel = clampf(vel / 127.f, 0.f, 1.f); v.born = ++bornCounter_;
    v.slice = ((note - kitBase_) % 16 + 16) % 16;
    resolveWindow(v, v.buf, v.ws, v.we);
    if (v.we - v.ws < 2) { v.on = false; return; }
    v.pos = v.ws;
    v.env.set(atk_, dec_, sus_, rel_, (float)sr_);
    if (mode_ == ONESHOT) v.env.set(0.002f, 0.01f, 1.f, 0.01f, (float)sr_);
    v.env.gateOn();
    if (mode_ == PAUL) {
        if (vi >= kPaulVoices || !paul_[vi]) { v.on = false; return; }
        v.paulIdx = vi;
        paul_[vi]->start(v.buf->d.data(), v.buf->frames, v.ws, v.we, paulStretch_);   // the producer thread makes the first hop at once
    }
}
void Sampler::noteOff(int note) {
    if (mode_ == ONESHOT) return;   // always plays out
    for (auto& v : voices_) if (v.on && v.gate && v.note == note) { v.gate = false; v.env.gateOff(); }
}
void Sampler::allOff() { for (auto& v : voices_) v = Voice(); for (auto* p : paul_) if (p) p->stop(); }
void Sampler::releaseAll() { for (auto& v : voices_) if (v.on && v.gate) { v.gate = false; v.env.gateOff(); } }
bool Sampler::hasVoices() const { for (auto& v : voices_) if (v.on) return true; return false; }
int Sampler::voiceLimit() const {
    const int lim = mode_ == PAUL ? kPaulVoices : mode_ == GRAIN ? kGrainVoices : mode_ == KIT ? kKitVoices : kVoices;
    return cap_ > 0 ? std::min(lim, cap_) : lim;
}
int Sampler::activeVoiceCount() const { int a = 0; for (int i = 0; i < voiceLimit(); ++i) a += voices_[i].on; return a; }
void Sampler::setVoiceCap(int cap) {
    if (cap == cap_) return;
    cap_ = std::max(0, cap);
    for (int i = voiceLimit(); i < kVoices; i++) if (voices_[i].on) { if (i < kPaulVoices && paul_[i]) paul_[i]->stop(); voices_[i] = Voice(); }
}

void Sampler::render(float* out, int n) {
    if (!orig_) return;
    const int limit = voiceLimit();
    for (int i = 0; i < limit; ++i) if (voices_[i].on) renderVoice(voices_[i], out, n);
}

void Sampler::renderVoice(Voice& v, float* out, int n) {
    const float* d = v.buf->d.data(); const int frames = v.buf->frames;
    const double srcRate = (double)v.buf->sr / sr_;
    const double pitch = mode_ == KIT ? std::pow(2.0, (kitSemis_ + bend_) / 12.0) : std::pow(2.0, (v.note - root_ + bend_) / 12.0);
    const double rate = srcRate * pitch;
    auto readAt = [&](double p, float& l, float& r) {
        const int i0 = std::min(frames - 2, std::max(0, (int)p)); const float f = (float)(p - i0);
        l = d[(size_t)i0 * 2] + (d[(size_t)i0 * 2 + 2] - d[(size_t)i0 * 2]) * f;
        r = d[(size_t)i0 * 2 + 1] + (d[(size_t)i0 * 2 + 3] - d[(size_t)i0 * 2 + 1]) * f;
    };
    const float gain = 0.8f * v.vel;
    if (mode_ == GRAIN) { renderGrains(v, out, n, rate, gain); return; }
    PaulStream* ps = mode_ == PAUL && v.paulIdx >= 0 && v.paulIdx < kPaulVoices ? paul_[v.paulIdx] : nullptr;
    if (ps) ps->beginRead();
    const double tailScale = 1.0 / (128.0 * rate);
    for (int i = 0; i < n; i++) {
        float l = 0.f, r = 0.f;
        const float e = v.env.next();
        if (v.env.done() && !v.gate) { if (ps) { ps->endRead(); ps->stop(); } v.on = false; return; }
        if (ps) {
            ps->next(pitch, l, r);
            if (paulWidth_ != 1.f) { const float m = 0.5f * (l + r), sd = 0.5f * (l - r) * paulWidth_; l = m + sd; r = m - sd; }
        } else {
            double p = v.pos;
            if (mode_ == LOOPED) {
                const double xf = std::min(256.0, (v.we - v.ws) * 0.25);
                readAt(p, l, r);
                if (p >= v.we - xf) { const double t = (p - (v.we - xf)) / xf; float l2, r2; readAt(v.ws + (p - (v.we - xf)), l2, r2); l = l * (float)(1 - t) + l2 * (float)t; r = r * (float)(1 - t) + r2 * (float)t; }
                v.pos += rate;
                if (v.pos >= v.we) v.pos = v.ws + xf + (v.pos - v.we);
            } else {
                if (p >= v.we) { v.on = false; return; }
                readAt(p, l, r);
                const float tail = (float)std::min(1.0, (v.we - p) * tailScale);   // short fade at the end of the window
                l *= tail; r *= tail;
                v.pos += rate;
            }
        }
        out[2 * i] += l * e * gain; out[2 * i + 1] += r * e * gain;
    }
    if (ps) ps->endRead();
}

// samples a grain can still play: until its length is used up or it would read outside the buffer
int Sampler::grainLife(const Grain& g, int frames) const {
    int life = g.len - g.age;
    if (g.pos < 1 || g.pos >= frames - 2) return 0;
    if (g.inc > 0) life = std::min<double>(life, std::ceil((frames - 2 - g.pos) / g.inc));
    else if (g.inc < 0) life = std::min<double>(life, std::floor((g.pos - 1) / -g.inc) + 1);
    return std::max(0, life);
}

void Sampler::renderGrains(Voice& v, float* out, int n, double rate, float gain) {
    const float* d = v.buf->d.data(); const int frames = v.buf->frames;
    constexpr int kChunk = 64;
    float env[kChunk], tl[kChunk], tr[kChunk];
    for (int base = 0; base < n; base += kChunk) {
        const int m = std::min(kChunk, n - base);
        // the envelope first: the voice ends at the sample where it has finished its release
        int end = m; bool ends = false;
        for (int i = 0; i < m; i++) { env[i] = v.env.next(); if (v.env.done() && !v.gate) { end = i; ends = true; break; } }
        std::fill(tl, tl + end, 0.f); std::fill(tr, tr + end, 0.f);
        auto run = [&](Grain& g, int from, int to) {   // one grain over [from, to) of this chunk
            double c0 = g.c0, c1 = g.c1, pos = g.pos; const double k = g.k, inc = g.inc;
            for (int j = from; j < to; j++) {
                const float w = (float)(0.5 - 0.5 * c0);
                const double c2 = k * c0 - c1; c1 = c0; c0 = c2;
                const int i0 = (int)pos; const float f = (float)(pos - i0);
                const float* s = d + (size_t)i0 * 2;
                tl[j] += (s[0] + (s[2] - s[0]) * f) * w; tr[j] += (s[1] + (s[3] - s[1]) * f) * w;
                pos += inc;
            }
            g.c0 = c0; g.c1 = c1; g.pos = pos; g.age += to - from;
        };
        int startAt[12], dieAt[12];   // where each slot's current grain starts and stops in this chunk (-1: slot free)
        for (int s = 0; s < 12; s++) { startAt[s] = 0; dieAt[s] = v.grain[s].on ? grainLife(v.grain[s], frames) : -1; }
        // the cursor and the spawns, sample by sample as the phone does (same random numbers, in the same order)
        for (int i = 0; i < end; i++) {
            v.spawn -= 1.0;
            v.pos += (v.we - v.ws) / (4.0 * sr_); if (v.pos >= v.we) v.pos = v.ws;
            if (v.spawn > 0.0) continue;
            for (int s = 0; s < 12; s++) {
                if (dieAt[s] >= i) continue;   // still sounding at this sample
                Grain& g = v.grain[s];
                if (g.on) { run(g, startAt[s], std::min(dieAt[s], end)); g.on = false; }   // the grain that ended earlier in this chunk
                g.on = true; g.age = 0; g.len = std::max(2, (int)(sr_ * grainLen_ * (0.5f + rnd())));
                { const double w = 2.0 * 3.141592653589793 / g.len; g.k = 2.0 * std::cos(w); g.c0 = std::cos(0.5 * w); g.c1 = g.c0; }
                const float kq = rnd(); const double mult = kq < 0.15 ? 0.5 : kq < 0.3 ? 2.0 : 1.0;
                g.inc = rate * mult * (1.0 + (rnd() - 0.5) * 0.004) * (rnd() < 0.2f ? -1.0 : 1.0);
                // the cursor sweeps the window in about four seconds; scatter is how far around it a grain may start (1 = anywhere in the window)
                const double win = std::max(1.0, v.we - v.ws);
                double p = v.pos + (rnd() - 0.5) * grainScatter_ * win;
                p = std::min(v.we - g.len * std::fabs(g.inc) - 1, std::max(v.ws, p));
                g.pos = g.inc > 0 ? p : std::min(v.we - 1, p + g.len * std::fabs(g.inc));
                startAt[s] = i; dieAt[s] = i + grainLife(g, frames);
                break;
            }
            v.spawn = sr_ * (double)grainLen_ / grainN_ * (0.6 + 0.8 * rnd());
        }
        for (int s = 0; s < 12; s++) {
            Grain& g = v.grain[s];
            if (!g.on) continue;
            const int to = std::min(dieAt[s], end);
            if (to > startAt[s]) run(g, startAt[s], to);
            if (dieAt[s] <= end) g.on = false;
        }
        float* o = out + 2 * base;
        const float gg = 0.5f * gain;
        for (int i = 0; i < end; i++) { o[2 * i] += tl[i] * env[i] * gg; o[2 * i + 1] += tr[i] * env[i] * gg; }
        if (ends) { v.on = false; return; }
    }
}
