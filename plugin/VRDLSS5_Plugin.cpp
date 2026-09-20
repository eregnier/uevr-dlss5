#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <memory>
#include <mutex>
#include <string>

#include "imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

#include "rendering/d3d11.hpp"
#include "rendering/d3d12.hpp"

#include "uevr/Plugin.hpp"
#include "VRDLSS5_Plugin.hpp"

using namespace uevr;

// ----------------------------------------------------------------------------
// Dedicated On-Disk Logger for Troubleshooting
// ----------------------------------------------------------------------------
static void PluginLog(const char* fmt, ...) {
    static char logPath[MAX_PATH] = "";
    if (logPath[0] == '\0') {
        char exePath[MAX_PATH] = "";
        if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
            char* lastSlash = strrchr(exePath, '\\');
            if (lastSlash) {
                *(lastSlash + 1) = '\0';
                strcpy_s(logPath, MAX_PATH, exePath);
                strcat_s(logPath, MAX_PATH, "vrdlss5_uevr_plugin.log");
            }
        }
        if (logPath[0] == '\0') {
            strcpy_s(logPath, MAX_PATH, "vrdlss5_uevr_plugin.log");
        }
    }

    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) {
        fprintf(f, "[%02d:%02d:%02d.%03d][PID:%lu][TID:%lu] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                GetCurrentProcessId(), GetCurrentThreadId(), buf);
        fflush(f);
        fclose(f);
    }

    OutputDebugStringA("[VRDLSS5] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    try {
        if (API::get().get() != nullptr) {
            API::get()->log_info("[VRDLSS5] %s", buf);
        }
    } catch (...) {}
}

static bool s_pluginInitialized = false;

// ----------------------------------------------------------------------------
// VR-DLSS5 Plugin for UEVR (Unreal Engine VR)
// ----------------------------------------------------------------------------
class VRDLSS5Plugin : public uevr::Plugin {
public:
    VRDLSS5Plugin() = default;

    void on_dllmain() override {
        PluginLog("[DLL] on_dllmain called (Process attach).");
    }

    void on_initialize() override {
        if (s_pluginInitialized) {
            PluginLog("[INIT] VR-DLSS5 Plugin already initialized, skipping duplicate call.");
            return;
        }
        s_pluginInitialized = true;

        PluginLog("================================================================================");
        PluginLog("[INIT] VR-DLSS5 Plugin initializing for UEVR...");
        PluginLog("[INIT] Target Process: PID %lu", GetCurrentProcessId());

        char exePath[MAX_PATH] = "";
        if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
            PluginLog("[INIT] Process Image: %s", exePath);
        }

        // 1. Actively detect OptiScaler Pre-SR Engine (can be dxgi.dll, version.dll, OptiScaler.dll, or OptiScaler.asi)
        HMODULE hOpti = nullptr;
        const char* candidateModules[] = { "dxgi.dll", "version.dll", "OptiScaler.dll", "OptiScaler.asi" };
        for (const char* modName : candidateModules) {
            HMODULE h = GetModuleHandleA(modName);
            if (h && GetProcAddress(h, "GetBehaviorValue") != nullptr) {
                hOpti = h;
                PluginLog("[INIT] Detected OptiScaler running as %s (Module=%p)", modName, h);
                break;
            }
        }

        if (!hOpti) {
            hOpti = GetModuleHandleA("OptiScaler.dll");
            if (!hOpti) hOpti = GetModuleHandleA("OptiScaler.asi");
            if (!hOpti) {
                PluginLog("[INIT] Attempting to load OptiScaler.dll...");
                hOpti = LoadLibraryA("OptiScaler.dll");
                if (!hOpti) {
                    PluginLog("[INIT] OptiScaler.dll not found directly, trying OptiScaler.asi...");
                    hOpti = LoadLibraryA("OptiScaler.asi");
                }
            }
        }

