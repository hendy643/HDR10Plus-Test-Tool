// Detects and enables HDR10+ GAMING through NVIDIA NVAPI and the Intel Graphics Control Library.
#include "displays.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>

#include <nvapi.h>
#include <igcl_api.h>

using Microsoft::WRL::ComPtr;

// ---- NVIDIA ----------------------------------------------------------------

static const char* nvModeName(NV_DISPLAY_OUTPUT_MODE mode) {
    switch (mode) {
    case NV_DISPLAY_OUTPUT_MODE_SDR: return "SDR";
    case NV_DISPLAY_OUTPUT_MODE_HDR10: return "HDR10";
    case NV_DISPLAY_OUTPUT_MODE_HDR10PLUS_GAMING: return "HDR10+ GAMING";
    default: return "unknown";
    }
}

static std::string nvError(NvAPI_Status status) {
    NvAPI_ShortString text = {};
    NvAPI_GetErrorMessage(status, text);
    return text;
}

// How Windows says the monitor behind a GDI device name is connected; OTHER if it cannot tell.
static DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY connectionOf(const wchar_t* gdiName) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), paths[i].sourceInfo.adapterId,
                         paths[i].sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
            wcscmp(source.viewGdiDeviceName, gdiName) == 0)
            return paths[i].targetInfo.outputTechnology;
    }
    return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
}

