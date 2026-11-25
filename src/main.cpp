#include "displays.h"
#include "test_image.h"
#include "test_window.h"
#include "text.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>

// A GUI program has no console of its own. Output that is already redirected is left alone;
// otherwise borrow the console of the shell that started us, if there is one.
static void attachParentConsole() {
	const bool needOut = _fileno(stdout) < 0, needError = _fileno(stderr) < 0;
	if ((!needOut && !needError) || !AttachConsole(ATTACH_PARENT_PROCESS)) return;
	FILE* stream = nullptr;
	if (needOut) freopen_s(&stream, "CONOUT$", "w", stdout);
	if (needError) freopen_s(&stream, "CONOUT$", "w", stderr);
}

// Prints a failure, and shows it in a dialog when there is nowhere to print (started from Explorer).
static void fail(const char* message) {
	if (_fileno(stdout) >= 0)
		printf("%s\n", message);
	else
		MessageBoxA(nullptr, message, "hdr10pg", MB_OK | MB_ICONERROR);
}

static int usage() {
	if (_fileno(stdout) < 0) fail("Unknown arguments. Run hdr10pg from a terminal; see README.md.");
	printf("Usage: hdr10pg [display]           open the DX12 test window with HDR10+ GAMING on\n"
		"       hdr10pg fullscreen [display] [--green] [--8bit]\n"
		"                                   gradient test screen: CMYK or green, 10-bit HDR or 8-bit SDR\n"
		"       hdr10pg sdr-image [--green]  write the 8-bit SDR gradient to a .bmp file\n"
		"       hdr10pg detect              list displays and exit\n"
		"       hdr10pg disable [display]   reset an Intel output left enabled\n"
		"       hdr10pg selftest            check the PQ curve\n");
	return 2;
}

static int selftest() {
	// Reference points of the ST 2084 curve.
	const struct { float nits, signal; } points[] = { { 0, 0.0f },{ 100, 0.5081f },{ 1000, 0.7518f },{ 10000, 1.0f } };
	for (const auto& p : points) {
		const float got = pqFromNits(p.nits);
		if (std::fabs(got - p.signal) > 0.0005f) {
			printf("PQ(%g) = %f, expected %f\n", p.nits, got, p.signal);
			return 1;
		}
	}
	// White must survive the gamut conversion unchanged.
	for (const float channel : bt709ToBt2020({ 1, 1, 1 }))
		if (std::fabs(channel - 1) > 0.0005f) {
			printf("BT.709 white became %f in BT.2020\n", channel);
			return 1;
		}
	// The gradient must hit cyan, magenta, yellow and black exactly on the corner pixels.
	const struct { LONG x, y; Rgb rgb; } corners[] = {
		{ 0, 0,{ 0, 1, 1 } },{ 639, 0,{ 1, 0, 1 } },{ 0, 479,{ 1, 1, 0 } },{ 639, 479,{ 0, 0, 0 } } };
	for (const auto& c : corners)
		if (gradient(Pattern::cmyk, c.x, c.y, 640, 480) != c.rgb) {
			printf("Wrong gradient colour at corner %ld,%ld\n", c.x, c.y);
			return 1;
		}
	// The bit depth screen: only green is driven, and the top half resolves far more steps than 8 bits.
	std::set<uint32_t> fine, coarse;
	for (LONG x = 0; x < 2560; ++x) {
		const uint32_t top = hdrScreenPixel(Pattern::green, x, 0, 2560, 1440, 1000);
		const uint32_t bottom = hdrScreenPixel(Pattern::green, x, 1439, 2560, 1440, 1000);
		if ((top | bottom) & 0x3FF003FFu) {
			printf("Red or blue is not zero in the green gradient at column %ld\n", x);
			return 1;
		}
		fine.insert(top);
		coarse.insert(bottom);
	}
	if (fine.size() <= 256 || coarse.size() > 256) {
		printf("Green gradient steps: %zu in 10-bit, %zu in the 8-bit reference\n", fine.size(), coarse.size());
		return 1;
	}
	// Every character a label can contain must have a glyph.
	for (const char* c = "0123456789nits"; *c; ++c) {
		const char one[] = { *c, '\0' };
		if (textRects(one, 0, 0, 1).empty()) {
			printf("No glyph for '%c'\n", *c);
			return 1;
		}
	}
	// Every grey bar must change level when cut to 8 bits, whether by truncating or by rounding.
	for (const float peak : {100.0f, 496.0f, 1000.0f, 10000.0f})
		for (int bar = 0; bar < greyBars; ++bar) {
			const int first = greyBarCode(bar, 0, peak), last = greyBarCode(bar, greyStrips - 1, peak);
			if (first < 0 || last > 1023 || first / 4 == last / 4 ||
				(first * 255 + 511) / 1023 == (last * 255 + 511) / 1023) {
				printf("Grey bar %d at %g nits (codes %d-%d) has no 8-bit edge\n", bar, peak, first, last);
				return 1;
			}
		}
	printf("selftest passed\n");
	return 0;
}

