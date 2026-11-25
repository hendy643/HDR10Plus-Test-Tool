# hdr10pg

A Windows tool for checking HDR displays. It:

- reports each display's HDR10, HDR10+ and HDR10+ GAMING support;
- turns on the HDR10+ GAMING signal while its DirectX 12 test window is open;
- shows test screens for peak brightness, colour gradients and bit depth.

It follows the *HDR10+ GAMING Developer Guideline* v1.2 from HDR10+ Technologies. It is not a
certification tool.

## Requirements

**To build**

- Windows 10 or 11, x64
- Visual Studio with the C++ desktop workload (MSVC and the Windows SDK)
- CMake 3.24 or newer, and Git
- Network access on the first configure: NVAPI and the Intel Graphics Control Library are downloaded at
  pinned commits, under their own licences

**To use HDR output**

- An HDR display, with **Use HDR** on in Windows display settings
- An NVIDIA GPU (driver R525 or newer) or an Intel GPU

**For HDR10+ GAMING, additionally**

- A certified HDR10+ GAMING display, connected by **HDMI**. It does not work over DisplayPort.
- The display's Game Mode and HDR10+ GAMING settings turned on
- On Intel: Arc A750/A770 or newer, or Meteor Lake or newer

AMD GPUs are not detected. The Intel code compiles but has not been tested on Intel hardware.

## Build

Run from the project folder:

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The program is `build\Release\hdr10pg.exe`. For another Visual Studio version, change the name after `-G`
(`cmake --help` lists the names your CMake knows). Without `-G`, some CMake installs choose Ninja, which
rejects `-A x64`.

## Quick start

```
build\Release\hdr10pg.exe detect     # what does my display support?
build\Release\hdr10pg.exe            # open the test window
```

## Commands

| Command | What it does |
|---|---|
| `hdr10pg [display]` | Opens the test window. HDR10+ GAMING is on while it is open, if the display supports it. |
| `hdr10pg detect` | Lists the displays and their capabilities, then exits. |
| `hdr10pg fullscreen [display]` | 10-bit HDR CMYK gradient, fullscreen. |
| `hdr10pg fullscreen [display] --green` | 10-bit HDR bit depth test, black to green. |
| `hdr10pg fullscreen [display] --8bit` | CMYK gradient in 8-bit SDR. Add `--green` for the green ramp. |
| `hdr10pg sdr-image` | Writes `cmyk_sdr_8bit.bmp`. |
| `hdr10pg sdr-image --green` | Writes `green_sdr_8bit.bmp`. |
| `hdr10pg disable [display]` | Intel only: resets a signal left on after the program was killed. |
| `hdr10pg selftest` | Checks the colour math and test patterns. Needs no display. |

`display` is the number in square brackets printed by `detect`. Without it, the first HDR10+ GAMING
display is used, or the first display if none qualifies.

The image files are 8-bit sRGB at the primary monitor's resolution. They are written to the current
folder and replace earlier copies.

## Keys

| Key | Action |
|---|---|
| Up / Down | Test peak up or down by 10 nits |
| Page Up / Page Down | Test peak up or down by 100 nits |
| F11 | Switch between window and borderless fullscreen |
| Esc | Close |

The test peak ranges from 100 to 10000 nits. It has no effect on the 8-bit screens.

