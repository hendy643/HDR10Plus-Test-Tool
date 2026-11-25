#pragma once

#include "displays.h"
#include "test_image.h"

#include <optional>

struct FullscreenImage {
    Pattern pattern;
    bool eightBit; // an 8-bit sRGB swap chain instead of 10-bit HDR10
};

// Opens the D3D12 HDR10 test window and blocks until it closes. If `display` is supported, the
// HDR10+ GAMING signal is on while the window is open. `display` may be null.
// With `image` it starts fullscreen on that gradient instead of the luminance pattern; an 8-bit
// image never turns the HDR10+ GAMING signal on.
// Returns a process exit code; throws std::runtime_error if Win32 or D3D12 setup or rendering fails.
int runTestWindow(HINSTANCE instance, int showCommand, const Display* display,
                  std::optional<FullscreenImage> image);