int WINAPI WinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int showCommand) {
	attachParentConsole();
	int argc = __argc;
	char** argv = __argv;

	// Take the two flags out first; what is left is a command and perhaps a display number.
	FullscreenImage image = { Pattern::cmyk, false };
	int kept = 1;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--green") image.pattern = Pattern::green;
		else if (arg == "--8bit") image.eightBit = true;
		else argv[kept++] = argv[i];
	}
	argc = kept;

	int next = 1;
	std::string command = "run";
	if (argc > 1) {
		const std::string first = argv[1];
		if (first == "detect" || first == "disable" || first == "selftest" || first == "fullscreen" ||
			first == "sdr-image") {
			command = first;
			next = 2;
		}
	}

	int wanted = -1; // -1: first supported display
	if (argc > next) {
		char* end = nullptr;
		const long n = std::strtol(argv[next], &end, 10);
		if (argc > next + 1 || command == "detect" || command == "selftest" || command == "sdr-image" ||
			end == argv[next] || *end || n < 0 || n > 1000)
			return usage();
		wanted = static_cast<int>(n);
	}
	const bool green = image.pattern == Pattern::green;
	if ((green && command != "fullscreen" && command != "sdr-image") || (image.eightBit && command != "fullscreen"))
		return usage();
	if (command == "selftest") return selftest();
	if (command == "sdr-image") {
		const char* path = green ? "green_sdr_8bit.bmp" : "cmyk_sdr_8bit.bmp";
		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // real pixels, not scaled
		const int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
		if (width <= 0 || height <= 0 || !writeSdrImage(path, image.pattern, width, height)) {
			printf("Cannot write %s\n", path);
			return 1;
		}
		printf("Wrote %s (%dx%d)\n", path, width, height);
		return 0;
	}

	const std::vector<Display> displays = enumerateDisplays();
	for (size_t i = 0; i < displays.size(); ++i)
		printf("[%zu] %s\n", i, displays[i].description.c_str());
	if (displays.empty()) printf("No NVIDIA or Intel display outputs found.\n");
	if (command == "detect") return 0;

	if (wanted >= static_cast<int>(displays.size())) {
		fail("There is no display with that number.");
		return 1;
	}
	int target = wanted;
	for (size_t i = 0; target < 0 && i < displays.size(); ++i)
		if (displays[i].supported) target = static_cast<int>(i);
	// Otherwise test on the first display, so that the starting peak is its own maximum.
	if (target < 0 && !displays.empty()) target = 0;
	const Display* display = target < 0 ? nullptr : &displays[target];

	if (command == "disable") {
		if (!display || !display->supported) {
			printf("No HDR10+ GAMING display to disable.\n");
			return 1;
		}
		if (!display->reset) {
			printf("Nothing to reset: NVIDIA releases the override when the enabling process exits.\n");
			return 0;
		}
		if (!display->reset()) return 1;
		printf("Display %d reset to driver defaults.\n", target);
		return 0;
	}

	if (image.eightBit)
		printf("8-bit SDR screen.\n");
	else if (!display || !display->supported)
		printf("No HDR10+ GAMING support on the chosen display.\n");
	else if (!display->monitor)
		printf("Drag the window onto display %d.\n", target);

	try {
		std::optional<FullscreenImage> screen;
		if (command == "fullscreen") screen = image;
		return runTestWindow(instance, showCommand, display, screen);
	}
	catch (const std::exception& e) {
		fail(e.what());
		return 1;
	}
}
