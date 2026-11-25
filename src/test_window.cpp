// Win32 window with a D3D12 swap chain (10-bit PQ / BT.2020, or 8-bit sRGB) showing the test screens.
#include "test_window.h"
#include "test_image.h"
#include "text.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char message[128];
        snprintf(message, sizeof(message), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}

// Holds the HDR10+ GAMING signal and always gives it back, including when rendering throws.
struct Signal {
    const Display* display = nullptr;
    bool on = false;

    void enable() {
        on = display && display->supported && display->enable();
    }
    void disable() {
        if (!on) return;
        on = false;
        display->restore();
        printf("HDR10+ GAMING signal restored.\n");
        fflush(stdout);
    }
    ~Signal() { disable(); }
};

struct Window {
    Signal signal;
    unsigned peakNits = 1000;
    bool eightBit = false;
    UINT width = 0, height = 0;
    bool resized = false;
    bool titleStale = true;
    WINDOWPLACEMENT windowed = {sizeof(WINDOWPLACEMENT)};
};

void toggleFullscreen(HWND hwnd, Window& w) {
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO monitor = {sizeof(monitor)};
        if (!GetWindowPlacement(hwnd, &w.windowed) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
            return;
        const RECT& r = monitor.rcMonitor;
        SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &w.windowed);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w) return DefWindowProcW(hwnd, message, wParam, lParam);

    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            w->width = LOWORD(lParam);
            w->height = HIWORD(lParam);
            w->resized = true;
        }
        return 0;
    case WM_KEYDOWN: {
        int step = 0;
        switch (wParam) {
        case VK_UP: step = 10; break;
        case VK_DOWN: step = -10; break;
        case VK_PRIOR: step = 100; break;
        case VK_NEXT: step = -100; break;
        case VK_F11: toggleFullscreen(hwnd, *w); return 0;
        case VK_ESCAPE: PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        default: return 0;
        }
        w->peakNits = static_cast<unsigned>(std::clamp(static_cast<int>(w->peakNits) + step, 100, 10000));
        w->titleStale = true;
        return 0;
    }
    case WM_ENDSESSION: // the process is killed after this returns
        if (wParam) w->signal.disable();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

constexpr UINT bufferCount = 2;

