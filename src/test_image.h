// The gradient test images, shared by the 10-bit HDR and 8-bit SDR screens and the SDR files.
#pragma once

#include "pq.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using Rgb = std::array<float, 3>;

// cmyk: cyan top left, magenta top right, yellow bottom left, black bottom right.
// green: black at the left edge to pure green at the right edge.
enum class Pattern { cmyk, green };

// sRGB-encoded colour of a pixel.
inline Rgb gradient(Pattern pattern, LONG x, LONG y, LONG width, LONG height) {
    const float u = width > 1 ? static_cast<float>(x) / (width - 1) : 0.0f;
    const float v = height > 1 ? static_cast<float>(y) / (height - 1) : 0.0f;
    if (pattern == Pattern::green) return {0, u, 0};
    // The bilinear blend of the four corner colours, written out per channel.
    return {u * (1 - v) + v * (1 - u), 1 - u, 1 - v};
}

// An R8G8B8A8 pixel holding the sRGB values.
inline uint32_t sdrPixel(const Rgb& srgb) {
    uint32_t pixel = 0xFFu << 24; // opaque alpha
    for (int i = 0; i < 3; ++i) pixel |= static_cast<uint32_t>(srgb[i] * 255 + 0.5f) << (8 * i);
    return pixel;
}

inline float linearFromSrgb(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// BT.2020 is only the transport: the colours stay inside BT.709 (ITU-R BT.2087 matrix).
inline Rgb bt709ToBt2020(const Rgb& c) {
    return {0.6274f * c[0] + 0.3293f * c[1] + 0.0433f * c[2],
            0.0691f * c[0] + 0.9195f * c[1] + 0.0114f * c[2],
            0.0164f * c[0] + 0.0880f * c[1] + 0.8956f * c[2]};
}

// The same colour as an R10G10B10A2 HDR10 pixel, with full-scale sRGB at `peakNits`.
inline uint32_t hdr10Pixel(const Rgb& srgb, float peakNits) {
    const Rgb c = bt709ToBt2020({linearFromSrgb(srgb[0]), linearFromSrgb(srgb[1]), linearFromSrgb(srgb[2])});
    uint32_t pixel = 3u << 30; // opaque alpha
    for (int i = 0; i < 3; ++i)
        pixel |= static_cast<uint32_t>(pqFromNits(c[i] * peakNits) * 1023 + 0.5f) << (10 * i);
    return pixel;
}

// A pixel of the 10-bit screen. Green is a bit depth test, so it drives only the green channel (the
// BT.2020 primary), linearly in PQ signal from 0 to the code for `peakNits`:
//   top half:    every 10-bit code, which a true 10-bit path shows as the finer steps;
//   bottom half: the same ramp rounded to 256 levels, to show what 8-bit stepping looks like.
// If both halves band alike, something between this buffer and your eye is limited to about 8 bits.
// A thin black line separates them.
inline uint32_t hdrScreenPixel(Pattern pattern, LONG x, LONG y, LONG width, LONG height, float peakNits) {
    const Rgb rgb = gradient(pattern, x, y, width, height);
    if (pattern != Pattern::green) return hdr10Pixel(rgb, peakNits);

    const uint32_t black = 3u << 30;
    if (y >= height / 2 - 2 && y < height / 2 + 2) return black;
    float signal = rgb[1] * pqFromNits(peakNits);
    if (y >= height / 2) signal = static_cast<uint32_t>(signal * 255 + 0.5f) / 255.0f;
    return black | static_cast<uint32_t>(signal * 1023 + 0.5f) << 10;
}

// The luminance pattern's grey bars. Each is split into strips one 10-bit code apart, which look
// nearly flat on a 10-bit path. Five consecutive codes are wider than one 8-bit step, so an 8-bit
// path without dithering shows a hard edge inside every bar.
constexpr int greyBars = 16;
constexpr int greyStrips = 5;

// The 10-bit code of a strip. Bars run from black to the code for `peakNits`.
inline int greyBarCode(int bar, int strip, float peakNits) {
    const int top = static_cast<int>(pqFromNits(peakNits) * 1023 + 0.5f);
    const int base = (top * bar + (greyBars - 1) / 2) / (greyBars - 1);
    return std::min(base, 1023 - (greyStrips - 1)) + strip;
}

// Writes the SDR version as a 24-bit (8 bits per channel) sRGB BMP.
inline bool writeSdrImage(const char* path, Pattern pattern, LONG width, LONG height) {
    const size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~size_t{3};
    std::vector<unsigned char> pixels(stride * height);
    for (LONG y = 0; y < height; ++y)
        for (LONG x = 0; x < width; ++x) {
            const uint32_t rgb = sdrPixel(gradient(pattern, x, y, width, height));
            // BMP rows run bottom to top, and pixels are stored blue first.
            unsigned char* out = &pixels[stride * (height - 1 - y) + 3 * static_cast<size_t>(x)];
            for (int c = 0; c < 3; ++c) out[c] = static_cast<unsigned char>(rgb >> (8 * (2 - c)));
        }

    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 24;
    BITMAPFILEHEADER file = {};
    file.bfType = 0x4D42; // "BM"
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0) return false;
    const bool written = fwrite(&file, sizeof(file), 1, f) == 1 && fwrite(&info, sizeof(info), 1, f) == 1 &&
                         fwrite(pixels.data(), 1, pixels.size(), f) == pixels.size();
    return fclose(f) == 0 && written;
}