        if (hOpti) {
            char optiPath[MAX_PATH] = "";
            GetModuleFileNameA(hOpti, optiPath, MAX_PATH);
            PluginLog("[INIT] OptiScaler Pre-SR Engine ACTIVE at: %s (Module=%p)", optiPath, hOpti);
            m_optiScalerLoaded = true;
        } else {
            DWORD err = GetLastError();
            PluginLog("[INIT] WARNING: OptiScaler could not be found or loaded! (GetLastError=%lu / 0x%08X)", err, err);
            m_optiScalerLoaded = false;
        }

        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        // Tune ImGui style for VR readability (high contrast neon cyan/amber)
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.WindowBorderSize = 1.5f;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.08f, 0.12f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.0f, 0.75f, 1.0f, 0.85f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.25f, 0.45f, 0.85f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.35f, 0.60f, 1.0f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.45f, 0.75f, 1.0f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.10f, 0.20f, 0.35f, 0.9f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.32f, 0.55f, 1.0f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.44f, 0.75f, 1.0f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.0f, 0.85f, 1.0f, 1.0f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.2f, 1.0f, 0.8f, 1.0f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.95f, 0.98f, 1.0f);

        InitPaths();
        InitVariablesFromIni();
        bool ctlOk = VrCtlEnsure();
        VrCtlPublish();

        PluginLog("[INIT] Shared memory control channel: %s", ctlOk ? "OK (Created/Mapped)" : "FAILED");
        PluginLog("[INIT] Configuration loaded: Enabled=%d, WorkingScale=%.2f, RunBeforeSR=%d, Preset=%d, ResidualAcrossRR=%d",
                  g_masterEnable ? 1 : 0, g_workingScale, g_runBeforeSR ? 1 : 0, g_nrPreset, g_residualAcrossRR ? 1 : 0);
        PluginLog("[INIT] Controls ready: UEVR native menu (L3+R3 / Insert), Keyboard F6 / Home");
        PluginLog("[INIT] Initialization complete.");
        PluginLog("================================================================================");
    }

    void on_device_reset() override {
        PluginLog("[DEVICE] on_device_reset called. Resetting ImGui...");
        std::scoped_lock _{m_imgui_mutex};
        const auto renderer_data = API::get()->param()->renderer;

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_Shutdown();
            g_d3d11 = {};
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            g_d3d12.reset();
            ImGui_ImplDX12_Shutdown();
            g_d3d12 = {};
        }

        m_initialized = false;
        PluginLog("[DEVICE] Reset finished. m_initialized = false.");
    }

    void on_present() override {
        std::scoped_lock _{m_imgui_mutex};

        static bool s_firstPresentLogged = false;
        if (!s_firstPresentLogged) {
            s_firstPresentLogged = true;
            const auto renderer_data = API::get()->param()->renderer;
            const auto vr_active = API::get()->param()->vr->is_hmd_active();
            PluginLog("[PRESENT] First on_present callback received! RendererType=%d, Swapchain=%p, HMDActive=%s",
                      renderer_data ? renderer_data->renderer_type : -1,
                      renderer_data ? renderer_data->swapchain : nullptr,
                      vr_active ? "YES" : "NO");
        }

        if (!m_initialized) {
            if (!initialize_imgui()) {
                return;
            }
        }

        // Periodic check for 500ms debounce commit
        CheckDebounceCommit();

        // Telemetry update
        UpdateTelemetry();

        // Periodic heartbeat every 5 seconds when HUD is open
        static uint64_t s_lastHeartbeat = 0;
        uint64_t now = GetTickCount64();
        if (g_hudVisible && (now - s_lastHeartbeat >= 5000)) {
            s_lastHeartbeat = now;
            PluginLog("[HEARTBEAT] HUD is OPEN. OptiReady=%d, GpuAvg=%.2f ms, HMDActive=%s",
                      g_optiReady ? 1 : 0, g_gpuMsAvg, API::get()->param()->vr->is_hmd_active() ? "YES" : "NO");
        }

        // When VR is NOT active, render to desktop swapchain
        const auto vr_active = API::get()->param()->vr->is_hmd_active();
        if (!vr_active && g_hudVisible) {
            if (!m_was_rendering_desktop) {
                m_was_rendering_desktop = true;
                on_device_reset();
                return;
            }
            m_was_rendering_desktop = true;

            const auto renderer_data = API::get()->param()->renderer;
            if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                DrawHUD();

                ImGui::EndFrame();
                ImGui::Render();
                g_d3d11.render_imgui();
            } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
                auto command_queue = (ID3D12CommandQueue*)renderer_data->command_queue;
                if (command_queue != nullptr) {
                    ImGui_ImplDX12_NewFrame();
                    ImGui_ImplWin32_NewFrame();
                    ImGui::NewFrame();

                    DrawHUD();

                    ImGui::EndFrame();
                    ImGui::Render();
                    g_d3d12.render_imgui();
                }
            }
        }
    }

    void on_post_render_vr_framework_dx11(ID3D11DeviceContext* context, ID3D11Texture2D* texture, ID3D11RenderTargetView* rtv) override {
        const auto vr_active = API::get()->param()->vr->is_hmd_active();

        static bool s_firstVRRenderLogged = false;
        if (!s_firstVRRenderLogged) {
            s_firstVRRenderLogged = true;
            PluginLog("[VR-RENDER] First on_post_render_vr_framework_dx11 called! Context=%p, Texture=%p, RTV=%p, HMDActive=%s",
                      context, texture, rtv, vr_active ? "YES" : "NO");
        }

        if (!m_initialized) {
            if (!initialize_imgui()) return;
        }

        if (!vr_active || !g_hudVisible) return;

        if (m_was_rendering_desktop) {
            m_was_rendering_desktop = false;
            on_device_reset();
            return;
        }

        std::scoped_lock _{m_imgui_mutex};

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawHUD();

        ImGui::EndFrame();
        ImGui::Render();
        g_d3d11.render_imgui_vr(context, rtv);
    }

    void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList* command_list, ID3D12Resource* rt, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {
        const auto vr_active = API::get()->param()->vr->is_hmd_active();

        static bool s_firstVRRenderLogged = false;
        if (!s_firstVRRenderLogged) {
            s_firstVRRenderLogged = true;
            PluginLog("[VR-RENDER] First on_post_render_vr_framework_dx12 called! CommandList=%p, RT=%p, RTV=%p, HMDActive=%s",
                      command_list, rt, rtv, vr_active ? "YES" : "NO");
        }

        if (!m_initialized) {
            if (!initialize_imgui()) return;
        }

        if (!vr_active || !g_hudVisible) return;

        if (m_was_rendering_desktop) {
            m_was_rendering_desktop = false;
            on_device_reset();
            return;
        }

        std::scoped_lock _{m_imgui_mutex};

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawHUD();

        ImGui::EndFrame();
        ImGui::Render();
        g_d3d12.render_imgui_vr(command_list, rtv);
    }

    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override {
        // Toggle HUD on F6 / Home (VK_INSERT reserved for UEVR menu coordination)
        if (msg == WM_KEYDOWN) {
            if (wparam == VK_F6) {
                ToggleHUD("Keyboard F6");
                return false;
            } else if (wparam == VK_HOME) {
                ToggleHUD("Keyboard HOME");
                return false;
            } else if (wparam == VK_F8) {
                g_residualAcrossRR = !g_residualAcrossRR;
                g_hasPendingSave = true;
                g_lastChangeTick = GetTickCount64();
                VrCtlPublish();
                PluginLog("[INPUT] ResidualAcrossRR toggled via F8: %s", g_residualAcrossRR ? "ON" : "OFF");
                return false;
            } else if (wparam == VK_ESCAPE && g_hudVisible) {
                ToggleHUD("Keyboard ESCAPE");
                return false;
            }
        }

        if (g_hudVisible) {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        }

        return true;
    }

    void on_custom_event(const char* event_name, const char* event_data) override {
        if (!event_name || !event_data) return;

        if (strcmp(event_name, "VRDLSS5_SYNC") == 0) {
            float ws = 0.75f, inten = 1.0f, ls = 1.0f, lt = 0.0f, ss = -1.0f, ts = 1.0f, cs = 1.0f;
            int en = 1, rbsr = 1, pr = 2, sty = 0, am = 1, pa = 1;

            if (sscanf_s(event_data, "%f;%d;%d;%d;%f;%d;%f;%f;%d;%f;%d;%f;%f",
                         &ws, &en, &rbsr, &pr, &inten, &sty, &ls, &lt, &am, &ss, &pa, &ts, &cs) >= 4) {
                g_workingScale = ws;
                g_masterEnable = (en != 0);
                g_runBeforeSR = (rbsr != 0);
                g_nrPreset = pr;
                g_nrIntensity = inten;
                g_nrStyle = sty;
                g_nrLocalStructure = ls;
                g_nrLocalTone = lt;
                g_nrAutoMask = (am != 0);
                g_nrSkinStructure = ss;
                g_nrPasses = pa;
                g_nrTransferStrength = ts;
                g_nrColourStrength = cs;

                VrCtlPublish();
                PluginLog("[LUA-SYNC] Real-time parameters updated: WorkingScale=%.2f, Preset=%d, RunBeforeSR=%d", ws, pr, rbsr);
            }
        }
    }