static void addNvidiaDisplays(std::vector<Display>& displays) {
    if (NvAPI_Status s = NvAPI_Initialize(); s != NVAPI_OK) {
        printf("NVIDIA: NVAPI unavailable (%s)\n", nvError(s).c_str());
        return;
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        printf("NVIDIA: cannot create a DXGI factory\n");
        return;
    }

    // Only active desktop outputs can be switched, so enumerate those and ask NVAPI which are its own.
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc = {};
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc))) continue;

            char gdiName[sizeof(desc.DeviceName)] = {};
            WideCharToMultiByte(CP_ACP, 0, desc.DeviceName, -1, gdiName, sizeof(gdiName) - 1, nullptr, nullptr);
            NvU32 id = 0;
            if (NvAPI_DISP_GetDisplayIdByDisplayName(gdiName, &id) != NVAPI_OK) continue; // not an NVIDIA output

            Display d;
            d.description = std::string("NVIDIA ") + gdiName;
            d.monitor = desc.Monitor;

            NV_HDR_CAPABILITIES caps = {};
            caps.version = NV_HDR_CAPABILITIES_VER;
            if (NvAPI_Status s = NvAPI_Disp_GetHdrCapabilities(id, &caps); s != NVAPI_OK) {
                d.description += "\n  HDR capability query failed: " + nvError(s) + " (driver R525 or newer required)";
                displays.push_back(std::move(d));
                continue;
            }

            const bool windowsHdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            // The driver can report support, and accept the mode, over DisplayPort, where it does not work.
            const auto connection = connectionOf(desc.DeviceName);
            const bool hdmi = connection == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
            d.supported = caps.isHdr10PlusGamingSupported && hdmi;
            d.maxLuminance = caps.display_data.desired_content_max_luminance;
            // HDR10 and HDR10+ are what the display advertises; ST 2084 (PQ) is the HDR10 transfer function.
            d.description += std::string("\n  HDR10 supported: ") + (caps.isST2084EotfSupported ? "yes" : "no");
            d.description += std::string("\n  HDR10+ supported: ") + (caps.isHdr10PlusSupported ? "yes" : "no");
            d.description += std::string("\n  HDR10+ GAMING supported: ") + (d.supported                       ? "yes"
                              : !caps.isHdr10PlusGamingSupported ? "no (needs a certified display with Game Mode on)"
                                                                 : "no (needs an HDMI connection)");
            d.description += std::string("\n  Connection: ") +
                             (hdmi ? "HDMI"
                              : connection == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL ? "DisplayPort"
                                                                                                   : "other");
            d.description += std::string("\n  Windows HDR: ") + (windowsHdr ? "on" : "off");

            NV_DISPLAY_OUTPUT_MODE mode = NV_DISPLAY_OUTPUT_MODE_SDR;
            if (NvAPI_Disp_GetOutputMode(id, &mode) == NVAPI_OK)
                d.description += std::string("\n  Output mode: ") + nvModeName(mode);

            // What the GPU sends down the cable; it says nothing about the panel itself.
            NV_COLOR_DATA color = {};
            color.version = NV_COLOR_DATA_VER;
            color.size = sizeof(color);
            color.cmd = NV_COLOR_CMD_GET;
            if (NvAPI_Disp_ColorControl(id, &color) == NVAPI_OK) {
                const char* depth = color.data.bpc == NV_BPC_6    ? "6 bpc"
                                    : color.data.bpc == NV_BPC_8  ? "8 bpc"
                                    : color.data.bpc == NV_BPC_10 ? "10 bpc"
                                    : color.data.bpc == NV_BPC_12 ? "12 bpc"
                                    : color.data.bpc == NV_BPC_16 ? "16 bpc"
                                                                  : "default";
                d.description += std::string("\n  GPU output depth: ") + depth;
            }

            if (caps.isHdr10PlusGamingSupported) {
                const auto& v = caps.hdr10plus_vsvdb;
                d.description +=
                    "\n  VSVDB application version: " + std::to_string(v.application_version) +
                    "\n  VSVDB peak luminance index: " + std::to_string(v.peak_luminance_index) +
                    "\n  VSVDB full-frame peak luminance index: " + std::to_string(v.full_frame_peak_luminance_index) +
                    "\n  Standard HDR max luminance: " + std::to_string(caps.display_data.desired_content_max_luminance) +
                    " nits (not the HDR10+ GAMING tone mapping target)";
            }

            d.enable = [id, windowsHdr] {
                if (!windowsHdr) {
                    printf("Turn on \"Use HDR\" in Windows display settings first.\n");
                    return false;
                }
                NV_DISPLAY_OUTPUT_MODE request = NV_DISPLAY_OUTPUT_MODE_HDR10PLUS_GAMING;
                if (NvAPI_Status s = NvAPI_Disp_SetOutputMode(id, &request); s != NVAPI_OK) {
                    printf("NvAPI_Disp_SetOutputMode failed: %s\n", nvError(s).c_str());
                    return false;
                }
                NV_DISPLAY_OUTPUT_MODE now = NV_DISPLAY_OUTPUT_MODE_SDR;
                if (NvAPI_Disp_GetOutputMode(id, &now) != NVAPI_OK || now != NV_DISPLAY_OUTPUT_MODE_HDR10PLUS_GAMING) {
                    printf("The driver accepted the request but the output mode is %s.\n", nvModeName(now));
                    return false;
                }
                return true;
            };
            // The override belongs to this process; setting the earlier mode (or exiting) ends it.
            d.restore = [id, mode] {
                NV_DISPLAY_OUTPUT_MODE request = mode;
                if (NvAPI_Status s = NvAPI_Disp_SetOutputMode(id, &request); s != NVAPI_OK)
                    printf("Restore failed: %s (exiting also releases the override)\n", nvError(s).c_str());
            };
            displays.push_back(std::move(d));
        }
    }
}

// ---- Intel -----------------------------------------------------------------

static bool igclSettings(ctl_display_output_handle_t output, ctl_display_settings_t& s, bool set) {
    s.Size = sizeof(s);
    s.Version = 0;
    s.Set = set;
    s.ValidFlags = CTL_DISPLAY_SETTING_FLAG_LOW_LATENCY | CTL_DISPLAY_SETTING_FLAG_SOURCE_TM;
    const ctl_result_t r = ctlGetSetDisplaySettings(output, &s);
    if (r != CTL_RESULT_SUCCESS && set)
        printf("ctlGetSetDisplaySettings failed: 0x%X\n", static_cast<unsigned>(r));
    return r == CTL_RESULT_SUCCESS;
}