class Renderer {
public:
    Renderer(HWND hwnd, UINT width, UINT height, bool eightBit)
        : format(eightBit ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM) {
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

        const D3D12_COMMAND_QUEUE_DESC queueDesc = {D3D12_COMMAND_LIST_TYPE_DIRECT};
        check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = bufferCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> chain;
        check(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc, nullptr, nullptr, &chain),
              "CreateSwapChainForHwnd");
        check(chain.As(&swapChain), "IDXGISwapChain3");
        check(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
        check(swapChain->SetColorSpace1(eightBit ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                                                 : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020),
              "SetColorSpace1");

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, bufferCount};
        check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap");
        createTargets();

        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
              "CreateCommandAllocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list)),
              "CreateCommandList");
        check(list->Close(), "Close");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    }

    // Every frame ends with the GPU idle, so the buffers are free to be released or rewritten.
    void resize(UINT width, UINT height) {
        for (auto& target : targets) target.Reset();
        check(swapChain->ResizeBuffers(bufferCount, width, height, format, 0), "ResizeBuffers");
        createTargets();
    }

    // Copies a gradient, edge to edge, into the back buffer.
    // Returns false when the window is occluded.
    bool drawGradient(Pattern pattern, float peakNits) {
        const UINT index = begin();
        const D3D12_RESOURCE_DESC target = targets[index]->GetDesc();
        if (!upload || target.Width != imageWidth || target.Height != imageHeight || peakNits != imagePeak)
            buildGradient(target, pattern, peakNits);

        transition(index, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to = {targets[index].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION from = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        from.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(index, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        return present();
    }

    // Draws the luminance pattern (10-bit only). Returns false when the window is occluded.
    bool draw(LONG width, LONG height, float peakNits) {
        const UINT index = begin();
        transition(index, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        // Clears write PQ code values straight into the buffer, so no shaders are needed.
        const D3D12_CPU_DESCRIPTOR_HANDLE view = viewOf(index);
        const auto fill = [&](const D3D12_RECT* rect, float nits) {
            const float v = pqFromNits(nits);
            const float color[4] = {v, v, v, 1};
            list->ClearRenderTargetView(view, color, rect ? 1 : 0, rect);
        };
        fill(nullptr, 0);

        // Top: grey steps from black to the test peak, evenly spaced in PQ signal, each holding a
        // mini gradient that tests the bit depth.
        for (int i = 0; i < greyBars; ++i) {
            const LONG left = width * i / greyBars, right = width * (i + 1) / greyBars;
            for (int s = 0; s < greyStrips; ++s) {
                const D3D12_RECT strip = {left + (right - left) * s / greyStrips, height / 20,
                                          left + (right - left) * (s + 1) / greyStrips, height * 3 / 10};
                const float signal = greyBarCode(i, s, peakNits) / 1023.0f;
                const float color[4] = {signal, signal, signal, 1};
                list->ClearRenderTargetView(view, color, 1, &strip);
            }
        }

        // Bottom: a patch at the test peak inside a 10000 nit surround. It vanishes once the test
        // peak reaches the level where the display clips.
        const D3D12_RECT surround = {width / 4, height * 2 / 5, width * 3 / 4, height * 9 / 10};
        const D3D12_RECT patch = {width * 2 / 5, height * 21 / 40, width * 3 / 5, height * 31 / 40};
        fill(&surround, 10000);
        fill(&patch, peakNits);

        // The patch's luminance, to the right of the square. Sized to fit the quarter of the window there.
        char label[16];
        snprintf(label, sizeof(label), "%.0f nits", peakNits);
        const LONG cell = std::max<LONG>(1, std::min(height / 140, width / 4 / 64));
        const auto text = textRects(label, surround.right + 4 * cell, (patch.top + patch.bottom - 7 * cell) / 2, cell);
        const float grey = pqFromNits(200); // readable without competing with the highlights
        const float textColor[4] = {grey, grey, grey, 1};
        // One call per rectangle: passing the whole array in one call crashed the process on NVIDIA.
        for (const RECT& dot : text) list->ClearRenderTargetView(view, textColor, 1, &dot);

        transition(index, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return present();
    }

private:
    UINT begin() {
        check(allocator->Reset(), "allocator Reset");
        check(list->Reset(allocator.Get(), nullptr), "list Reset");
        return swapChain->GetCurrentBackBufferIndex();
    }

    // Computes the image on the CPU, once per size or peak, into an upload buffer.
    void buildGradient(const D3D12_RESOURCE_DESC& target, Pattern pattern, float peakNits) {
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&target, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);

        const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(upload.ReleaseAndGetAddressOf())),
              "CreateCommittedResource");

        unsigned char* data = nullptr;
        const D3D12_RANGE noRead = {};
        check(upload->Map(0, &noRead, reinterpret_cast<void**>(&data)), "Map");
        const LONG width = static_cast<LONG>(target.Width), height = static_cast<LONG>(target.Height);
        for (LONG y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(data + footprint.Offset +
                                                    static_cast<size_t>(y) * footprint.Footprint.RowPitch);
            for (LONG x = 0; x < width; ++x)
                row[x] = format == DXGI_FORMAT_R8G8B8A8_UNORM
                             ? sdrPixel(gradient(pattern, x, y, width, height))
                             : hdrScreenPixel(pattern, x, y, width, height, peakNits);
        }
        upload->Unmap(0, nullptr);

        imageWidth = target.Width;
        imageHeight = target.Height;
        imagePeak = peakNits;
    }

    bool present() {
        check(list->Close(), "Close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);

        const HRESULT presented = swapChain->Present(1, 0);
        check(presented, "Present");
        waitForGpu();
        return presented != DXGI_STATUS_OCCLUDED;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE viewOf(UINT index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) *
                      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        return handle;
    }

    void createTargets() {
        for (UINT i = 0; i < bufferCount; ++i) {
            check(swapChain->GetBuffer(i, IID_PPV_ARGS(&targets[i])), "GetBuffer");
            device->CreateRenderTargetView(targets[i].Get(), nullptr, viewOf(i));
        }
    }

    void transition(UINT index, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {targets[index].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &barrier);
    }

    // shortcut: one frame in flight; add per-frame allocators and fences if real rendering goes in here.
    void waitForGpu() {
        check(queue->Signal(fence.Get(), ++fenceValue), "Signal");
        // A null event makes this call block until the fence reaches the value.
        check(fence->SetEventOnCompletion(fenceValue, nullptr), "SetEventOnCompletion");
    }

    const DXGI_FORMAT format;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> targets[bufferCount];
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    ComPtr<ID3D12Resource> upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 imageWidth = 0;
    UINT imageHeight = 0;
    float imagePeak = 0;
};

std::atomic<HWND> consoleTarget;
HANDLE finished;

BOOL WINAPI onConsoleEvent(DWORD) {
    PostMessageW(consoleTarget, WM_CLOSE, 0, 0);
    // Windows kills the process when this returns on console close, so wait for the restore.
    WaitForSingleObject(finished, 4000);
    return TRUE;
}

void setTitle(HWND hwnd, const Window& w) {
    if (w.eightBit) {
        SetWindowTextW(hwnd, L"hdr10pg | 8-bit SDR | F11, Esc");
        return;
    }
    wchar_t title[192];
    swprintf(title, std::size(title),
             L"hdr10pg | HDR10+ GAMING %ls | test peak %u nits | Up/Down, PgUp/PgDn, F11, Esc",
             w.signal.on ? L"ON" : L"OFF", w.peakNits);
    SetWindowTextW(hwnd, title);
}

int run(HINSTANCE instance, int showCommand, const Display* display, std::optional<FullscreenImage> image) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"hdr10pg";
    if (!RegisterClassW(&windowClass)) throw std::runtime_error("RegisterClassW failed");

    Window w;
    w.eightBit = image && image->eightBit;
    w.signal.display = w.eightBit ? nullptr : display;
    if (display && display->maxLuminance) w.peakNits = std::clamp(display->maxLuminance, 100u, 10000u);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, cx = 1280, cy = 720;
    MONITORINFO monitor = {sizeof(monitor)};
    if (display && display->monitor && GetMonitorInfoW(display->monitor, &monitor)) {
        const RECT& r = monitor.rcWork;
        cx = std::min<int>(cx, r.right - r.left);
        cy = std::min<int>(cy, r.bottom - r.top);
        x = r.left + (r.right - r.left - cx) / 2;
        y = r.top + (r.bottom - r.top - cy) / 2;
    }
    const HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"hdr10pg", WS_OVERLAPPEDWINDOW, x, y, cx, cy,
                                      nullptr, nullptr, windowClass.hInstance, &w);
    if (!hwnd) throw std::runtime_error("CreateWindowExW failed");

    RECT client = {};
    GetClientRect(hwnd, &client);
    Renderer renderer(hwnd, client.right, client.bottom, w.eightBit);

    consoleTarget = hwnd;
    finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!finished || !SetConsoleCtrlHandler(onConsoleEvent, TRUE))
        throw std::runtime_error("Cannot install the Ctrl+C handler");

    ShowWindow(hwnd, showCommand);
    if (image) toggleFullscreen(hwnd, w);
    w.signal.enable();
    printf(w.signal.on ? "HDR10+ GAMING enabled. Close the window to restore.\n"
                       : "Showing the test screen without the HDR10+ GAMING signal.\n");
    fflush(stdout);

    // The message pump: handle everything queued, then present a frame (or sleep while minimised).
    MSG message = {};
    while (message.message != WM_QUIT) {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        if (IsIconic(hwnd) || !w.width || !w.height) {
            WaitMessage();
            continue;
        }
        if (w.resized) {
            renderer.resize(w.width, w.height);
            w.resized = false;
        }
        if (w.titleStale) {
            setTitle(hwnd, w);
            w.titleStale = false;
        }
        const float peak = static_cast<float>(w.peakNits);
        if (!(image ? renderer.drawGradient(image->pattern, peak) : renderer.draw(w.width, w.height, peak)))
            Sleep(50);
    }
    return static_cast<int>(message.wParam); // the PostQuitMessage code
}

} // namespace

int runTestWindow(HINSTANCE instance, int showCommand, const Display* display,
                  std::optional<FullscreenImage> image) {
    // The signal is restored when run() unwinds; only then may a waiting console handler return.
    struct Done {
        ~Done() {
            if (finished) SetEvent(finished);
        }
    } done;
    return run(instance, showCommand, display, image);
}