private:
    bool initialize_imgui() {
        const auto renderer_data = API::get()->param()->renderer;
        if (!renderer_data || !renderer_data->swapchain) {
            PluginLog("[IMGUI] Cannot initialize ImGui: renderer_data or swapchain is null!");
            return false;
        }

        auto swapchain = (IDXGISwapChain*)renderer_data->swapchain;
        DXGI_SWAP_CHAIN_DESC swap_desc{};
        swapchain->GetDesc(&swap_desc);
        m_wnd = swap_desc.OutputWindow;
        if (!m_wnd) m_wnd = GetActiveWindow();

        PluginLog("[IMGUI] Initializing ImGui Win32 backend with HWND: %p...", m_wnd);
        if (m_wnd && !ImGui_ImplWin32_Init(m_wnd)) {
            PluginLog("[IMGUI] Failed to initialize ImGui Win32 backend!");
            return false;
        }

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            PluginLog("[IMGUI] Initializing D3D11 renderlib backend...");
            if (!g_d3d11.initialize()) {
                PluginLog("[IMGUI] Failed to initialize D3D11 renderlib backend!");
                return false;
            }
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            PluginLog("[IMGUI] Initializing D3D12 renderlib backend...");
            if (!g_d3d12.initialize()) {
                PluginLog("[IMGUI] Failed to initialize D3D12 renderlib backend!");
                return false;
            }
        }

        m_initialized = true;
        PluginLog("[IMGUI] ImGui initialized successfully! Ready to render HUD.");
        return true;
    }

    void ToggleHUD(const char* source) {
        g_hudVisible = !g_hudVisible;
        PluginLog("[HUD] State changed -> %s (Source: %s)", g_hudVisible ? "OPEN" : "CLOSED", source);

        try {
            const auto functions = API::get()->param()->functions;
            if (functions && m_wnd) {
                bool isDrawingUI = functions->is_drawing_ui();
                PluginLog("[HUD] Current UEVR is_drawing_ui = %s", isDrawingUI ? "YES" : "NO");

                // In VR, UEVR only displays the VR UI overlay into the headset if is_drawing_ui is true.
                // If our HUD is opening and UEVR UI quad is closed, post VK_INSERT so UEVR opens the VR UI layer.
                if (g_hudVisible && !isDrawingUI) {
                    PluginLog("[HUD] Posting VK_INSERT to activate UEVR VR UI layer in headset...");
                    PostMessageA(m_wnd, WM_KEYDOWN, VK_INSERT, 0);
                    PostMessageA(m_wnd, WM_KEYUP, VK_INSERT, 0);
                }
                // If our HUD is closing and UEVR UI was opened, post VK_INSERT to close it
                else if (!g_hudVisible && isDrawingUI) {
                    PluginLog("[HUD] Posting VK_INSERT to close UEVR VR UI layer in headset...");
                    PostMessageA(m_wnd, WM_KEYDOWN, VK_INSERT, 0);
                    PostMessageA(m_wnd, WM_KEYUP, VK_INSERT, 0);
                }
            }
        } catch (...) {
            PluginLog("[HUD] Exception in ToggleHUD while coordinating with UEVR.");
        }
    }

    void InitPaths() {
        char exePath[MAX_PATH];
        if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
            char* lastSlash = strrchr(exePath, '\\');
            if (lastSlash) {
                *(lastSlash + 1) = '\0';
                strcpy_s(g_iniPath, MAX_PATH, exePath);
                strcat_s(g_iniPath, MAX_PATH, "OptiScaler.ini");
                PluginLog("[PATHS] IniPath resolved: %s", g_iniPath);
            }
        }
    }

    void InitVariablesFromIni() {
        if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
            PluginLog("[INI] OptiScaler.ini not found at %s. Using default settings.", g_iniPath);
            return;
        }

        char enabledStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "Enabled", "false", enabledStr, sizeof(enabledStr), g_iniPath);
        g_masterEnable = (_stricmp(enabledStr, "true") == 0 || _stricmp(enabledStr, "1") == 0);

        char scaleStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "WorkingScale", "0.75", scaleStr, sizeof(scaleStr), g_iniPath);
        g_workingScale = (float)atof(scaleStr);
        if (g_workingScale < 0.25f || g_workingScale > 1.50f) g_workingScale = 0.75f;

        char preSrStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "RunBeforeSR", "true", preSrStr, sizeof(preSrStr), g_iniPath);
        g_runBeforeSR = (_stricmp(preSrStr, "true") == 0 || _stricmp(preSrStr, "1") == 0);

        g_nrPreset = GetPrivateProfileIntA("DlssNr", "Preset", 2, g_iniPath);
        if (g_nrPreset < 0 || g_nrPreset > 2) g_nrPreset = 2;

        char rrStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "ResidualAcrossRR", "false", rrStr, sizeof(rrStr), g_iniPath);
        g_residualAcrossRR = (_stricmp(rrStr, "true") == 0 || _stricmp(rrStr, "1") == 0);

        char intStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "Intensity", "1.00", intStr, sizeof(intStr), g_iniPath);
        g_nrIntensity = (float)atof(intStr);

        g_nrStyle = GetPrivateProfileIntA("DlssNr", "Style", 0, g_iniPath);

        char structStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "LocalStructure", "1.00", structStr, sizeof(structStr), g_iniPath);
        g_nrLocalStructure = (float)atof(structStr);

        char toneStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "LocalTone", "0.00", toneStr, sizeof(toneStr), g_iniPath);
        g_nrLocalTone = (float)atof(toneStr);

        char autoMaskStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "AutoMask", "true", autoMaskStr, sizeof(autoMaskStr), g_iniPath);
        g_nrAutoMask = (_stricmp(autoMaskStr, "true") == 0 || _stricmp(autoMaskStr, "1") == 0);

        char skinStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "SkinStructure", "-1.00", skinStr, sizeof(skinStr), g_iniPath);
        g_nrSkinStructure = (float)atof(skinStr);

        char transferStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "TransferStrength", "1.00", transferStr, sizeof(transferStr), g_iniPath);
        g_nrTransferStrength = (float)atof(transferStr);

        char colourStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "ColourStrength", "1.00", colourStr, sizeof(colourStr), g_iniPath);
        g_nrColourStrength = (float)atof(colourStr);

        g_nrPasses = GetPrivateProfileIntA("DlssNr", "Passes", 1, g_iniPath);
        if (g_nrPasses < 1 || g_nrPasses > 3) g_nrPasses = 1;

        g_frameGuardActive = GetPrivateProfileIntA("VRHUD", "FrameGuard", 1, g_iniPath) != 0;
    }

    bool VrCtlEnsure() {
        if (g_vrCtl) return true;
        if (!g_vrCtlMap) {
            char mappingName[96];
            sprintf_s(mappingName, sizeof(mappingName), "%s_%lu", VRDLSS5_CTL_MAPPING_NAME, GetCurrentProcessId());
            g_vrCtlMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                            sizeof(VrDlss5ControlBlock), mappingName);
        }
        if (!g_vrCtlMap) {
            PluginLog("[VRCTL] CreateFileMappingA failed! Error: %lu", GetLastError());
            return false;
        }

        g_vrCtl = (VrDlss5ControlBlock*)MapViewOfFile(g_vrCtlMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VrDlss5ControlBlock));
        if (!g_vrCtl) {
            PluginLog("[VRCTL] MapViewOfFile failed! Error: %lu", GetLastError());
            return false;
        }

        if (g_vrCtl->magic != VRDLSS5_CTL_MAGIC || g_vrCtl->version != VRDLSS5_CTL_VERSION) {
            memset(g_vrCtl, 0, sizeof(VrDlss5ControlBlock));
            g_vrCtl->magic = VRDLSS5_CTL_MAGIC;
            g_vrCtl->version = VRDLSS5_CTL_VERSION;
        }
        return true;
    }

    void VrCtlPublish() {
        if (!VrCtlEnsure()) return;

        g_vrCtl->enabled = g_masterEnable ? 1 : 0;
        g_vrCtl->workingScale = g_workingScale;
        g_vrCtl->runBeforeSR = g_runBeforeSR ? 1 : 0;
        g_vrCtl->residualAcrossRR = g_residualAcrossRR ? 1 : 0;
        g_vrCtl->preset = g_nrPreset;
        g_vrCtl->intensity = g_nrIntensity;
        g_vrCtl->style = g_nrStyle;
        g_vrCtl->localStructure = g_nrLocalStructure;
        g_vrCtl->localTone = g_nrLocalTone;
        g_vrCtl->autoMask = g_nrAutoMask ? 1 : 0;
        g_vrCtl->skinStructure = g_nrSkinStructure;
        g_vrCtl->transferStrength = g_nrTransferStrength;
        g_vrCtl->colourStrength = g_nrColourStrength;
        g_vrCtl->passes = g_nrPasses;
        InterlockedIncrement(&g_vrCtl->seq);
    }

    void CommitSettingsToDisk() {
        if (g_iniPath[0] == '\0') return;

        WritePrivateProfileStringA("DlssNr", "Enabled", g_masterEnable ? "true" : "false", g_iniPath);

        char buf[32];
        sprintf_s(buf, sizeof(buf), "%.2f", g_workingScale);
        WritePrivateProfileStringA("DlssNr", "WorkingScale", buf, g_iniPath);

        WritePrivateProfileStringA("DlssNr", "RunBeforeSR", g_runBeforeSR ? "true" : "false", g_iniPath);

        sprintf_s(buf, sizeof(buf), "%d", g_nrPreset);
        WritePrivateProfileStringA("DlssNr", "Preset", buf, g_iniPath);

        WritePrivateProfileStringA("DlssNr", "ResidualAcrossRR", g_residualAcrossRR ? "true" : "false", g_iniPath);

        sprintf_s(buf, sizeof(buf), "%.2f", g_nrIntensity);
        WritePrivateProfileStringA("DlssNr", "Intensity", buf, g_iniPath);

        sprintf_s(buf, sizeof(buf), "%d", g_nrStyle);
        WritePrivateProfileStringA("DlssNr", "Style", buf, g_iniPath);

        sprintf_s(buf, sizeof(buf), "%.2f", g_nrLocalStructure);
        WritePrivateProfileStringA("DlssNr", "LocalStructure", buf, g_iniPath);

        sprintf_s(buf, sizeof(buf), "%.2f", g_nrLocalTone);
        WritePrivateProfileStringA("DlssNr", "LocalTone", buf, g_iniPath);

        WritePrivateProfileStringA("DlssNr", "AutoMask", g_nrAutoMask ? "true" : "false", g_iniPath);

        sprintf_s(buf, sizeof(buf), "%.2f", g_nrSkinStructure);
        WritePrivateProfileStringA("DlssNr", "SkinStructure", buf, g_iniPath);

        sprintf_s(buf, sizeof(buf), "%.2f", g_nrTransferStrength);
        WritePrivateProfileStringA("DlssNr", "TransferStrength", buf, g_iniPath);

        sprintf_s(buf, sizeof(buf), "%.2f", g_nrColourStrength);
        WritePrivateProfileStringA("DlssNr", "ColourStrength", buf, g_iniPath);

        sprintf_s(buf, sizeof(buf), "%d", g_nrPasses);
        WritePrivateProfileStringA("DlssNr", "Passes", buf, g_iniPath);

        sprintf_s(buf, sizeof(buf), "%d", g_frameGuardActive ? 1 : 0);
        WritePrivateProfileStringA("VRHUD", "FrameGuard", buf, g_iniPath);

        VrCtlPublish();
        PluginLog("[CONFIG] Settings committed to %s", g_iniPath);
    }

    void CheckDebounceCommit() {
        if (g_hasPendingSave && (GetTickCount64() - g_lastChangeTick >= 500)) {
            g_hasPendingSave = false;
            CommitSettingsToDisk();
        }
    }

    void UpdateTelemetry() {
        uint64_t now = GetTickCount64();
        static uint64_t s_lastSample = 0;
        if (now - s_lastSample < 200) return;
        s_lastSample = now;

        if (g_vrCtl && g_vrCtl->optiReady) {
            g_optiReady = true;
            if (g_vrCtl->gpuFrameMs > 0.1f) {
                g_gpuMsAvg = (g_gpuMsAvg * 0.7) + ((double)g_vrCtl->gpuFrameMs * 0.3);
            }
        }

        // Dynamic VR Frame Guard
        const int budgetHz = 72; // default target
        const double budgetMs = 1000.0 / (double)budgetHz;
        if (g_frameGuardActive && g_gpuMsAvg > (budgetMs - 0.9) && g_workingScale > 0.50f) {
            static int s_hotCount = 0;
            if (++s_hotCount >= 8) { // sustained pressure ~1.6s
                s_hotCount = 0;
                float oldScale = g_workingScale;
                if (g_workingScale > 0.70f) g_workingScale = 0.66f;
                else g_workingScale = 0.50f;
                g_frameGuardTriggered = true;
                g_hasPendingSave = true;
                g_lastChangeTick = now;
                VrCtlPublish();
                PluginLog("[GUARD] GPU %.2f ms / budget %.2f ms -> WorkingScale %.2f -> %.2f",
                          g_gpuMsAvg, budgetMs, oldScale, g_workingScale);
            }
        }
    }

    void DrawHUD() {
        if (!g_hudVisible) return;

        ImGui::SetNextWindowSize(ImVec2(540, 520), ImGuiCond_FirstUseEver);
        ImGui::Begin("DLSS 5 <> VR for UEVR (Neural Reconstruction)", &g_hudVisible);

        // Header Status
        ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "DLSS 5 Neural Reconstruction Engine");
        ImGui::SameLine();
        if (g_masterEnable) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "[ACTIVE]");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "[BYPASS]");
        }

        ImGui::Separator();

        // System Diagnostics Status
        ImGui::Text("OptiScaler Engine : %s", m_optiScalerLoaded ? "Loaded" : "NOT Loaded (Check OptiScaler.dll)");
        ImGui::SameLine();
        ImGui::Text("| IPC: %s", (g_vrCtl && g_vrCtl->magic == VRDLSS5_CTL_MAGIC) ? "Connected" : "Disconnected");

        // Telemetry Bar
        ImGui::Text("GPU: %.2f ms  |  WorkingScale: %.2fx  |  OptiScaler: %s", 
                    g_gpuMsAvg, g_workingScale, g_optiReady ? "Active" : "Waiting for game...");
        if (g_frameGuardTriggered) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[Frame Guard] Scale throttled to protect VR frame budget");
        }

        ImGui::Spacing();

        // Main Controls
        bool changed = false;

        if (ImGui::Checkbox("Enable Neural Engine (DLSS 5)", &g_masterEnable)) changed = true;

        if (ImGui::SliderFloat("VR WorkingScale", &g_workingScale, 0.25f, 1.50f, "%.2fx")) changed = true;
        ImGui::SameLine();
        if (ImGui::Button("Reset 0.75x")) { g_workingScale = 0.75f; changed = true; }

        if (ImGui::Checkbox("Run Before SR (Pre-SR Multipass)", &g_runBeforeSR)) changed = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("ResidualAcrossRR", &g_residualAcrossRR)) changed = true;

        const char* presets[] = { "0 - Default", "1 - Quality", "2 - Performance (VR Recommended)" };
        if (ImGui::Combo("AI Model Preset", &g_nrPreset, presets, IM_ARRAYSIZE(presets))) changed = true;

        const char* styles[] = { "0 - Standard", "1 - Natural", "2 - Cinematic" };
        if (ImGui::Combo("DLSS 5 Style", &g_nrStyle, styles, IM_ARRAYSIZE(styles))) changed = true;

        if (ImGui::SliderFloat("DLSS 5 Intensity", &g_nrIntensity, 0.0f, 2.0f, "%.2fx")) changed = true;

        ImGui::Spacing();
        ImGui::Separator();

        // Extended Parameters Collapsing Header
        if (ImGui::CollapsingHeader("Advanced Rendering & World Parameters")) {
            if (ImGui::SliderFloat("World Structure", &g_nrLocalStructure, 0.0f, 2.0f, "%.2fx")) changed = true;
            if (ImGui::SliderFloat("Shadow Tone", &g_nrLocalTone, 0.0f, 2.0f, "%.2fx")) changed = true;
            if (ImGui::Checkbox("Auto Mask (Characters & Faces)", &g_nrAutoMask)) changed = true;
            if (ImGui::SliderFloat("Skin Structure", &g_nrSkinStructure, -1.0f, 2.0f, "%.2fx")) changed = true;

            const char* passItems[] = { "1 - Single Pass", "2 - Double Pass (Dense)", "3 - Triple Pass (Hyper)" };
            int passIdx = g_nrPasses - 1;
            if (ImGui::Combo("AI Passes", &passIdx, passItems, IM_ARRAYSIZE(passItems))) {
                g_nrPasses = passIdx + 1;
                changed = true;
            }

            if (ImGui::SliderFloat("Texture Transfer", &g_nrTransferStrength, 0.0f, 2.0f, "%.2fx")) changed = true;
            if (ImGui::SliderFloat("Color Strength", &g_nrColourStrength, 0.0f, 1.0f, "%.2fx")) changed = true;
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Frame Guard and Safety
        if (ImGui::Checkbox("Dynamic VR Frame Guard (Auto-Drop Scale)", &g_frameGuardActive)) changed = true;

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "UEVR Menu: L3 + R3 (Native) | Shortcuts: F6 / Home");

        if (changed) {
            g_hasPendingSave = true;
            g_lastChangeTick = GetTickCount64();
            VrCtlPublish();
        }

        ImGui::End();
    }