static void addIntelDisplays(std::vector<Display>& displays) {
    ctl_init_args_t init = {};
    init.Size = sizeof(init);
    init.AppVersion = CTL_MAKE_VERSION(CTL_IMPL_MAJOR_VERSION, CTL_IMPL_MINOR_VERSION);
    ctl_api_handle_t api = nullptr; // kept open until process exit: the display handles depend on it
    if (const ctl_result_t r = ctlInit(&init, &api); r != CTL_RESULT_SUCCESS) {
        printf("Intel: control library unavailable (0x%X)\n", static_cast<unsigned>(r));
        return;
    }

    uint32_t adapterCount = 0;
    ctlEnumerateDevices(api, &adapterCount, nullptr);
    std::vector<ctl_device_adapter_handle_t> adapters(adapterCount);
    if (!adapterCount || ctlEnumerateDevices(api, &adapterCount, adapters.data()) != CTL_RESULT_SUCCESS) return;

    for (uint32_t a = 0; a < adapterCount; ++a) {
        uint32_t count = 0;
        ctlEnumerateDisplayOutputs(adapters[a], &count, nullptr);
        std::vector<ctl_display_output_handle_t> outputs(count);
        if (!count || ctlEnumerateDisplayOutputs(adapters[a], &count, outputs.data()) != CTL_RESULT_SUCCESS) continue;

        for (uint32_t o = 0; o < count; ++o) {
            const ctl_display_output_handle_t output = outputs[o];
            ctl_display_properties_t props = {};
            props.Size = sizeof(props);
            if (ctlGetDisplayProperties(output, &props) != CTL_RESULT_SUCCESS ||
                !(props.DisplayConfigFlags & CTL_DISPLAY_CONFIG_FLAG_DISPLAY_ATTACHED))
                continue;

            ctl_display_settings_t before = {};
            const bool queried = igclSettings(output, before, false);
            const auto needed = static_cast<ctl_display_setting_flags_t>(
                CTL_DISPLAY_SETTING_FLAG_LOW_LATENCY | CTL_DISPLAY_SETTING_FLAG_SOURCE_TM);
            const bool certified = props.FeatureSupportedFlags & CTL_STD_DISPLAY_FEATURE_FLAG_HDR10_PLUS_CERTIFIED;
            const bool hdmi = props.Type == CTL_DISPLAY_OUTPUT_TYPES_HDMI;
            const bool controllable = queried && (before.SupportedFlags & needed) == needed &&
                                      (before.ControllableFlags & needed) == needed;

            Display d;
            d.supported = certified && hdmi && controllable;
            d.description = "Intel adapter " + std::to_string(a) + " output " + std::to_string(o) +
                "\n  HDR10 supported: " +
                (props.FeatureSupportedFlags & CTL_STD_DISPLAY_FEATURE_FLAG_HDR ? "yes" : "no") +
                "\n  HDR10+ supported: " + (certified ? "yes (certified)" : "no certification reported") +
                "\n  HDR10+ GAMING supported: " + (d.supported ? "yes" : "no") +
                "\n  HDMI: " + (hdmi ? "yes" : "no") +
                "\n  Low latency + source tone mapping controllable: " + (controllable ? "yes" : "no (is Windows HDR on?)");
            if (controllable) {
                const bool on = before.SourceTM == CTL_DISPLAY_SETTING_SOURCETM_ENABLED &&
                                before.LowLatency == CTL_DISPLAY_SETTING_LOW_LATENCY_ENABLED;
                d.description += std::string("\n  HDR10+ GAMING signal: ") + (on ? "enabled" : "not enabled");
            }

            d.enable = [output] {
                ctl_display_settings_t s = {};
                s.LowLatency = CTL_DISPLAY_SETTING_LOW_LATENCY_ENABLED;
                s.SourceTM = CTL_DISPLAY_SETTING_SOURCETM_ENABLED;
                return igclSettings(output, s, true);
            };
            // shortcut: Intel settings outlive the process, so a killed process leaves them on; run `disable`.
            d.restore = [output, before] {
                ctl_display_settings_t s = before;
                igclSettings(output, s, true);
            };
            d.reset = [output] {
                ctl_display_settings_t s = {};
                s.LowLatency = CTL_DISPLAY_SETTING_LOW_LATENCY_DEFAULT;
                s.SourceTM = CTL_DISPLAY_SETTING_SOURCETM_DEFAULT;
                return igclSettings(output, s, true);
            };
            displays.push_back(std::move(d));
        }
    }
}

std::vector<Display> enumerateDisplays() {
    std::vector<Display> displays;
    addNvidiaDisplays(displays);
    addIntelDisplays(displays);
    return displays;
}
