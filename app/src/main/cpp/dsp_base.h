#pragma once
// Definisi dasar yang dipakai bersama oleh compressor.h dan multiband.h.

#include <cmath>

inline float dbToLin(float db) { return std::exp(db * 0.11512925f); }  // ln(10)/20

// Untuk fader: ujung bawah (<= -89 dB) benar-benar senyap, bukan sekadar kecil.
inline float dbToLinSilent(float db) { return db <= -89.f ? 0.f : dbToLin(db); }

struct CompParams {
    float thresholdDb = -20.f;
    float ratio = 4.f;
    float attackMs = 10.f;
    float releaseMs = 200.f;
    float kneeDb = 6.f;
    float makeupDb = 0.f;
};