private:
    std::mutex m_imgui_mutex;
    bool m_initialized{false};
    bool m_was_rendering_desktop{false};
    bool m_optiScalerLoaded{false};
    HWND m_wnd{NULL};

    // State & Settings
    bool g_hudVisible{false};
    bool g_masterEnable{false};
    float g_workingScale{0.75f};
    bool g_runBeforeSR{true};
    int g_nrPreset{2};
    bool g_residualAcrossRR{false};
    float g_nrIntensity{1.00f};
    int g_nrStyle{0};

    float g_nrLocalStructure{1.00f};
    float g_nrLocalTone{0.00f};
    bool g_nrAutoMask{true};
    float g_nrSkinStructure{-1.00f};
    float g_nrTransferStrength{1.00f};
    float g_nrColourStrength{1.00f};
    int g_nrPasses{1};

    bool g_frameGuardActive{true};
    bool g_frameGuardTriggered{false};
    double g_gpuMsAvg{0.0};
    bool g_optiReady{false};

    char g_iniPath[MAX_PATH]{""};
    bool g_hasPendingSave{false};
    uint64_t g_lastChangeTick{0};

    HANDLE g_vrCtlMap{NULL};
    VrDlss5ControlBlock* g_vrCtl{NULL};
};

// Instantiate the plugin
static VRDLSS5Plugin g_vrdlss5_plugin{};
