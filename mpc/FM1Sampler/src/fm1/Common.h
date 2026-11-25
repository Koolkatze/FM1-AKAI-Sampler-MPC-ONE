// Small shared helpers of the DSP code.
// MPC port: adapted from engine/dsp/Common.h. The MPC host runs at a fixed 44.1 kHz.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef SAMPLERATE
#define SAMPLERATE 44100   // MPC standalone: fixed 44.1 kHz (the FM-1 / Android build also uses 44100)
#endif

constexpr float kPi = 3.14159265358979f;
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
inline float dbToGain(float db) { return std::pow(10.f, db / 20.f); }

// tanh by a [7/6] Pade approximant, clamped: |error| < 1e-4 (-80 dB) everywhere. The phone build calls std::tanh per sample,
// which is one of the most expensive calls on the Cortex-A17 of an MPC One.
inline float ftanh(float x) {
    x = clampf(x, -4.97f, 4.97f);
    const float x2 = x * x;
    const float a = x * (135135.f + x2 * (17325.f + x2 * (378.f + x2)));
    const float b = 135135.f + x2 * (62370.f + x2 * (3150.f + x2 * 28.f));
    return clampf(a / b, -1.f, 1.f);
}