It starts at the display's standard HDR maximum luminance, or 1000 nits if that is unknown. This is
**not** the HDR10+ GAMING tone mapping target (see [Limitations](#limitations)).

## Reading the `detect` report

| Line | Meaning |
|---|---|
| HDR10 / HDR10+ supported | What the display advertises to the driver. Intel reports HDR10+ only as a certification flag. |
| HDR10+ GAMING supported | `yes` only if the signal can be used. Otherwise the reason is given where known. |
| Connection | HDMI, DisplayPort or other, as Windows reports it (NVIDIA). |
| Windows HDR | Whether **Use HDR** is on. It must be on to enable HDR10+ GAMING. |
| Output mode | The driver's current mode: SDR, HDR10 or HDR10+ GAMING. |
| GPU output depth | Bits per channel sent down the cable. It says nothing about the panel. |
| VSVDB values | Raw HDR10+ GAMING data from the display. |
| Standard HDR max luminance | The display's ordinary HDR maximum, in nits. |

Some lines appear only for NVIDIA displays.

## Test screens

None of these measure light output. Judge them by eye on the display; screenshots do not preserve HDR
brightness.

### Test window

- **Grey bars (top):** 16 steps from black to the test peak. Each is split into five strips one 10-bit code
  apart. On a 10-bit path a bar should look nearly flat. On an 8-bit path without dithering, a hard edge
  appears inside it. Bars above the display's maximum clip together and show nothing.
- **Square (bottom):** a patch at the test peak, whose value is written beside it, inside a 10000 nit
  surround. Raise the peak until the patch disappears into the surround to find where the display clips.

The window outputs HDR10 (10-bit PQ, BT.2020), with or without the HDR10+ GAMING signal.

### CMYK gradient

Cyan (top left), magenta (top right), yellow (bottom left) and black (bottom right) sit on the exact
corner pixels, with a smooth blend between them. The HDR version shows full-scale sRGB colours at the test
peak, carried in BT.2020. It is recalculated when the peak changes, which can take a moment.

### Green bit depth test

A horizontal ramp from black to green. Only the BT.2020 green channel is driven. A thin line divides it:

- **Top half:** every 10-bit code.
- **Bottom half:** the same ramp rounded to 256 levels, as a reference for 8-bit banding.

| What you see | What it suggests |
|---|---|
| Top is finer or smoother than bottom | More than 8 bits are getting through. |
| Both halves band alike | Something limits the path to about 8 bits: GPU setting, cable mode or monitor. |
| Both look smooth | Lower the peak to widen the bands, and look closely at the darker part. |

With `--8bit` the screen is a single 8-bit ramp to sRGB green. That green and its brightness differ from the
10-bit screen, so do not compare the two directly.

**Smooth results do not prove a native 10-bit panel.** A GPU or monitor can dither an 8-bit signal or panel
and hide the steps.

## HDR10+ GAMING

To enable it:

1. Check that `detect` shows `HDR10+ GAMING supported: yes`.
2. Run `hdr10pg`. The title bar shows `HDR10+ GAMING ON`.
3. Confirm on the display. On a Samsung TV, hold Play/Pause to open the Game Menu Bar.

Closing the window, or pressing Ctrl+C in the console, restores the previous state. The display leaves the
mode about two seconds after the signal stops. The `fullscreen` HDR screens enable it too; the `--8bit`
screens do not.

On NVIDIA the signal also ends if the program is killed. On Intel the setting can stay on; run
`hdr10pg disable`.

## Troubleshooting

| Problem | Check |
|---|---|
| `needs an HDMI connection` | Use HDMI instead of DisplayPort. |
| `needs a certified display with Game Mode on` | Confirm the model is certified and turn Game Mode on. |
| The mode does not switch on | Turn on Windows **Use HDR**, then restart the program. It is checked only at startup. |
| `Intel: control library unavailable` | Expected on a PC without a working Intel graphics driver. |
| The test window stays on another monitor (Intel) | Drag it to the target display. |

## Limitations

- The display's `peak_luminance_index` is printed as a raw number. Neither the guideline nor the NVAPI header
  gives the table that converts it to nits; ask HDR10+ Technologies (info@hdr10plus.org).
- HDR10+ is detected only. No HDR10+ dynamic metadata is sent.
- The program does not turn Windows HDR on or off.

## Using the code in a game

| File | Contents |
|---|---|
| `src/displays.cpp` | Detection and enabling through NVAPI and the Intel library |
| `src/test_window.cpp` | Win32 window, DirectX 12 swap chain and drawing |
| `src/pq.h`, `src/test_image.h`, `src/text.h` | PQ curve, test patterns and label font |
| `src/main.cpp` | Command line and self-test |

A game that reuses this code must follow the [license](#license). It makes the same enabling calls itself; on NVIDIA the mode lasts only as long as the process that set
it. The game must also output 10-bit PQ in BT.2020 and tone map to the display's declared HDR10+ GAMING
peak luminance, not its standard HDR maximum (guideline section 2.5).

## License

Copyright 2026 hendy643. Licensed under the [Apache License 2.0](LICENSE).

You may use, change and share this project, including commercially. When you share it or work based on it,
you must include the license, keep the copyright notices, carry over the attribution in [NOTICE](NOTICE),
and mark the files you changed. The license grants no right to use the author's names or trademarks, and the
software comes without warranty. The `LICENSE` file is what applies; this is only a summary.

**Third-party code:** NVAPI and the Intel Graphics Control Library keep their own licenses. Intel's license
text limits use and redistribution of its software to Intel platforms, and the built program includes
Intel's wrapper code. Review those terms before distributing a binary.

HDR10+ and HDR10+ GAMING names and logos belong to their owner. This license gives no right to use them.
