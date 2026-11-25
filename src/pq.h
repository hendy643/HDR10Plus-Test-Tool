#pragma once

#include <cmath>

// SMPTE ST 2084 inverse EOTF: luminance in nits to a 0..1 PQ signal.
inline float pqFromNits(float nits) {
    constexpr float m1 = 2610.0f / 16384, m2 = 2523.0f / 4096 * 128;
    constexpr float c1 = 3424.0f / 4096, c2 = 2413.0f / 4096 * 32, c3 = 2392.0f / 4096 * 32;
    const float y = std::pow(nits / 10000.0f, m1);
    return std::pow((c1 + c2 * y) / (1 + c3 * y), m2);
}
