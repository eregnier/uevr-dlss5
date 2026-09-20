#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <xinput.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#define OPENVR_BUILD_STATIC
#include "openvr.h"
#include "MinHook.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID riid, void **ppFactory);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID riid, void **ppFactory);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void **ppFactory);
typedef HRESULT (WINAPI *PFN_DXGIDeclareAdapterRemovalSupport)();
typedef HRESULT (WINAPI *PFN_DXGIGetDebugInterface1)(UINT Flags, REFIID riid, void **pDebug);

// NGX Prototypes
typedef int (WINAPI *PFN_NVSDK_NGX_D3D12_CreateFeature)(void* pCmdList, int FeatureId, void* pParameters, void** ppHandle);
typedef int (WINAPI *PFN_NVSDK_NGX_D3D12_EvaluateFeature)(void* pCmdList, void* pHandle, void* pParameters, void* pCallback);
typedef int (WINAPI *PFN_NVSDK_NGX_D3D12_ReleaseFeature)(void* pHandle);

static PFN_NVSDK_NGX_D3D12_CreateFeature g_pfnNGXCreateFeature = NULL;
static PFN_NVSDK_NGX_D3D12_EvaluateFeature g_pfnNGXEvaluateFeature = NULL;
static PFN_NVSDK_NGX_D3D12_ReleaseFeature g_pfnNGXReleaseFeature = NULL;
static unsigned long long g_evalFrameCounter = 0;
extern "C" int WINAPI Proxy_NVSDK_NGX_D3D12_EvaluateFeature(void* pCmdList, void* pHandle, void* pParameters, void* pCallback);

static HMODULE g_hRealVR = NULL;
static HMODULE g_hOptiScaler = NULL;
static HMODULE g_hSysDxgi = NULL;

static PFN_CreateDXGIFactory g_pfnCreateDXGIFactory = NULL;
static PFN_CreateDXGIFactory1 g_pfnCreateDXGIFactory1 = NULL;
static PFN_CreateDXGIFactory2 g_pfnCreateDXGIFactory2 = NULL;
static PFN_DXGIDeclareAdapterRemovalSupport g_pfnDXGIDeclareAdapterRemovalSupport = NULL;
static PFN_DXGIGetDebugInterface1 g_pfnDXGIGetDebugInterface1 = NULL;

static void LogMsg(const char *msg)
{
    static char logPath[MAX_PATH] = "";
    if (logPath[0] == '\0')
    {
        if (GetModuleFileNameA(NULL, logPath, MAX_PATH))
        {
            char* lastSlash = strrchr(logPath, '\\');
            if (lastSlash)
            {
                *(lastSlash + 1) = '\0';
                strcat_s(logPath, MAX_PATH, "vr_dlss5_proxy.log");
            }
            else
            {
                strcpy_s(logPath, MAX_PATH, "vr_dlss5_proxy.log");
            }
        }
    }
    FILE *f = NULL;
    fopen_s(&f, logPath, "a");
    if (f)
    {
        char exePath[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        const char* base = strrchr(exePath, '\\');
        base = base ? base + 1 : exePath;
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d][%s:%lu] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                base, GetCurrentProcessId(), msg);
        fflush(f);
        fclose(f);
    }
}

static bool g_hudVisible = false;
static uint64_t g_bootTick = 0;
static DWORD g_inputWatcherThreadId = 0;
static DWORD WINAPI InputWatcherThread(LPVOID lpParam);

// ----------------------------------------------------------------------------
// VR-DLSS5 live control channel (mirror of OptiScaler/DLSSNR fork's Config.h).
// OptiScaler only reads OptiScaler.ini at startup, so the HUD publishes desired
// values here and OptiScaler's worker applies them to Config in real time.
// ----------------------------------------------------------------------------
#define VRDLSS5_CTL_MAGIC 0x354C4456u // 'VDL5'
#define VRDLSS5_CTL_VERSION 3u
#define VRDLSS5_CTL_MAPPING_NAME "Local\\VRDLSS5_Control_1"

#pragma pack(push, 4)
struct VrDlss5ControlBlock
{
    unsigned int magic;
    unsigned int version;
    volatile LONG seq;
    volatile LONG ackSeq;
    volatile LONG optiReady;
    volatile LONG enabled;
    float workingScale;
    volatile LONG runBeforeSR;
    volatile LONG residualAcrossRR;
    volatile LONG preset;
    float intensity;
    volatile LONG style;
    volatile LONG featureRunning;
    float gpuFrameMs;
    float localStructure;      // desired DlssNr.LocalStructure (0.00 - 2.00)
    float localTone;           // desired DlssNr.LocalTone (0.00 - 2.00)
    volatile LONG autoMask;    // desired DlssNr.AutoMask (0 / 1)
    float skinStructure;       // desired DlssNr.SkinStructure (-1.00 - 2.00)
    float transferStrength;    // desired DlssNr.TransferStrength (0.00 - 2.00)
    float colourStrength;      // desired DlssNr.ColourStrength (0.00 - 1.00)
    volatile LONG passes;      // desired DlssNr.Passes (1 - 3)
    volatile LONG reserved[8];
};
#pragma pack(pop)

static HANDLE g_vrCtlMap = NULL;
static VrDlss5ControlBlock* g_vrCtl = NULL;

static bool VrCtlEnsure()
{
    if (g_vrCtl) return true;
    if (!g_vrCtlMap)
    {
        // Per-process name: two games using the tool at once must not cross-talk.
        char mappingName[96];
        sprintf_s(mappingName, sizeof(mappingName), "%s_%lu", VRDLSS5_CTL_MAPPING_NAME,
                  GetCurrentProcessId());
        g_vrCtlMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                        sizeof(VrDlss5ControlBlock), mappingName);
    }
    if (!g_vrCtlMap) return false;

    g_vrCtl = (VrDlss5ControlBlock*)MapViewOfFile(g_vrCtlMap, FILE_MAP_ALL_ACCESS, 0, 0,
                                                  sizeof(VrDlss5ControlBlock));
    if (!g_vrCtl) return false;

    if (g_vrCtl->magic != VRDLSS5_CTL_MAGIC || g_vrCtl->version != VRDLSS5_CTL_VERSION)
    {
        memset(g_vrCtl, 0, sizeof(VrDlss5ControlBlock));
        g_vrCtl->magic = VRDLSS5_CTL_MAGIC;
        g_vrCtl->version = VRDLSS5_CTL_VERSION;
    }
    return true;
}

// Auxiliary child processes (error/crash reporters) also load this dxgi.dll from
// the game folder. They must not create an OpenVR overlay, hook XInput or poll
// the HUD: that is what produced the CreateOverlay(17)/SetOverlayRaw(12) storm.
static bool IsAuxiliaryProcess()
{
    char exePath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(NULL, exePath, MAX_PATH))
        return false;

    const char* base = strrchr(exePath, '\\');
    base = base ? base + 1 : exePath;

    return _strnicmp(base, "nvngx_update", 12) == 0 ||
           _strnicmp(base, "CrashReportClient", 17) == 0 ||
           _strnicmp(base, "UnrealCEFSubProcess", 19) == 0 ||
           _strnicmp(base, "EpicWebHelper", 13) == 0 ||
           _strnicmp(base, "ShaderCompileWorker", 19) == 0 ||
           _strnicmp(base, "UnrealVersionSelector", 21) == 0 ||
           _strnicmp(base, "REDEngineErrorReporter", 22) == 0 ||
           _strnicmp(base, "CrashReporter", 13) == 0 ||
           _strnicmp(base, "REDprelauncher", 14) == 0 ||
           _strnicmp(base, "ErrorReporter", 13) == 0 ||
           _strnicmp(base, "UbisoftConnectInstaller", 23) == 0 ||
           _strnicmp(base, "Uplay", 5) == 0 ||
           _strnicmp(base, "upc", 3) == 0 ||
           _strnicmp(base, "elevate", 7) == 0 ||
           _strnicmp(base, "rxrepl", 6) == 0 ||
           _strnicmp(base, "Vdesync", 7) == 0 ||
           _strnicmp(base, "xrsetruntime", 12) == 0 ||
           _strnicmp(base, "RealFluid", 9) == 0 ||
           _strnicmp(base, "ReplaceJSON", 11) == 0;
}


// ----------------------------------------------------------------------------
// Universal Input Isolation: Windows Message Hooks (PeekMessage / GetMessage / GetRawInputData)
// and XInput / WinMM filters.
// Completely prevents gamepad D-Pad, face buttons (A/B) and navigation keys from
// leaking to the game engine when the VR HUD overlay is active, across all input APIs:
// - Windows Raw Input (WM_INPUT / RIM_TYPEHID) used by DualSense/DirectInput in Cyberpunk & Outlaws
// - DirectInput POV hat & buttons in winmm.dll
// - XInput wButtons in XINPUT1_4, XINPUT9_1_0, XINPUT1_3 and RealVR64
// - Keyboard navigation keys (WM_KEYDOWN / WM_KEYUP)
// ----------------------------------------------------------------------------
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, XINPUT_STATE* pState);
typedef MMRESULT (WINAPI *PFN_joyGetPosEx)(UINT uJoyID, LPJOYINFOEX pji);

typedef BOOL (WINAPI *PFN_PeekMessageW)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
typedef BOOL (WINAPI *PFN_PeekMessageA)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
typedef BOOL (WINAPI *PFN_GetMessageW)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
typedef BOOL (WINAPI *PFN_GetMessageA)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
typedef UINT (WINAPI *PFN_GetRawInputData)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
typedef UINT (WINAPI *PFN_GetRawInputBuffer)(PRAWINPUT pData, PUINT pcbSize, UINT cbSizeHeader);

static PFN_XInputGetState g_origXInput1_4_GetState = NULL;
static PFN_XInputGetState g_origXInput1_4_Ex = NULL;
static PFN_XInputGetState g_origXInput1_3_GetState = NULL;
static PFN_XInputGetState g_origXInput9_1_0_GetState = NULL;
static PFN_XInputGetState g_origRealVR_GetState = NULL;
static PFN_joyGetPosEx g_origJoyGetPosEx = NULL;

static PFN_PeekMessageW g_origPeekMessageW = NULL;
static PFN_PeekMessageA g_origPeekMessageA = NULL;
static PFN_GetMessageW g_origGetMessageW = NULL;
static PFN_GetMessageA g_origGetMessageA = NULL;
static PFN_GetRawInputData g_origGetRawInputData = NULL;
static PFN_GetRawInputBuffer g_origGetRawInputBuffer = NULL;

static volatile LONG g_dpadMaskedSamples = 0;

static inline bool IsHudNavigationKey(WPARAM vk)
{
    return (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
            vk == VK_RETURN || vk == VK_SPACE || vk == VK_ESCAPE || vk == VK_TAB ||
            vk == VK_BACK || vk == VK_F6 || vk == VK_F7 || vk == VK_F8);
}

static inline bool FilterWindowMessage(LPMSG lpMsg)
{
    if (!lpMsg || !g_hudVisible) return false;

    // 1. RawInput messages (gamepad / joystick / controller HID reports)
    if (lpMsg->message == WM_INPUT)
    {
        RAWINPUTHEADER hdr;
        UINT size = sizeof(hdr);
        PFN_GetRawInputData pfnGetRaw = g_origGetRawInputData ? g_origGetRawInputData : GetRawInputData;
        if (pfnGetRaw((HRAWINPUT)lpMsg->lParam, RID_HEADER, &hdr, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1)
        {
            if (hdr.dwType == RIM_TYPEHID)
            {
                // Mask gamepad HID reports completely while HUD is open
                lpMsg->message = WM_NULL;
                lpMsg->wParam = 0;
                lpMsg->lParam = 0;
                InterlockedIncrement(&g_dpadMaskedSamples);
                return true;
            }
            else if (hdr.dwType == RIM_TYPEKEYBOARD)
            {
                RAWINPUT raw;
                UINT rawSize = sizeof(raw);
                if (pfnGetRaw((HRAWINPUT)lpMsg->lParam, RID_INPUT, &raw, &rawSize, sizeof(RAWINPUTHEADER)) != (UINT)-1)
                {
                    if (IsHudNavigationKey(raw.data.keyboard.VKey))
                    {
                        lpMsg->message = WM_NULL;
                        lpMsg->wParam = 0;
                        lpMsg->lParam = 0;
                        InterlockedIncrement(&g_dpadMaskedSamples);
                        return true;
                    }
                }
            }
        }
        else
        {
            // If header retrieval fails while HUD is active, conservatively neutralize
            lpMsg->message = WM_NULL;
            lpMsg->wParam = 0;
            lpMsg->lParam = 0;
            return true;
        }
    }
    // 2. Keyboard messages: filter arrow keys, space, enter, escape, backspace, tab, F6-F8
    else if (lpMsg->message == WM_KEYDOWN || lpMsg->message == WM_KEYUP ||
             lpMsg->message == WM_SYSKEYDOWN || lpMsg->message == WM_SYSKEYUP)
    {
        if (IsHudNavigationKey(lpMsg->wParam))
        {
            lpMsg->message = WM_NULL;
            lpMsg->wParam = 0;
            lpMsg->lParam = 0;
            InterlockedIncrement(&g_dpadMaskedSamples);
            return true;
        }
    }

    return false;
}

static BOOL WINAPI Hooked_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
{
    BOOL res = g_origPeekMessageW ? g_origPeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg) : FALSE;
    if (res && lpMsg) FilterWindowMessage(lpMsg);
    return res;
}

static BOOL WINAPI Hooked_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
{
    BOOL res = g_origPeekMessageA ? g_origPeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg) : FALSE;
    if (res && lpMsg) FilterWindowMessage(lpMsg);
    return res;
}

static BOOL WINAPI Hooked_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    BOOL res = g_origGetMessageW ? g_origGetMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax) : FALSE;
    if (res > 0 && lpMsg) FilterWindowMessage(lpMsg);
    return res;
}

static BOOL WINAPI Hooked_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    BOOL res = g_origGetMessageA ? g_origGetMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax) : FALSE;
    if (res > 0 && lpMsg) FilterWindowMessage(lpMsg);
    return res;
}

static UINT WINAPI Hooked_GetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader)
{
    UINT res = g_origGetRawInputData ? g_origGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader) : (UINT)-1;
    if (res == (UINT)-1 || !pData || !g_hudVisible) return res;

    if (uiCommand == RID_INPUT && res >= sizeof(RAWINPUTHEADER))
    {
        RAWINPUT* pRaw = (RAWINPUT*)pData;
        if (pRaw->header.dwType == RIM_TYPEHID)
        {
            if (pRaw->data.hid.dwSizeHid > 0 && pRaw->data.hid.dwCount > 0)
            {
                size_t totalBytes = (size_t)pRaw->data.hid.dwSizeHid * pRaw->data.hid.dwCount;
                ZeroMemory(pRaw->data.hid.bRawData, totalBytes);
                InterlockedIncrement(&g_dpadMaskedSamples);
            }
        }
    }
    return res;
}

static UINT WINAPI Hooked_GetRawInputBuffer(PRAWINPUT pData, PUINT pcbSize, UINT cbSizeHeader)
{
    UINT res = g_origGetRawInputBuffer ? g_origGetRawInputBuffer(pData, pcbSize, cbSizeHeader) : (UINT)-1;
    if (res == (UINT)-1 || !pData || !g_hudVisible) return res;

    PRAWINPUT cur = pData;
    for (UINT i = 0; i < res; i++)
    {
        if (cur->header.dwType == RIM_TYPEHID)
        {
            if (cur->data.hid.dwSizeHid > 0 && cur->data.hid.dwCount > 0)
            {
                size_t totalBytes = (size_t)cur->data.hid.dwSizeHid * cur->data.hid.dwCount;
                ZeroMemory(cur->data.hid.bRawData, totalBytes);
                InterlockedIncrement(&g_dpadMaskedSamples);
            }
        }
        cur = (PRAWINPUT)(((uintptr_t)cur + cur->header.dwSize + 7) & ~(uintptr_t)7);
    }
    return res;
}

static inline DWORD FilterXInputState(DWORD dwUserIndex, XINPUT_STATE* pState, DWORD result)
{
    // If called from our own InputWatcherThread, NEVER mask so that HUD can be navigated freely
    if (GetCurrentThreadId() == g_inputWatcherThreadId) {
        return result;
    }

    // When the VR HUD is visible, mask out D-pad AND navigation buttons (A, B, Select, L3)
    if (result == ERROR_SUCCESS && pState && g_hudVisible) {
        pState->Gamepad.wButtons &= ~(XINPUT_GAMEPAD_DPAD_UP |
                                      XINPUT_GAMEPAD_DPAD_DOWN |
                                      XINPUT_GAMEPAD_DPAD_LEFT |
                                      XINPUT_GAMEPAD_DPAD_RIGHT |
                                      XINPUT_GAMEPAD_A |
                                      XINPUT_GAMEPAD_B |
                                      XINPUT_GAMEPAD_BACK |
                                      XINPUT_GAMEPAD_LEFT_THUMB);
        InterlockedIncrement(&g_dpadMaskedSamples);
    }
    return result;
}

static DWORD WINAPI Hooked_XInput1_4_GetState(DWORD dwUserIndex, XINPUT_STATE* pState)
{
    DWORD res = g_origXInput1_4_GetState ? g_origXInput1_4_GetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    return FilterXInputState(dwUserIndex, pState, res);
}

static DWORD WINAPI Hooked_XInput1_4_Ex(DWORD dwUserIndex, XINPUT_STATE* pState)
{
    DWORD res = g_origXInput1_4_Ex ? g_origXInput1_4_Ex(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    return FilterXInputState(dwUserIndex, pState, res);
}

static DWORD WINAPI Hooked_XInput1_3_GetState(DWORD dwUserIndex, XINPUT_STATE* pState)
{
    DWORD res = g_origXInput1_3_GetState ? g_origXInput1_3_GetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    return FilterXInputState(dwUserIndex, pState, res);
}

static DWORD WINAPI Hooked_XInput9_1_0_GetState(DWORD dwUserIndex, XINPUT_STATE* pState)
{
    DWORD res = g_origXInput9_1_0_GetState ? g_origXInput9_1_0_GetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    return FilterXInputState(dwUserIndex, pState, res);
}

static __declspec(thread) int t_realVrStubDepth = 0;

static DWORD WINAPI Hooked_RealVR_GetState(DWORD dwUserIndex, XINPUT_STATE* pState)
{
    if (t_realVrStubDepth > 0)
        return g_origRealVR_GetState ? g_origRealVR_GetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;

    t_realVrStubDepth++;
    DWORD res = g_origRealVR_GetState ? g_origRealVR_GetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    t_realVrStubDepth--;

    return FilterXInputState(dwUserIndex, pState, res);
}

static MMRESULT WINAPI Hooked_joyGetPosEx(UINT uJoyID, LPJOYINFOEX pji)
{
    MMRESULT res = g_origJoyGetPosEx ? g_origJoyGetPosEx(uJoyID, pji) : JOYERR_PARMS;
    if (GetCurrentThreadId() == g_inputWatcherThreadId) return res;
    if (res == JOYERR_NOERROR && pji && g_hudVisible) {
        pji->dwPOV = JOY_POVCENTERED; // 0xFFFF = neutral POV hat (D-pad centered)
        pji->dwButtons = 0;           // mask all buttons
        InterlockedIncrement(&g_dpadMaskedSamples);
    }
    return res;
}

static void InstallXInputHooks()
{
    static bool s_minHookInited = false;
    if (!s_minHookInited) {
        MH_STATUS st = MH_Initialize();
        if (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) {
            s_minHookInited = true;
            LogMsg("[Proxy-Input] MinHook engine initialized successfully");
        } else {
            char buf[128];
            sprintf_s(buf, sizeof(buf), "[Proxy-Input] MinHook init error: %d", st);
            LogMsg(buf);
            return;
        }
    }

    // 1. Hook user32.dll (PeekMessageW/A, GetMessageW/A, GetRawInputData, GetRawInputBuffer)
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        if (!g_origPeekMessageW) {
            void* p = (void*)GetProcAddress(hUser32, "PeekMessageW");
            if (p && MH_CreateHook(p, (LPVOID)&Hooked_PeekMessageW, (LPVOID*)&g_origPeekMessageW) == MH_OK) {
                MH_EnableHook(p);
                LogMsg("[Proxy-Input] Hooked user32.dll!PeekMessageW (RawInput & Navigation filter armed)");
            }
        }
        if (!g_origPeekMessageA) {
            void* p = (void*)GetProcAddress(hUser32, "PeekMessageA");
            if (p && MH_CreateHook(p, (LPVOID)&Hooked_PeekMessageA, (LPVOID*)&g_origPeekMessageA) == MH_OK) {
                MH_EnableHook(p);
                LogMsg("[Proxy-Input] Hooked user32.dll!PeekMessageA (RawInput & Navigation filter armed)");
            }
        }
        if (!g_origGetMessageW) {
            void* p = (void*)GetProcAddress(hUser32, "GetMessageW");
            if (p && MH_CreateHook(p, (LPVOID)&Hooked_GetMessageW, (LPVOID*)&g_origGetMessageW) == MH_OK) {
                MH_EnableHook(p);
                LogMsg("[Proxy-Input] Hooked user32.dll!GetMessageW (RawInput & Navigation filter armed)");
            }
        }
        if (!g_origGetMessageA) {
            void* p = (void*)GetProcAddress(hUser32, "GetMessageA");
            if (p && MH_CreateHook(p, (LPVOID)&Hooked_GetMessageA, (LPVOID*)&g_origGetMessageA) == MH_OK) {
                MH_EnableHook(p);
                LogMsg("[Proxy-Input] Hooked user32.dll!GetMessageA (RawInput & Navigation filter armed)");
            }
        }
        if (!g_origGetRawInputData) {
            void* p = (void*)GetProcAddress(hUser32, "GetRawInputData");
            if (p && MH_CreateHook(p, (LPVOID)&Hooked_GetRawInputData, (LPVOID*)&g_origGetRawInputData) == MH_OK) {
                MH_EnableHook(p);
                LogMsg("[Proxy-Input] Hooked user32.dll!GetRawInputData (HID report zeroing armed)");
            }
        }
        if (!g_origGetRawInputBuffer) {
            void* p = (void*)GetProcAddress(hUser32, "GetRawInputBuffer");
            if (p && MH_CreateHook(p, (LPVOID)&Hooked_GetRawInputBuffer, (LPVOID*)&g_origGetRawInputBuffer) == MH_OK) {
                MH_EnableHook(p);
                LogMsg("[Proxy-Input] Hooked user32.dll!GetRawInputBuffer (HID report zeroing armed)");
            }
        }
    }

    // 2. Hook XINPUT1_4.dll
    HMODULE hX14 = GetModuleHandleA("XINPUT1_4.dll");
    if (!hX14) hX14 = LoadLibraryA("XINPUT1_4.dll");
    if (hX14) {
        if (!g_origXInput1_4_GetState) {
            void* pTarget = (void*)GetProcAddress(hX14, "XInputGetState");
            if (pTarget && MH_CreateHook(pTarget, (LPVOID)&Hooked_XInput1_4_GetState, (LPVOID*)&g_origXInput1_4_GetState) == MH_OK) {
                MH_EnableHook(pTarget);
                LogMsg("[Proxy-Input] Hooked XINPUT1_4.dll!XInputGetState (D-Pad filter armed)");
            }
        }
        if (!g_origXInput1_4_Ex) {
            void* pEx = (void*)GetProcAddress(hX14, (LPCSTR)100);
            if (pEx && pEx != (void*)g_origXInput1_4_GetState) {
                if (MH_CreateHook(pEx, (LPVOID)&Hooked_XInput1_4_Ex, (LPVOID*)&g_origXInput1_4_Ex) == MH_OK) {
                    MH_EnableHook(pEx);
                    LogMsg("[Proxy-Input] Hooked XINPUT1_4.dll!XInputGetStateEx (ordinal 100 armed)");
                }
            }
        }
    }

    // 3. Hook XINPUT1_3.dll if loaded
    HMODULE hX13 = GetModuleHandleA("XINPUT1_3.dll");
    if (hX13 && !g_origXInput1_3_GetState) {
        void* pTarget = (void*)GetProcAddress(hX13, "XInputGetState");
        if (pTarget && MH_CreateHook(pTarget, (LPVOID)&Hooked_XInput1_3_GetState, (LPVOID*)&g_origXInput1_3_GetState) == MH_OK) {
            MH_EnableHook(pTarget);
            LogMsg("[Proxy-Input] Hooked XINPUT1_3.dll!XInputGetState (D-Pad filter armed)");
        }
    }

    // 4. Hook XINPUT9_1_0.dll if loaded
    HMODULE hX9 = GetModuleHandleA("XINPUT9_1_0.dll");
    if (hX9 && !g_origXInput9_1_0_GetState) {
        void* pTarget = (void*)GetProcAddress(hX9, "XInputGetState");
        if (pTarget && MH_CreateHook(pTarget, (LPVOID)&Hooked_XInput9_1_0_GetState, (LPVOID*)&g_origXInput9_1_0_GetState) == MH_OK) {
            MH_EnableHook(pTarget);
            LogMsg("[Proxy-Input] Hooked XINPUT9_1_0.dll!XInputGetState (D-Pad filter armed)");
        }
    }

    // 5. Hook joyGetPosEx in winmm.dll
    HMODULE hWinmm = GetModuleHandleA("winmm.dll");
    if (hWinmm && !g_origJoyGetPosEx) {
        void* pTarget = (void*)GetProcAddress(hWinmm, "joyGetPosEx");
        if (pTarget && MH_CreateHook(pTarget, (LPVOID)&Hooked_joyGetPosEx, (LPVOID*)&g_origJoyGetPosEx) == MH_OK) {
            MH_EnableHook(pTarget);
            LogMsg("[Proxy-Input] Hooked winmm.dll!joyGetPosEx (POV hat neutralizer armed)");
        }
    }

    // 6. Bind RealVR64 export if loaded
    if (g_hRealVR && !g_origRealVR_GetState) {
        void* pRealVr = (void*)GetProcAddress(g_hRealVR, "XInputGetState");
        if (pRealVr) {
            g_origRealVR_GetState = (PFN_XInputGetState)pRealVr;
            LogMsg("[Proxy-Input] Bound g_origRealVR_GetState to RealVR64 export");
        }
    }
}

// ----------------------------------------------------------------------------
// RealVR64 XInput stub patch: LukeRoss rewrites the XInput entry points to a
// writable thunk ("E9 -> FF 25 [slot] -> RealVR64 code"). The watcher polls the
// captured RealVR64 function so Select+L3 keeps working; the thunk target is
// repointed to our detour so the D-Pad filter applies when the HUD is open.
// ----------------------------------------------------------------------------
static void** g_realVrStubSlot = NULL;
static void* g_realVrStubTarget = NULL;

static bool IsAddressInRealVR(void* p)
{
    if (!g_hRealVR || !p) return false;
    HMODULE hOwner = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)p, &hOwner))
        return false;
    return hOwner == g_hRealVR;
}

static bool InstallRealVRXInputStubPatch()
{
    if (!g_hRealVR) return false;

    void* pRealVrGetState = (void*)GetProcAddress(g_hRealVR, "XInputGetState");
    if (!pRealVrGetState) return false;

    if (!g_origRealVR_GetState) {
        g_origRealVR_GetState = (PFN_XInputGetState)pRealVrGetState;
    }

    if (g_realVrStubSlot && *g_realVrStubSlot == (void*)&Hooked_RealVR_GetState)
        return true;

    static unsigned int s_diagLoggedMask = 0;

    const char* modules[] = { "XINPUT9_1_0.dll", "XINPUT1_4.dll" };
    for (int mi = 0; mi < 2; mi++)
    {
        const char* moduleName = modules[mi];
        HMODULE hMod = GetModuleHandleA(moduleName);
        if (!hMod) continue;

        unsigned char* pFn = (unsigned char*)GetProcAddress(hMod, "XInputGetState");
        if (!pFn || pFn[0] != 0xE9) continue;

        int rel = 0;
        memcpy(&rel, pFn + 1, sizeof(rel));
        unsigned char* stub = pFn + 5 + rel;

        if (stub[0] != 0xFF || stub[1] != 0x25) continue;
        void** slot = (void**)(stub + 6);

        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) continue;
        if (mbi.Protect == PAGE_NOACCESS || mbi.Protect == PAGE_GUARD) continue;

        // RealVR64 may point the thunk at an internal variant rather than its
        // exported XInputGetState: accept any target inside the module.
        if (!IsAddressInRealVR(*slot))
        {
            if (!(s_diagLoggedMask & (1u << mi)))
            {
                s_diagLoggedMask |= (1u << mi);
                char dbg[256];
                sprintf_s(dbg, sizeof(dbg),
                    "[Proxy-Input] DIAG %s slot target=%p is not RealVR64 (skipped)", moduleName, *slot);
                LogMsg(dbg);
            }
            continue;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) continue;

        g_origRealVR_GetState = (PFN_XInputGetState)*slot;
        g_realVrStubTarget = *slot;
        g_realVrStubSlot = slot;
        InterlockedExchangePointer((PVOID volatile*)slot, (PVOID)&Hooked_RealVR_GetState);
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);

        char buf[256];
        sprintf_s(buf, sizeof(buf),
            "[Proxy-Input] RealVR64 XInput stub redirected in %s (target=%p, D-Pad filter LIVE)",
            moduleName, g_realVrStubTarget);
        LogMsg(buf);
        return true;
    }

    return false;
}

static void MaintainRealVRXInputStubPatch()
{
    if (!g_hRealVR) return;

    if (g_realVrStubSlot)
    {
        if (*g_realVrStubSlot != (void*)&Hooked_RealVR_GetState)
        {
            DWORD oldProtect = 0;
            if (VirtualProtect(g_realVrStubSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
            {
                InterlockedExchangePointer((PVOID volatile*)g_realVrStubSlot,
                                           (PVOID)&Hooked_RealVR_GetState);
                VirtualProtect(g_realVrStubSlot, sizeof(void*), oldProtect, &oldProtect);
                LogMsg("[Proxy-Input] RealVR64 XInput stub re-patched after external overwrite");
            }
        }
        return;
    }

    InstallRealVRXInputStubPatch();
}

static void InitProxy()
{
    static BOOL initialized = FALSE;
    if (initialized) return;
    initialized = TRUE;
    if (g_bootTick == 0) g_bootTick = GetTickCount64();

    LogMsg("[Proxy] Initializing VR-DLSS5 Dual Proxy (OptiScaler Pre-SR Engine)...");

    static bool s_auxiliary = false;
    static bool s_vrHost = false;

    if (IsAuxiliaryProcess())
    {
        s_auxiliary = true;
        LogMsg("[Proxy] Auxiliary process detected: VR HUD / OpenVR / input hooks disabled (DXGI forwarding only)");
    }

    // 1. Charger OptiScaler Pre-SR Engine (OptiScaler.asi / OptiScaler.dll / dbghelp.dll)
    if (!s_auxiliary)
    {
    g_hOptiScaler = LoadLibraryA("OptiScaler.asi");
    if (!g_hOptiScaler) g_hOptiScaler = LoadLibraryA("OptiScaler.dll");
    if (!g_hOptiScaler) g_hOptiScaler = LoadLibraryA("dbghelp.dll");
    if (g_hOptiScaler)
    {
        char optiPath[MAX_PATH] = {0};
        GetModuleFileNameA(g_hOptiScaler, optiPath, MAX_PATH);
        char optiBuf[MAX_PATH + 64];
        sprintf_s(optiBuf, sizeof(optiBuf), "[Proxy] Successfully loaded OptiScaler Pre-SR Engine: %s", optiPath);
        LogMsg(optiBuf);
    }
    else
    {
        LogMsg("[Proxy] OptiScaler not loaded directly (will be loaded by RealVR64 as OptiScaler.asi if present)");
    }

    // 2. Charger RealVR64.dll (LukeRoss VR mod) ensuite
    g_hRealVR = LoadLibraryA("RealVR64.dll");
    if (g_hRealVR)
    {
        LogMsg("[Proxy] Successfully loaded RealVR64.dll");

        g_pfnCreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(g_hRealVR, "CreateDXGIFactory");
        g_pfnCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(g_hRealVR, "CreateDXGIFactory1");
        g_pfnCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_hRealVR, "CreateDXGIFactory2");
        g_pfnDXGIDeclareAdapterRemovalSupport = (PFN_DXGIDeclareAdapterRemovalSupport)GetProcAddress(g_hRealVR, "DXGIDeclareAdapterRemovalSupport");
        g_pfnDXGIGetDebugInterface1 = (PFN_DXGIGetDebugInterface1)GetProcAddress(g_hRealVR, "DXGIGetDebugInterface1");

        // Résolution directe sans altération de code ni trampoline instable
        g_pfnNGXCreateFeature = (PFN_NVSDK_NGX_D3D12_CreateFeature)GetProcAddress(g_hRealVR, "NVSDK_NGX_D3D12_CreateFeature");
        g_pfnNGXEvaluateFeature = (PFN_NVSDK_NGX_D3D12_EvaluateFeature)GetProcAddress(g_hRealVR, "NVSDK_NGX_D3D12_EvaluateFeature");
        g_pfnNGXReleaseFeature = (PFN_NVSDK_NGX_D3D12_ReleaseFeature)GetProcAddress(g_hRealVR, "NVSDK_NGX_D3D12_ReleaseFeature");
        LogMsg("[Proxy] RealVR64 exports resolved natively (zero memory corruption)");
    }
    else
    {
        LogMsg("[Proxy] WARNING: RealVR64.dll not found, falling back to system dxgi.dll");
    }
    } // !s_auxiliary

    // VR host = the real game process where LukeRoss is active. Only there do we
    // own an OpenVR overlay, XInput hooks and the HUD thread.
    s_vrHost = (!s_auxiliary && g_hRealVR != NULL);

    // 3. Repli de secours vers system32 dxgi si une fonction n'est pas dans RealVR
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, MAX_PATH, "\\dxgi.dll");
    g_hSysDxgi = LoadLibraryA(sysPath);

    if (g_hSysDxgi)
    {
        if (!g_pfnCreateDXGIFactory) g_pfnCreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(g_hSysDxgi, "CreateDXGIFactory");
        if (!g_pfnCreateDXGIFactory1) g_pfnCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(g_hSysDxgi, "CreateDXGIFactory1");
        if (!g_pfnCreateDXGIFactory2) g_pfnCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_hSysDxgi, "CreateDXGIFactory2");
        if (!g_pfnDXGIDeclareAdapterRemovalSupport) g_pfnDXGIDeclareAdapterRemovalSupport = (PFN_DXGIDeclareAdapterRemovalSupport)GetProcAddress(g_hSysDxgi, "DXGIDeclareAdapterRemovalSupport");
        if (!g_pfnDXGIGetDebugInterface1) g_pfnDXGIGetDebugInterface1 = (PFN_DXGIGetDebugInterface1)GetProcAddress(g_hSysDxgi, "DXGIGetDebugInterface1");
    }

    if (!s_vrHost)
    {
        LogMsg("[Proxy] Proxy ready (VR features disabled in this process).");
        return;
    }

    // 4. Installer les hooks d'interception D-Pad (MinHook)
    InstallXInputHooks();

    // 4b. Neutraliser la redirection XInput de RealVR64 (le jeu appelle son
    // export, pas la fonction système : les hooks ci-dessus ne suffisent pas).
    if (!InstallRealVRXInputStubPatch())
        LogMsg("[Proxy-Input] RealVR64 XInput stub not ready yet (watcher will retry)");

    // 5. Lancer le thread d'écoute autonome pour F6 et Select+L3
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InputWatcherThread, NULL, 0, &g_inputWatcherThreadId);

    // NOTE: Do NOT call SetPriorityClass/SetThreadPriority here — this runs during
    // DllMain (loader lock) and alters thread scheduling during the D3D12/OpenXR
    // initialization sequence, causing XR_ERROR_CALL_ORDER_INVALID in RealVR64.
    // The benefit is marginal for a GPU-bound VR workload anyway.

    LogMsg("[Proxy] Proxy ready (Autonomous Input Thread armed).");
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);
        InitProxy();
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void **ppFactory)
{
    InitProxy();
    LogMsg("[Proxy] Proxy_CreateDXGIFactory called");
    if (g_pfnCreateDXGIFactory) return g_pfnCreateDXGIFactory(riid, ppFactory);
    return E_FAIL;
}

HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void **ppFactory)
{
    InitProxy();
    LogMsg("[Proxy] Proxy_CreateDXGIFactory1 called");
    if (g_pfnCreateDXGIFactory1) return g_pfnCreateDXGIFactory1(riid, ppFactory);
    return E_FAIL;
}

HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)
{
    InitProxy();
    LogMsg("[Proxy] Proxy_CreateDXGIFactory2 called");
    if (g_pfnCreateDXGIFactory2) return g_pfnCreateDXGIFactory2(Flags, riid, ppFactory);
    return E_FAIL;
}

HRESULT WINAPI Proxy_DXGIDeclareAdapterRemovalSupport()
{
    InitProxy();
    if (g_pfnDXGIDeclareAdapterRemovalSupport) return g_pfnDXGIDeclareAdapterRemovalSupport();
    return S_OK;
}

HRESULT WINAPI Proxy_DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **pDebug)
{
    InitProxy();
    if (g_pfnDXGIGetDebugInterface1) return g_pfnDXGIGetDebugInterface1(Flags, riid, pDebug);
    return E_FAIL;
}

typedef HRESULT (WINAPI *PFN_ApplyCompatResolutionQuirking)();
typedef HRESULT (WINAPI *PFN_CompatString)();
typedef HRESULT (WINAPI *PFN_CompatValue)();
typedef HRESULT (WINAPI *PFN_DXGID3D10CreateDevice)();
typedef HRESULT (WINAPI *PFN_DXGID3D10CreateLayeredDevice)();
typedef HRESULT (WINAPI *PFN_DXGID3D10GetLayeredDeviceSize)();
typedef HRESULT (WINAPI *PFN_DXGID3D10RegisterLayers)();
typedef HRESULT (WINAPI *PFN_DXGIDisableVBlankVirtualization)();
typedef HRESULT (WINAPI *PFN_DXGIDumpJournal)();
typedef HRESULT (WINAPI *PFN_DXGIReportAdapterConfiguration)();
typedef HRESULT (WINAPI *PFN_PIXBeginCapture)();
typedef HRESULT (WINAPI *PFN_PIXEndCapture)();
typedef HRESULT (WINAPI *PFN_PIXGetCaptureState)();
typedef HRESULT (WINAPI *PFN_SetAppCompatStringPointer)();
typedef HRESULT (WINAPI *PFN_UpdateHMDEmulationStatus)();

static void* ResolveProc(const char *name)
{
    void *p = NULL;
    if (g_hRealVR) p = (void*)GetProcAddress(g_hRealVR, name);
    if (!p && g_hSysDxgi) p = (void*)GetProcAddress(g_hSysDxgi, name);
    return p;
}

#define FORWARD_HR(name) \
    static PFN_##name s_pfn_##name = NULL; \
    if (!s_pfn_##name) s_pfn_##name = (PFN_##name)ResolveProc(#name); \
    if (s_pfn_##name) return s_pfn_##name(); \
    return S_OK;

HRESULT WINAPI Proxy_ApplyCompatResolutionQuirking() { InitProxy(); FORWARD_HR(ApplyCompatResolutionQuirking); }
HRESULT WINAPI Proxy_CompatString() { InitProxy(); FORWARD_HR(CompatString); }
HRESULT WINAPI Proxy_CompatValue() { InitProxy(); FORWARD_HR(CompatValue); }
HRESULT WINAPI Proxy_DXGID3D10CreateDevice() { InitProxy(); FORWARD_HR(DXGID3D10CreateDevice); }
HRESULT WINAPI Proxy_DXGID3D10CreateLayeredDevice() { InitProxy(); FORWARD_HR(DXGID3D10CreateLayeredDevice); }
HRESULT WINAPI Proxy_DXGID3D10GetLayeredDeviceSize() { InitProxy(); FORWARD_HR(DXGID3D10GetLayeredDeviceSize); }
HRESULT WINAPI Proxy_DXGID3D10RegisterLayers() { InitProxy(); FORWARD_HR(DXGID3D10RegisterLayers); }
HRESULT WINAPI Proxy_DXGIDisableVBlankVirtualization() { InitProxy(); FORWARD_HR(DXGIDisableVBlankVirtualization); }
HRESULT WINAPI Proxy_DXGIDumpJournal() { InitProxy(); FORWARD_HR(DXGIDumpJournal); }
HRESULT WINAPI Proxy_DXGIReportAdapterConfiguration() { InitProxy(); FORWARD_HR(DXGIReportAdapterConfiguration); }
HRESULT WINAPI Proxy_PIXBeginCapture() { InitProxy(); FORWARD_HR(PIXBeginCapture); }
HRESULT WINAPI Proxy_PIXEndCapture() { InitProxy(); FORWARD_HR(PIXEndCapture); }
HRESULT WINAPI Proxy_PIXGetCaptureState() { InitProxy(); FORWARD_HR(PIXGetCaptureState); }
HRESULT WINAPI Proxy_SetAppCompatStringPointer() { InitProxy(); FORWARD_HR(SetAppCompatStringPointer); }
HRESULT WINAPI Proxy_UpdateHMDEmulationStatus() { InitProxy(); FORWARD_HR(UpdateHMDEmulationStatus); }

int WINAPI Proxy_NVSDK_NGX_D3D12_CreateFeature(void* pCmdList, int FeatureId, void* pParameters, void** ppHandle)
{
    InitProxy();
    if (!g_pfnNGXCreateFeature && g_hRealVR)
        g_pfnNGXCreateFeature = (PFN_NVSDK_NGX_D3D12_CreateFeature)GetProcAddress(g_hRealVR, "NVSDK_NGX_D3D12_CreateFeature");
    
    char buf[128];
    sprintf_s(buf, sizeof(buf), "[VR-DLSS5] CreateFeature called: FeatureId=%d", FeatureId);
    LogMsg(buf);

    if (g_pfnNGXCreateFeature)
        return g_pfnNGXCreateFeature(pCmdList, FeatureId, pParameters, ppHandle);
    return 1;
}

} // extern "C"

// ============================================================================
// VR-DLSS 5 HUD & REAL-TIME CONTROLLER (Quest 3 & Pimax Multi-Res)
// ============================================================================

#define HUD_WIDTH  500
#define HUD_HEIGHT 300

struct HUDColor {
    uint8_t r, g, b, a;
};

static inline HUDColor MakeHUDColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    HUDColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}

enum MenuPage {
    MENU_MAIN = 0,
    MENU_RENDERING = 1
};
static MenuPage g_currentMenu = MENU_MAIN;

// Runtime state for OptiScaler Pre-SR Engine
static bool g_varsInitialized = false;

// HUD State & OptiScaler Pre-SR settings
static bool g_hudDirty = true;
static bool g_masterEnable = true;          // [DlssNr] Enabled
static float g_workingScale = 0.75f;        // [DlssNr] WorkingScale (0.25x - 1.50x, step 0.05)
static bool g_runBeforeSR = true;           // [DlssNr] RunBeforeSR (Pre-SR vs Post-SR)
static int g_nrPreset = 2;                  // [DlssNr] Preset (0, 1, 2)
static bool g_residualAcrossRR = true;      // [DlssNr] ResidualAcrossRR (true/false)
static float g_nrIntensity = 1.00f;         // [DlssNr] Intensity (0.00 - 2.00, step 0.05)
static int g_nrStyle = 0;                   // [DlssNr] Style (0 Standard, 1 Natural, 2 Cinematic)

// --- Extended World & Rendering parameters ---
static float g_nrLocalStructure = 1.00f;    // [DlssNr] LocalStructure (0.00 - 2.00, step 0.05)
static float g_nrLocalTone = 0.00f;         // [DlssNr] LocalTone (0.00 - 2.00, step 0.05)
static bool g_nrAutoMask = true;            // [DlssNr] AutoMask (true/false)
static float g_nrSkinStructure = -1.00f;    // [DlssNr] SkinStructure (-1.00 - 2.00, step 0.05; -1 = Auto)
static float g_nrTransferStrength = 1.00f;  // [DlssNr] TransferStrength (0.00 - 2.00, step 0.05)
static float g_nrColourStrength = 1.00f;    // [DlssNr] ColourStrength (0.00 - 1.00, step 0.05)
static int g_nrPasses = 1;                  // [DlssNr] Passes (1, 2, 3)

static int g_hudScale = 1;                  // [VRHUD] HUDScale (0: 1.0x Compact, 1: 1.5x Balanced, 2: 2.0x Comfort)
static int g_activeRow = 0;                 // Current row in active menu page (Main: 0-6, Rendering: 0-8)
static int g_hudPosIndex = 0;               // [VRHUD] HUDPosition (0: Bottom-Center, 1: Top-Center, 2: Top-Right, 3: Top-Left)
static int g_liveHz = 72;                   // measured FPS (HUD badge)
static int g_hmdHz = 72;                    // native HMD refresh (V-Sync budget)
static bool g_frameGuardActive = true;      // [VRHUD] FrameGuard (auto drop scale on VR cliff)
static bool g_frameGuardTriggered = false;
static double g_gpuMsAvg = 0.0;             // SteamVR per-frame GPU ms (smoothed)
static int g_vrOverlayMode = 2;             // [VRHUD] EnableVROverlay (0: Off, 1: Force On, 2: Auto)

// ----------------------------------------------------------------------------
// OpenXR Detection & VR Overlay Safety
// In SteamVR, a process running an OpenXR scene (VrApplication_OpenXRScene)
// cannot simultaneously connect as an OpenVR overlay (VRApplication_Overlay,
// state 2). Attempting to do so causes SteamVR to reject xrCreateSession with
// XR_ERROR_CALL_ORDER_INVALID (-37). When OpenXR is detected, the in-headset
// OpenVR overlay is automatically disabled while keeping the desktop floating
// OSD fully functional.
// ----------------------------------------------------------------------------
static bool IsOpenXRRuntime()
{
    // 1. Check if OpenXR runtime module is already loaded in the process
    if (GetModuleHandleA("openxr_loader.dll") ||
        GetModuleHandleA("openxr-64.dll") ||
        GetModuleHandleA("steamxr.dll") ||
        GetModuleHandleA("steamxr_win64.dll") ||
        GetModuleHandleA("XrApiLayer_steamvr.dll") ||
        GetModuleHandleA("UEVRBackend.dll"))
    {
        return true;
    }

    // 2. Check RealVR.ini in the executable directory
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH))
    {
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash)
        {
            *(lastSlash + 1) = '\0';
            char realVrIni[MAX_PATH];
            strcpy_s(realVrIni, MAX_PATH, exePath);
            strcat_s(realVrIni, MAX_PATH, "RealVR.ini");

            if (GetFileAttributesA(realVrIni) != INVALID_FILE_ATTRIBUTES)
            {
                char hmdName[128] = { 0 };
                GetPrivateProfileStringA("RVR", "LastHMDName", "", hmdName, sizeof(hmdName), realVrIni);
                if (strstr(hmdName, "OpenXR") || strstr(hmdName, "openxr"))
                {
                    return true;
                }

                int prefApi = GetPrivateProfileIntA("RVR", "PreferredAPI2", -1, realVrIni);
                if (prefApi == 2) // 2 = OpenXR in LukeRoss mods
                {
                    return true;
                }
            }
        }
    }

    return false;
}

static bool ShouldEnableVROverlay()
{
    if (g_vrOverlayMode == 0)
    {
        return false;
    }
    static bool s_loggedOnce = false;
    if (!s_loggedOnce)
    {
        s_loggedOnce = true;
        bool isOpenXR = IsOpenXRRuntime();
        char buf[160];
        sprintf_s(buf, sizeof(buf),
            "[OpenVR-Overlay] %s runtime detected: in-headset VR overlay armed (lazy-init on HUD toggle, zero boot collision).",
            isOpenXR ? "OpenXR" : "OpenVR");
        LogMsg(buf);
    }
    return true;
}

static void VrCtlPublish()
{
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

// ----------------------------------------------------------------------------
// Deferred process priority boost.
// Applied from the autonomous watcher thread after the app has settled.
// ----------------------------------------------------------------------------
static bool g_priorityBoosted = false;
static bool g_priorityBoostEnabled = true;

// ----------------------------------------------------------------------------
// High-resolution frame-time diagnostics (reprojection-cliff analysis).
// Sampled every 200 ms from the NGX evaluate counter (per-eye evaluates in
// stereo VR), giving a distribution of ms/evaluate to characterise how far the
// pipeline sits from the 72/90/120 Hz V-Sync budget.
// ----------------------------------------------------------------------------
static double g_ftMinMs = 1e9, g_ftMaxMs = 0.0, g_ftSumMs = 0.0;
static unsigned int g_ftSamples = 0;

// 500 ms Debounce State
static bool g_hasPendingSave = false;
static uint64_t g_lastChangeTick = 0;
static char g_iniPath[MAX_PATH] = ".\\OptiScaler.ini";

// Key & Gamepad repetition
struct KeyTracker {
    bool isDown;
    bool justPressed;
    uint64_t downSince;
    uint64_t lastRepeat;
};
static KeyTracker g_keys[256] = {};

static void InitPaths()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH))
    {
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash)
        {
            *(lastSlash + 1) = '\0';
            char optiIni[MAX_PATH];
            strcpy_s(optiIni, MAX_PATH, exePath);
            strcat_s(optiIni, MAX_PATH, "OptiScaler.ini");

            char reshadeIni[MAX_PATH];
            strcpy_s(reshadeIni, MAX_PATH, exePath);
            strcat_s(reshadeIni, MAX_PATH, "ReShade.ini");

            if (GetFileAttributesA(optiIni) != INVALID_FILE_ATTRIBUTES)
            {
                strcpy_s(g_iniPath, MAX_PATH, optiIni);
            }
            else if (GetFileAttributesA(reshadeIni) != INVALID_FILE_ATTRIBUTES)
            {
                strcpy_s(g_iniPath, MAX_PATH, reshadeIni);
            }
            else
            {
                strcpy_s(g_iniPath, MAX_PATH, optiIni);
            }
        }
    }
}

static void InitVariablesFromAddonOrIni()
{
    if (g_varsInitialized) return;
    g_varsInitialized = true;

    InitPaths();

    bool isOptiScaler = (strstr(g_iniPath, "OptiScaler.ini") != NULL);

    if (isOptiScaler)
    {
        char enabledStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "Enabled", "true", enabledStr, sizeof(enabledStr), g_iniPath);
        g_masterEnable = (_stricmp(enabledStr, "true") == 0 || _stricmp(enabledStr, "1") == 0);

        char scaleStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "WorkingScale", "0.75", scaleStr, sizeof(scaleStr), g_iniPath);
        g_workingScale = (float)atof(scaleStr);
        if (g_workingScale < 0.25f || g_workingScale > 2.0f) g_workingScale = 0.75f;

        char preSrStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "RunBeforeSR", "true", preSrStr, sizeof(preSrStr), g_iniPath);
        g_runBeforeSR = (_stricmp(preSrStr, "true") == 0 || _stricmp(preSrStr, "1") == 0);

        g_nrPreset = GetPrivateProfileIntA("DlssNr", "Preset", 2, g_iniPath);
        if (g_nrPreset < 0 || g_nrPreset > 2) g_nrPreset = 2;

        char rrStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "ResidualAcrossRR", "true", rrStr, sizeof(rrStr), g_iniPath);
        g_residualAcrossRR = (_stricmp(rrStr, "true") == 0 || _stricmp(rrStr, "1") == 0);

        char intStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "Intensity", "1.00", intStr, sizeof(intStr), g_iniPath);
        g_nrIntensity = (float)atof(intStr);
        if (g_nrIntensity < 0.0f || g_nrIntensity > 2.0f) g_nrIntensity = 1.00f;

        g_nrStyle = GetPrivateProfileIntA("DlssNr", "Style", 0, g_iniPath);
        if (g_nrStyle < 0 || g_nrStyle > 2) g_nrStyle = 0;

        char structStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "LocalStructure", "1.00", structStr, sizeof(structStr), g_iniPath);
        if (_stricmp(structStr, "auto") == 0) g_nrLocalStructure = 1.00f;
        else {
            g_nrLocalStructure = (float)atof(structStr);
            if (g_nrLocalStructure < 0.0f || g_nrLocalStructure > 2.0f) g_nrLocalStructure = 1.00f;
        }

        char toneStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "LocalTone", "0.00", toneStr, sizeof(toneStr), g_iniPath);
        if (_stricmp(toneStr, "auto") == 0) g_nrLocalTone = 0.00f;
        else {
            g_nrLocalTone = (float)atof(toneStr);
            if (g_nrLocalTone < 0.0f || g_nrLocalTone > 2.0f) g_nrLocalTone = 0.00f;
        }

        char autoMaskStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "AutoMask", "true", autoMaskStr, sizeof(autoMaskStr), g_iniPath);
        g_nrAutoMask = (_stricmp(autoMaskStr, "true") == 0 || _stricmp(autoMaskStr, "1") == 0 || _stricmp(autoMaskStr, "auto") == 0);

        char skinStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "SkinStructure", "-1.00", skinStr, sizeof(skinStr), g_iniPath);
        if (_stricmp(skinStr, "auto") == 0) g_nrSkinStructure = -1.00f;
        else {
            g_nrSkinStructure = (float)atof(skinStr);
            if (g_nrSkinStructure < -1.0f || g_nrSkinStructure > 2.0f) g_nrSkinStructure = -1.00f;
        }

        char transferStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "TransferStrength", "1.00", transferStr, sizeof(transferStr), g_iniPath);
        if (_stricmp(transferStr, "auto") == 0) g_nrTransferStrength = 1.00f;
        else {
            g_nrTransferStrength = (float)atof(transferStr);
            if (g_nrTransferStrength < 0.0f || g_nrTransferStrength > 2.0f) g_nrTransferStrength = 1.00f;
        }

        char colourStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "ColourStrength", "1.00", colourStr, sizeof(colourStr), g_iniPath);
        if (_stricmp(colourStr, "auto") == 0) g_nrColourStrength = 1.00f;
        else {
            g_nrColourStrength = (float)atof(colourStr);
            if (g_nrColourStrength < 0.0f || g_nrColourStrength > 1.0f) g_nrColourStrength = 1.00f;
        }

        char passesStr[32] = {0};
        GetPrivateProfileStringA("DlssNr", "Passes", "1", passesStr, sizeof(passesStr), g_iniPath);
        if (_stricmp(passesStr, "auto") == 0) g_nrPasses = 1;
        else {
            g_nrPasses = atoi(passesStr);
            if (g_nrPasses < 1 || g_nrPasses > 3) g_nrPasses = 1;
        }

        g_hudScale = GetPrivateProfileIntA("VRHUD", "HUDScale", 1, g_iniPath);
        if (g_hudScale < 0 || g_hudScale > 2) g_hudScale = 1;

        g_hudPosIndex = GetPrivateProfileIntA("VRHUD", "HUDPosition", 0, g_iniPath);
        if (g_hudPosIndex < 0 || g_hudPosIndex > 3) g_hudPosIndex = 0;

        g_frameGuardActive = GetPrivateProfileIntA("VRHUD", "FrameGuard", 1, g_iniPath) != 0;
        g_priorityBoostEnabled = GetPrivateProfileIntA("VRHUD", "PriorityBoost", 1, g_iniPath) != 0;

        char overlayStr[32] = { 0 };
        GetPrivateProfileStringA("VRHUD", "EnableVROverlay", "auto", overlayStr, sizeof(overlayStr), g_iniPath);
        if (_stricmp(overlayStr, "auto") == 0) {
            g_vrOverlayMode = 2; // Auto-detect (disable in OpenXR, enable in OpenVR)
        } else if (_stricmp(overlayStr, "true") == 0 || _stricmp(overlayStr, "1") == 0) {
            g_vrOverlayMode = 1; // Force enabled
        } else if (_stricmp(overlayStr, "false") == 0 || _stricmp(overlayStr, "0") == 0) {
            g_vrOverlayMode = 0; // Disabled
        } else {
            g_vrOverlayMode = 2; // Default auto
        }
    }
    else
    {
        // Legacy ReShade fallback
        int uplift = GetPrivateProfileIntA("RenoDX.DLSS5", "NeuralUplift", 1, g_iniPath);
        g_masterEnable = (uplift != 0);
        g_workingScale = 0.75f;
        g_runBeforeSR = true;
        g_residualAcrossRR = true;
        g_nrPreset = GetPrivateProfileIntA("RenoDX.DLSS5", "NRPreset", 2, g_iniPath);
        if (g_nrPreset < 0 || g_nrPreset > 2) g_nrPreset = 2;
        g_hudScale = GetPrivateProfileIntA("RenoDX.DLSS5", "HUDScale", 1, g_iniPath);
        if (g_hudScale < 0 || g_hudScale > 2) g_hudScale = 1;
        g_hudPosIndex = GetPrivateProfileIntA("RenoDX.DLSS5", "HUDPosition", 0, g_iniPath);
        if (g_hudPosIndex < 0 || g_hudPosIndex > 3) g_hudPosIndex = 0;
    }

    char buf[384];
    sprintf_s(buf, sizeof(buf), 
        "[VR-DLSS5-HUD] Initialized from %s: Enable=%d, WorkingScale=%.2f, RunBeforeSR=%d, Preset=%d, ResidualRR=%d, Intensity=%.2f, Style=%d, Scale=%d, Pos=%d, VROverlayMode=%d (Active=%d)",
        g_iniPath, g_masterEnable ? 1 : 0, g_workingScale, g_runBeforeSR ? 1 : 0, g_nrPreset, g_residualAcrossRR ? 1 : 0,
        g_nrIntensity, g_nrStyle, g_hudScale, g_hudPosIndex, g_vrOverlayMode, ShouldEnableVROverlay() ? 1 : 0);
    LogMsg(buf);

    // Seed the live channel so a mid-session OptiScaler start adopts current values.
    VrCtlPublish();
}

static void CommitSettingsToDisk()
{
    InitPaths();

    bool isOptiScaler = (strstr(g_iniPath, "OptiScaler.ini") != NULL);

    if (isOptiScaler)
    {
        WritePrivateProfileStringA("DlssNr", "Enabled", g_masterEnable ? "true" : "false", g_iniPath);

        char scaleBuf[32];
        sprintf_s(scaleBuf, sizeof(scaleBuf), "%.2f", g_workingScale);
        WritePrivateProfileStringA("DlssNr", "WorkingScale", scaleBuf, g_iniPath);

        WritePrivateProfileStringA("DlssNr", "RunBeforeSR", g_runBeforeSR ? "true" : "false", g_iniPath);

        char presetBuf[32];
        sprintf_s(presetBuf, sizeof(presetBuf), "%d", g_nrPreset);
        WritePrivateProfileStringA("DlssNr", "Preset", presetBuf, g_iniPath);

        WritePrivateProfileStringA("DlssNr", "ResidualAcrossRR", g_residualAcrossRR ? "true" : "false", g_iniPath);

        char intBuf[32];
        sprintf_s(intBuf, sizeof(intBuf), "%.2f", g_nrIntensity);
        WritePrivateProfileStringA("DlssNr", "Intensity", intBuf, g_iniPath);

        char styleBuf[32];
        sprintf_s(styleBuf, sizeof(styleBuf), "%d", g_nrStyle);
        WritePrivateProfileStringA("DlssNr", "Style", styleBuf, g_iniPath);

        char structBuf[32], toneBuf[32], skinBuf[32], transferBuf[32], colBuf[32], passBuf[32];
        sprintf_s(structBuf, sizeof(structBuf), "%.2f", g_nrLocalStructure);
        WritePrivateProfileStringA("DlssNr", "LocalStructure", structBuf, g_iniPath);

        sprintf_s(toneBuf, sizeof(toneBuf), "%.2f", g_nrLocalTone);
        WritePrivateProfileStringA("DlssNr", "LocalTone", toneBuf, g_iniPath);

        WritePrivateProfileStringA("DlssNr", "AutoMask", g_nrAutoMask ? "true" : "false", g_iniPath);

        sprintf_s(skinBuf, sizeof(skinBuf), "%.2f", g_nrSkinStructure);
        WritePrivateProfileStringA("DlssNr", "SkinStructure", skinBuf, g_iniPath);

        sprintf_s(transferBuf, sizeof(transferBuf), "%.2f", g_nrTransferStrength);
        WritePrivateProfileStringA("DlssNr", "TransferStrength", transferBuf, g_iniPath);

        sprintf_s(colBuf, sizeof(colBuf), "%.2f", g_nrColourStrength);
        WritePrivateProfileStringA("DlssNr", "ColourStrength", colBuf, g_iniPath);

        sprintf_s(passBuf, sizeof(passBuf), "%d", g_nrPasses);
        WritePrivateProfileStringA("DlssNr", "Passes", passBuf, g_iniPath);

        char hudScaleBuf[32], hudPosBuf[32], guardBuf[32];
        sprintf_s(hudScaleBuf, sizeof(hudScaleBuf), "%d", g_hudScale);
        sprintf_s(hudPosBuf, sizeof(hudPosBuf), "%d", g_hudPosIndex);
        sprintf_s(guardBuf, sizeof(guardBuf), "%d", g_frameGuardActive ? 1 : 0);
        WritePrivateProfileStringA("VRHUD", "HUDScale", hudScaleBuf, g_iniPath);
        WritePrivateProfileStringA("VRHUD", "HUDPosition", hudPosBuf, g_iniPath);
        WritePrivateProfileStringA("VRHUD", "FrameGuard", guardBuf, g_iniPath);

        char ovrCheck[32] = { 0 };
        GetPrivateProfileStringA("VRHUD", "EnableVROverlay", "", ovrCheck, sizeof(ovrCheck), g_iniPath);
        if (ovrCheck[0] == '\0') {
            const char* ovrVal = (g_vrOverlayMode == 1) ? "true" : ((g_vrOverlayMode == 0) ? "false" : "auto");
            WritePrivateProfileStringA("VRHUD", "EnableVROverlay", ovrVal, g_iniPath);
        }
    }
    else
    {
        char secBuf[512];
        int offset = 0;
        offset += sprintf_s(secBuf + offset, sizeof(secBuf) - offset, "NeuralUplift=%d", g_masterEnable ? 1 : 0) + 1;
        offset += sprintf_s(secBuf + offset, sizeof(secBuf) - offset, "NRPreset=%d", g_nrPreset) + 1;
        offset += sprintf_s(secBuf + offset, sizeof(secBuf) - offset, "HUDScale=%d", g_hudScale) + 1;
        offset += sprintf_s(secBuf + offset, sizeof(secBuf) - offset, "HUDPosition=%d", g_hudPosIndex) + 1;
        secBuf[offset] = '\0';
        WritePrivateProfileSectionA("RenoDX.DLSS5", secBuf, g_iniPath);
    }

    char logBuf[320];
    sprintf_s(logBuf, sizeof(logBuf), 
        "[VR-DLSS5-HUD] Settings committed to %s: Enable=%d, WorkingScale=%.2f, PreSR=%d, Preset=%d, ResidualRR=%d, Intensity=%.2f, Style=%d",
        g_iniPath, g_masterEnable ? 1 : 0, g_workingScale, g_runBeforeSR ? 1 : 0, g_nrPreset, g_residualAcrossRR ? 1 : 0,
        g_nrIntensity, g_nrStyle);
    LogMsg(logBuf);

    // Push to the running OptiScaler instance (its Config is only read from the
    // INI at startup). This is what makes the HUD rows take effect live.
    VrCtlPublish();
}

// ----------------------------------------------------------------------------
// Modern High-DPI GDI Vector Rasterizer (Anti-Aliased Segoe UI, Zero-Crash)
// ----------------------------------------------------------------------------
static uint32_t s_hudPixelsOSD[HUD_HEIGHT * HUD_WIDTH];   // Premultiplied BGRA for Desktop Layered Window
static uint8_t  s_hudPixelsVR[HUD_HEIGHT * HUD_WIDTH * 4]; // RGBA for OpenVR SetOverlayRaw
static HDC      g_hGdiMemDC = NULL;
static HBITMAP  g_hGdiBmp = NULL;
static uint32_t* g_pGdiBits = NULL;

static void InitGDIRasterizer()
{
    if (g_hGdiMemDC) return;
    HDC hdcScreen = GetDC(NULL);
    g_hGdiMemDC = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = HUD_WIDTH;
    bmi.bmiHeader.biHeight = -HUD_HEIGHT; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_hGdiBmp = CreateDIBSection(g_hGdiMemDC, &bmi, DIB_RGB_COLORS, (void**)&g_pGdiBits, NULL, 0);
    SelectObject(g_hGdiMemDC, g_hGdiBmp);
    ReleaseDC(NULL, hdcScreen);
}

static void DrawMiniSlider(HDC hdc, int x, int y, int w, int h, float norm01, COLORREF fillColor, COLORREF thumbColor)
{
    if (norm01 < 0.0f) norm01 = 0.0f;
    if (norm01 > 1.0f) norm01 = 1.0f;

    HBRUSH hTrackBg = CreateSolidBrush(RGB(22, 32, 48));
    HPEN hTrackPen = CreatePen(PS_SOLID, 1, RGB(45, 68, 98));
    HBRUSH hOldB = (HBRUSH)SelectObject(hdc, hTrackBg);
    HPEN hOldP = (HPEN)SelectObject(hdc, hTrackPen);
    RoundRect(hdc, x, y, x + w, y + h, 4, 4);

    int fillW = (int)(w * norm01);
    if (fillW > 0) {
        HBRUSH hFill = CreateSolidBrush(fillColor);
        HPEN hFillPen = CreatePen(PS_NULL, 0, 0);
        SelectObject(hdc, hFill);
        SelectObject(hdc, hFillPen);
        RoundRect(hdc, x, y, x + fillW, y + h, 4, 4);
        DeleteObject(hFill);
        DeleteObject(hFillPen);
    }

    int thumbX = x + fillW;
    if (thumbX > x + w) thumbX = x + w;
    HBRUSH hThumb = CreateSolidBrush(thumbColor);
    HPEN hThumbPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    SelectObject(hdc, hThumb);
    SelectObject(hdc, hThumbPen);
    RoundRect(hdc, thumbX - 3, y - 2, thumbX + 3, y + h + 2, 4, 4);
    DeleteObject(hThumb);
    DeleteObject(hThumbPen);

    SelectObject(hdc, hOldB);
    SelectObject(hdc, hOldP);
    DeleteObject(hTrackBg);
    DeleteObject(hTrackPen);
}

static void RenderModernHUD(HDC hdc, uint32_t* pGdiBits, bool masterEnable, float workingScale, bool runBeforeSR, 
                            int preset, bool residualAcrossRR, float nrIntensity, int nrStyle,
                            int posIdx, int scaleMode, int activeRow, int liveHz, bool frameGuardTriggered)
{
    if (!hdc || !pGdiBits) return;

    // 1. Clear GDI buffer to 0
    memset(pGdiBits, 0, HUD_WIDTH * HUD_HEIGHT * 4);

    // 2. Setup GDI state
    SetBkMode(hdc, TRANSPARENT);

    HFONT hFontTitle = CreateFontA(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 
                                  DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hFontMain  = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 
                                  DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hFontValue = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 
                                  DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hFontBadge = CreateFontA(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 
                                  DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hFontHelp  = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 
                                  DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    // 3. Draw Background Card (Translucent slate with rounded corners)
    HBRUSH hBrushCard = CreateSolidBrush(RGB(14, 18, 26));
    HPEN hPenBorder = CreatePen(PS_SOLID, 1, RGB(0, 180, 235)); // Cyan neon outline
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrushCard);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPenBorder);
    RoundRect(hdc, 1, 1, HUD_WIDTH - 1, HUD_HEIGHT - 1, 14, 14);

    // 4. Header Bar: Title
    SelectObject(hdc, hFontTitle);
    SetTextColor(hdc, RGB(0, 220, 255));
    if (g_currentMenu == MENU_MAIN) {
        TextOutA(hdc, 16, 8, "DLSS 5 <> VR (OptiScaler Pre-SR)", 32);
    } else {
        TextOutA(hdc, 16, 8, "DLSS 5 <> Rendu & Monde (Sous-Menu)", 35);
    }

    // Glowing Neon Dot
    HBRUSH hBrushDot = CreateSolidBrush(RGB(0, 255, 140));
    HPEN hPenDot = CreatePen(PS_SOLID, 1, RGB(0, 255, 140));
    SelectObject(hdc, hBrushDot);
    SelectObject(hdc, hPenDot);
    Ellipse(hdc, HUD_WIDTH - 150, 12, HUD_WIDTH - 140, 22);
    DeleteObject(hBrushDot);
    DeleteObject(hPenDot);

    // Hz & Status Pill Badge
    SelectObject(hdc, hFontBadge);
    char badgeBuf[32];
    sprintf_s(badgeBuf, sizeof(badgeBuf), "%d FPS  |  %s", liveHz, masterEnable ? "ACTIVE" : "BYPASS");
    COLORREF badgeBg = masterEnable ? RGB(16, 75, 42) : RGB(100, 24, 24);
    COLORREF badgeBorder = masterEnable ? RGB(45, 200, 100) : RGB(220, 60, 60);
    COLORREF badgeText = masterEnable ? RGB(220, 255, 230) : RGB(255, 220, 220);

    HBRUSH hBrushBadge = CreateSolidBrush(badgeBg);
    HPEN hPenBadge = CreatePen(PS_SOLID, 1, badgeBorder);
    SelectObject(hdc, hBrushBadge);
    SelectObject(hdc, hPenBadge);
    RoundRect(hdc, HUD_WIDTH - 134, 6, HUD_WIDTH - 12, 28, 8, 8);
    DeleteObject(hBrushBadge);
    DeleteObject(hPenBadge);

    SetTextColor(hdc, badgeText);
    RECT rcBadge = { HUD_WIDTH - 134, 6, HUD_WIDTH - 12, 28 };
    DrawTextA(hdc, badgeBuf, -1, &rcBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Header Separator Line
    HPEN hPenSep = CreatePen(PS_SOLID, 1, RGB(32, 54, 82));
    SelectObject(hdc, hPenSep);
    MoveToEx(hdc, 14, 34, NULL);
    LineTo(hdc, HUD_WIDTH - 14, 34);
    DeleteObject(hPenSep);

    // 5. Menu Rows
    if (g_currentMenu == MENU_MAIN)
    {
        int rowY[7] = { 40, 68, 96, 124, 152, 180, 208 };
        for (int i = 0; i < 7; i++)
        {
            int y = rowY[i];
            bool isActive = (i == activeRow);

            if (isActive) {
                HBRUSH hBrushRow = CreateSolidBrush(RGB(24, 46, 76));
                HPEN hPenRow = CreatePen(PS_SOLID, 1, RGB(0, 170, 255));
                SelectObject(hdc, hBrushRow);
                SelectObject(hdc, hPenRow);
                RoundRect(hdc, 10, y, HUD_WIDTH - 10, y + 24, 6, 6);
                DeleteObject(hBrushRow);
                DeleteObject(hPenRow);

                SelectObject(hdc, hFontMain);
                SetTextColor(hdc, RGB(255, 230, 0));
                TextOutA(hdc, 16, y + 3, ">", 1);
            }

            SelectObject(hdc, hFontMain);
            COLORREF labelColor = isActive ? RGB(255, 255, 255) : RGB(170, 185, 205);
            SetTextColor(hdc, labelColor);

            if (i == 0) { // Neural Engine Toggle
                TextOutA(hdc, 30, y + 3, "Neural Engine", 13);
                SelectObject(hdc, hFontBadge);
                const char* txt = masterEnable ? "[ ACTIVE ]" : "[ BYPASS ]";
                COLORREF cBg = masterEnable ? RGB(15, 120, 55) : RGB(130, 28, 28);
                COLORREF cBd = masterEnable ? RGB(60, 240, 120) : RGB(250, 70, 70);
                HBRUSH hb = CreateSolidBrush(cBg);
                HPEN hp = CreatePen(PS_SOLID, 1, cBd);
                SelectObject(hdc, hb);
                SelectObject(hdc, hp);
                RoundRect(hdc, 200, y + 2, 290, y + 22, 6, 6);
                DeleteObject(hb);
                DeleteObject(hp);
                SetTextColor(hdc, RGB(255, 255, 255));
                RECT rc = { 200, y + 2, 290, y + 22 };
                DrawTextA(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            else if (i == 1) { // Rendu DLSS 5 Submenu Entry
                TextOutA(hdc, 30, y + 3, "Rendu DLSS 5", 12);
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(255, 240, 120) : RGB(200, 210, 180));
                TextOutA(hdc, 160, y + 4, "Monde, Décor & Couleurs", 23);

                SelectObject(hdc, hFontBadge);
                COLORREF cBg = isActive ? RGB(0, 100, 160) : RGB(20, 45, 70);
                COLORREF cBd = isActive ? RGB(0, 230, 255) : RGB(50, 95, 140);
                HBRUSH hb = CreateSolidBrush(cBg);
                HPEN hp = CreatePen(PS_SOLID, 1, cBd);
                SelectObject(hdc, hb);
                SelectObject(hdc, hp);
                RoundRect(hdc, 345, y + 2, 480, y + 22, 6, 6);
                DeleteObject(hb);
                DeleteObject(hp);
                SetTextColor(hdc, isActive ? RGB(255, 255, 255) : RGB(160, 210, 255));
                RECT rc = { 345, y + 2, 480, y + 22 };
                DrawTextA(hdc, "[ D-Pad > : Ouvrir ]", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            else if (i == 2) { // VR WorkingScale (Full range 0.25x - 1.50x, step 0.05)
                TextOutA(hdc, 30, y + 3, "VR WorkingScale", 15);
                char scaleBuf[48];
                sprintf_s(scaleBuf, sizeof(scaleBuf), "%.2fx (%d%%)", workingScale, (int)(workingScale * 100.0f + 0.5f));
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(0, 240, 255) : RGB(140, 200, 230));
                TextOutA(hdc, 160, y + 4, scaleBuf, (int)strlen(scaleBuf));

                if (frameGuardTriggered) {
                    SelectObject(hdc, hFontBadge);
                    SetTextColor(hdc, RGB(255, 200, 50));
                    TextOutA(hdc, 265, y + 4, "[GUARD]", 7);
                }
                float norm = (workingScale - 0.25f) / 1.25f;
                DrawMiniSlider(hdc, 335, y + 8, 145, 8, norm, RGB(0, 160, 225), RGB(0, 255, 255));
            }
            else if (i == 3) { // Placement Mode
                TextOutA(hdc, 30, y + 3, "Placement Mode", 14);
                SelectObject(hdc, hFontBadge);
                const char* txt = runBeforeSR ? "Pre-SR [Render Res - Fast]" : "Post-SR [Output 4K - Heavy]";
                COLORREF cBg = runBeforeSR ? RGB(12, 70, 60) : RGB(120, 40, 20);
                COLORREF cBd = runBeforeSR ? RGB(40, 220, 180) : RGB(240, 90, 50);
                HBRUSH hb = CreateSolidBrush(cBg);
                HPEN hp = CreatePen(PS_SOLID, 1, cBd);
                SelectObject(hdc, hb);
                SelectObject(hdc, hp);
                RoundRect(hdc, 160, y + 2, 480, y + 22, 6, 6);
                DeleteObject(hb);
                DeleteObject(hp);
                SetTextColor(hdc, RGB(255, 255, 255));
                RECT rc = { 160, y + 2, 480, y + 22 };
                DrawTextA(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            else if (i == 4) { // AI Model Preset
                TextOutA(hdc, 30, y + 3, "AI Model Preset", 15);
                const char* presetNames[3] = { 
                    "Preset 0  [DLSS-D Neural RR]", 
                    "Preset 1  [Ultra Quality]", 
                    "Preset 2  [Performance - VR]" 
                };
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(255, 240, 120) : RGB(210, 200, 160));
                TextOutA(hdc, 160, y + 4, presetNames[preset % 3], (int)strlen(presetNames[preset % 3]));
            }
            else if (i == 5) { // DLSS5 Style
                TextOutA(hdc, 30, y + 3, "DLSS5 Style", 11);
                static const char* styleNames[3] = { "Standard", "Natural", "Cinematic" };
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(255, 240, 120) : RGB(210, 200, 160));
                const char* styleTxt = styleNames[nrStyle % 3];
                TextOutA(hdc, 160, y + 4, styleTxt, (int)strlen(styleTxt));
            }
            else if (i == 6) { // VR HUD Display
                TextOutA(hdc, 30, y + 3, "VR HUD Display", 14);
                static const char* posNames[4] = { "Bottom-Center", "Top-Center", "Top-Right", "Top-Left" };
                static const char* scaleNames[3] = { "1.0x (Pimax)", "1.5x (Q3)", "2.0x (Large)" };
                char dispBuf[72];
                sprintf_s(dispBuf, sizeof(dispBuf), "Pos: %s  |  Scale: %s",
                          posNames[posIdx % 4], scaleNames[scaleMode % 3]);
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(255, 220, 100) : RGB(210, 200, 150));
                TextOutA(hdc, 160, y + 4, dispBuf, (int)strlen(dispBuf));
            }
        }

        // Footer for Main Menu
        HPEN hPenFoot = CreatePen(PS_SOLID, 1, RGB(30, 50, 75));
        SelectObject(hdc, hPenFoot);
        MoveToEx(hdc, 14, 266, NULL);
        LineTo(hdc, HUD_WIDTH - 14, 266);
        DeleteObject(hPenFoot);

        SelectObject(hdc, hFontHelp);
        SetTextColor(hdc, RGB(120, 160, 200));
        RECT rcHelp = { 10, 271, HUD_WIDTH - 10, HUD_HEIGHT - 3 };
        DrawTextA(hdc, "D-Pad: Naviguer / Ajuster  |  D-Pad > sur Rendu: Sous-Menu  |  F6: Fermer", -1, &rcHelp, DT_CENTER | DT_SINGLELINE);
    }
    else // MENU_RENDERING
    {
        int subRowY[9] = { 38, 62, 86, 110, 134, 158, 182, 206, 230 };
        for (int i = 0; i < 9; i++)
        {
            int y = subRowY[i];
            bool isActive = (i == activeRow);

            if (isActive) {
                HBRUSH hBrushRow = CreateSolidBrush(RGB(24, 46, 76));
                HPEN hPenRow = CreatePen(PS_SOLID, 1, RGB(0, 170, 255));
                SelectObject(hdc, hBrushRow);
                SelectObject(hdc, hPenRow);
                RoundRect(hdc, 10, y, HUD_WIDTH - 10, y + 22, 6, 6);
                DeleteObject(hBrushRow);
                DeleteObject(hPenRow);

                SelectObject(hdc, hFontMain);
                SetTextColor(hdc, RGB(255, 230, 0));
                TextOutA(hdc, 16, y + 2, ">", 1);
            }

            SelectObject(hdc, hFontMain);
            COLORREF labelColor = isActive ? RGB(255, 255, 255) : RGB(170, 185, 205);
            SetTextColor(hdc, labelColor);

            if (i == 0) { // BACK BUTTON AT TOP
                COLORREF bBg = isActive ? RGB(18, 70, 115) : RGB(15, 30, 48);
                COLORREF bBd = isActive ? RGB(0, 230, 255) : RGB(40, 85, 130);
                HBRUSH hb = CreateSolidBrush(bBg);
                HPEN hp = CreatePen(PS_SOLID, 1, bBd);
                SelectObject(hdc, hb);
                SelectObject(hdc, hp);
                RoundRect(hdc, 28, y + 1, HUD_WIDTH - 28, y + 21, 6, 6);
                DeleteObject(hb);
                DeleteObject(hp);

                SelectObject(hdc, hFontBadge);
                SetTextColor(hdc, isActive ? RGB(255, 235, 120) : RGB(140, 210, 255));
                RECT rcBack = { 28, y + 1, HUD_WIDTH - 28, y + 21 };
                DrawTextA(hdc, "[ < RETOUR AU MENU PRINCIPAL ]", -1, &rcBack, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            else if (i == 1) { // Intensité DLSS5 (0.00 - 2.00, step 0.05)
                TextOutA(hdc, 30, y + 2, "Intensité DLSS5", 15);
                char valBuf[32];
                sprintf_s(valBuf, sizeof(valBuf), "%.2fx", nrIntensity);
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(255, 240, 120) : RGB(210, 200, 160));
                TextOutA(hdc, 170, y + 2, valBuf, (int)strlen(valBuf));
                DrawMiniSlider(hdc, 315, y + 6, 165, 8, nrIntensity / 2.0f, RGB(230, 170, 0), RGB(255, 220, 0));
            }
            else if (i == 2) { // Structure Décor (LocalStructure 0.00 - 2.00, step 0.05)
                TextOutA(hdc, 30, y + 2, "Structure Décor", 15);
                char valBuf[32];
                sprintf_s(valBuf, sizeof(valBuf), "%.2fx", g_nrLocalStructure);
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(80, 255, 180) : RGB(160, 220, 180));
                TextOutA(hdc, 170, y + 2, valBuf, (int)strlen(valBuf));
                DrawMiniSlider(hdc, 315, y + 6, 165, 8, g_nrLocalStructure / 2.0f, RGB(0, 200, 140), RGB(80, 255, 180));
            }
            else if (i == 3) { // Tonalité Ombres (LocalTone 0.00 - 2.00, step 0.05)
                TextOutA(hdc, 30, y + 2, "Tonalité Ombres", 15);
                char valBuf[32];
                sprintf_s(valBuf, sizeof(valBuf), "%.2fx", g_nrLocalTone);
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(220, 150, 255) : RGB(180, 160, 220));
                TextOutA(hdc, 170, y + 2, valBuf, (int)strlen(valBuf));
                DrawMiniSlider(hdc, 315, y + 6, 165, 8, g_nrLocalTone / 2.0f, RGB(180, 100, 240), RGB(220, 150, 255));
            }
            else if (i == 4) { // Masque Auto (AutoMask)
                TextOutA(hdc, 30, y + 2, "Masque Auto", 11);
                SelectObject(hdc, hFontBadge);
                const char* txt = g_nrAutoMask ? "[ ON - Focus Visages ]" : "[ OFF - Décor Homogène ]";
                COLORREF cBg = g_nrAutoMask ? RGB(16, 60, 95) : RGB(95, 60, 16);
                COLORREF cBd = g_nrAutoMask ? RGB(0, 180, 255) : RGB(255, 170, 0);
                HBRUSH hb = CreateSolidBrush(cBg);
                HPEN hp = CreatePen(PS_SOLID, 1, cBd);
                SelectObject(hdc, hb);
                SelectObject(hdc, hp);
                RoundRect(hdc, 170, y + 1, 480, y + 21, 6, 6);
                DeleteObject(hb);
                DeleteObject(hp);
                SetTextColor(hdc, g_nrAutoMask ? RGB(180, 230, 255) : RGB(255, 220, 160));
                RECT rc = { 170, y + 1, 480, y + 21 };
                DrawTextA(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            else if (i == 5) { // Structure Peau (SkinStructure -1.00 à 2.00)
                TextOutA(hdc, 30, y + 2, "Structure Peau", 14);
                char valBuf[32];
                if (g_nrSkinStructure < -0.99f) sprintf_s(valBuf, sizeof(valBuf), "[Auto/Décor]");
                else if (fabs(g_nrSkinStructure) < 0.01f) sprintf_s(valBuf, sizeof(valBuf), "0.00 [Adouci]");
                else sprintf_s(valBuf, sizeof(valBuf), "%.2fx", g_nrSkinStructure);
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(255, 180, 150) : RGB(220, 170, 160));
                TextOutA(hdc, 170, y + 2, valBuf, (int)strlen(valBuf));
                float norm = (g_nrSkinStructure + 1.0f) / 3.0f;
                DrawMiniSlider(hdc, 315, y + 6, 165, 8, norm, RGB(240, 120, 90), RGB(255, 180, 150));
            }
            else if (i == 6) { // Passes Neuronales (1, 2, 3)
                TextOutA(hdc, 30, y + 2, "Passes IA", 9);
                const char* passNames[3] = { "1 [Simple Pass]", "2 [Double IA - Dense]", "3 [Triple IA - Hyper]" };
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(255, 240, 120) : RGB(210, 200, 160));
                TextOutA(hdc, 170, y + 2, passNames[(g_nrPasses - 1) % 3], (int)strlen(passNames[(g_nrPasses - 1) % 3]));
            }
            else if (i == 7) { // Force Transfert IA (0.00 - 2.00)
                TextOutA(hdc, 30, y + 2, "Transfert IA", 12);
                char valBuf[32];
                sprintf_s(valBuf, sizeof(valBuf), "%.2fx", g_nrTransferStrength);
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(120, 220, 255) : RGB(140, 190, 220));
                TextOutA(hdc, 170, y + 2, valBuf, (int)strlen(valBuf));
                DrawMiniSlider(hdc, 315, y + 6, 165, 8, g_nrTransferStrength / 2.0f, RGB(0, 160, 240), RGB(100, 220, 255));
            }
            else if (i == 8) { // Force Couleurs IA (0.00 - 1.00)
                TextOutA(hdc, 30, y + 2, "Couleurs IA", 11);
                char valBuf[32];
                sprintf_s(valBuf, sizeof(valBuf), "%.2fx (%d%%)", g_nrColourStrength, (int)(g_nrColourStrength * 100.0f + 0.5f));
                SelectObject(hdc, hFontValue);
                SetTextColor(hdc, isActive ? RGB(255, 140, 200) : RGB(220, 150, 180));
                TextOutA(hdc, 170, y + 2, valBuf, (int)strlen(valBuf));
                DrawMiniSlider(hdc, 315, y + 6, 165, 8, g_nrColourStrength / 1.0f, RGB(240, 80, 170), RGB(255, 140, 210));
            }
        }

        // Footer for Submenu
        HPEN hPenFoot = CreatePen(PS_SOLID, 1, RGB(30, 50, 75));
        SelectObject(hdc, hPenFoot);
        MoveToEx(hdc, 14, 266, NULL);
        LineTo(hdc, HUD_WIDTH - 14, 266);
        DeleteObject(hPenFoot);

        SelectObject(hdc, hFontHelp);
        SetTextColor(hdc, RGB(120, 160, 200));
        RECT rcHelp = { 10, 271, HUD_WIDTH - 10, HUD_HEIGHT - 3 };
        DrawTextA(hdc, "D-Pad: Régler Slider  |  D-Pad < sur Retour: Quitter  |  A: Défaut  |  F6: Fermer", -1, &rcHelp, DT_CENTER | DT_SINGLELINE);
    }

    // Cleanup GDI objects
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrushCard);
    DeleteObject(hPenBorder);
    DeleteObject(hFontTitle);
    DeleteObject(hFontMain);
    DeleteObject(hFontValue);
    DeleteObject(hFontBadge);
    DeleteObject(hFontHelp);

    // 7. Fast Alpha channel post-processing for both Desktop (OSD) and VR (OpenVR)
    for (int y = 0; y < HUD_HEIGHT; y++) {
        int rowIdx = y * HUD_WIDTH;
        for (int x = 0; x < HUD_WIDTH; x++) {
            int idx = rowIdx + x;
            uint32_t px = pGdiBits[idx];
            uint8_t b = (uint8_t)(px & 0xFF);
            uint8_t g = (uint8_t)((px >> 8) & 0xFF);
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);

            uint32_t a = 0;
            uint32_t pr, pg, pb;
            if (r | g | b) {
                if (r <= 20 && g <= 24 && b <= 32) {
                    a = 230; // 90% translucent dark background
                    pr = (r * 230) >> 8;
                    pg = (g * 230) >> 8;
                    pb = (b * 230) >> 8;
                } else {
                    a = 255; // 100% solid for text, neon borders, sliders, badges
                    pr = r;
                    pg = g;
                    pb = b;
                }
            } else {
                pr = 0; pg = 0; pb = 0;
            }

            // Premultiplied BGRA for Windows UpdateLayeredWindow
            s_hudPixelsOSD[idx] = (a << 24) | (pr << 16) | (pg << 8) | pb;

            // Straight RGBA for OpenVR SetOverlayRaw (direct 32-bit dword write)
            *(uint32_t*)&s_hudPixelsVR[idx * 4] = (a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }
    }
}


// ----------------------------------------------------------------------------
// Dynamic OpenVR API Loader (Zero Static Dependency, Zero Loader Lock Deadlock)
// ----------------------------------------------------------------------------
typedef uint32_t (VR_CALLTYPE *PFN_VR_InitInternal2)(vr::EVRInitError *peError, vr::EVRApplicationType eApplicationType, const char *pStartupInfo);
typedef void (VR_CALLTYPE *PFN_VR_ShutdownInternal)();
typedef void* (VR_CALLTYPE *PFN_VR_GetGenericInterface)(const char *pchInterfaceVersion, vr::EVRInitError *peError);
typedef bool (VR_CALLTYPE *PFN_VR_IsInterfaceVersionValid)(const char *pchInterfaceVersion);
typedef const char* (VR_CALLTYPE *PFN_VR_GetVRInitErrorAsEnglishDescription)(vr::EVRInitError error);
typedef uint32_t (VR_CALLTYPE *PFN_VR_GetInitToken)();

static HMODULE g_hOpenVRDll = NULL;
static PFN_VR_InitInternal2 s_pfnVR_InitInternal2 = NULL;
static PFN_VR_ShutdownInternal s_pfnVR_ShutdownInternal = NULL;
static PFN_VR_GetGenericInterface s_pfnVR_GetGenericInterface = NULL;
static PFN_VR_IsInterfaceVersionValid s_pfnVR_IsInterfaceVersionValid = NULL;
static PFN_VR_GetVRInitErrorAsEnglishDescription s_pfnVR_GetVRInitErrorAsEnglishDescription = NULL;
static PFN_VR_GetInitToken s_pfnVR_GetInitToken = NULL;

static bool LoadOpenVRAPI()
{
    if (g_hOpenVRDll) return true;

    // 1. Essayer dans le dossier du jeu en cours
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH)) {
        char* slash = strrchr(path, '\\');
        if (slash) {
            *(slash + 1) = '\0';
            strcat_s(path, MAX_PATH, "openvr_api.dll");
            g_hOpenVRDll = LoadLibraryA(path);
        }
    }
    // 2. Repli standard
    if (!g_hOpenVRDll) {
        g_hOpenVRDll = LoadLibraryA("openvr_api.dll");
    }
    if (!g_hOpenVRDll) {
        LogMsg("[OpenVR-Dynamic] WARNING: openvr_api.dll could not be loaded");
        return false;
    }

    s_pfnVR_InitInternal2 = (PFN_VR_InitInternal2)GetProcAddress(g_hOpenVRDll, "VR_InitInternal2");
    s_pfnVR_ShutdownInternal = (PFN_VR_ShutdownInternal)GetProcAddress(g_hOpenVRDll, "VR_ShutdownInternal");
    s_pfnVR_GetGenericInterface = (PFN_VR_GetGenericInterface)GetProcAddress(g_hOpenVRDll, "VR_GetGenericInterface");
    s_pfnVR_IsInterfaceVersionValid = (PFN_VR_IsInterfaceVersionValid)GetProcAddress(g_hOpenVRDll, "VR_IsInterfaceVersionValid");
    s_pfnVR_GetVRInitErrorAsEnglishDescription = (PFN_VR_GetVRInitErrorAsEnglishDescription)GetProcAddress(g_hOpenVRDll, "VR_GetVRInitErrorAsEnglishDescription");
    s_pfnVR_GetInitToken = (PFN_VR_GetInitToken)GetProcAddress(g_hOpenVRDll, "VR_GetInitToken");

    bool ok = (s_pfnVR_InitInternal2 && s_pfnVR_GetGenericInterface && s_pfnVR_IsInterfaceVersionValid);
    if (ok) {
        LogMsg("[OpenVR-Dynamic] openvr_api.dll dynamically resolved successfully!");
    } else {
        LogMsg("[OpenVR-Dynamic] ERROR: Failed to resolve OpenVR core entry points");
    }
    return ok;
}

namespace vr {
uint32_t VR_CALLTYPE VR_InitInternal2(EVRInitError *peError, EVRApplicationType eApplicationType, const char *pStartupInfo)
{
    if (!LoadOpenVRAPI() || !s_pfnVR_InitInternal2) {
        if (peError) *peError = VRInitError_Init_FileNotFound;
        return 0;
    }
    return s_pfnVR_InitInternal2(peError, eApplicationType, pStartupInfo);
}

void VR_CALLTYPE VR_ShutdownInternal()
{
    if (s_pfnVR_ShutdownInternal) s_pfnVR_ShutdownInternal();
}

void* VR_CALLTYPE VR_GetGenericInterface(const char *pchInterfaceVersion, EVRInitError *peError)
{
    if (!LoadOpenVRAPI() || !s_pfnVR_GetGenericInterface) {
        if (peError) *peError = VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    return s_pfnVR_GetGenericInterface(pchInterfaceVersion, peError);
}

bool VR_CALLTYPE VR_IsInterfaceVersionValid(const char *pchInterfaceVersion)
{
    if (!LoadOpenVRAPI() || !s_pfnVR_IsInterfaceVersionValid) return false;
    return s_pfnVR_IsInterfaceVersionValid(pchInterfaceVersion);
}

const char* VR_CALLTYPE VR_GetVRInitErrorAsEnglishDescription(EVRInitError error)
{
    if (s_pfnVR_GetVRInitErrorAsEnglishDescription) return s_pfnVR_GetVRInitErrorAsEnglishDescription(error);
    return "OpenVR error";
}

uint32_t VR_CALLTYPE VR_GetInitToken()
{
    if (s_pfnVR_GetInitToken) return s_pfnVR_GetInitToken();
    return 0;
}
}

// ----------------------------------------------------------------------------
// OpenVR SteamVR Native Compositor Overlay (fpsVR Architecture)
// Zero-Crash, 100% Decoupled from Game Engine & D3D12 Pipeline
// ----------------------------------------------------------------------------
static vr::IVROverlay* g_pVROverlay = NULL;
static vr::IVRSystem* g_pVRSystem = NULL;
static vr::VROverlayHandle_t g_hVROverlay = vr::k_ulOverlayHandleInvalid;
static bool g_openvrInitialized = false;
static uint64_t g_lastOpenVRInitAttempt = 0;
static int g_overlayFailCount = 0;
static uint64_t g_overlayRetryAfterTick = 0;

static void ApplyOverlayTransformAndScale()
{
    if (!g_pVROverlay || g_hVROverlay == vr::k_ulOverlayHandleInvalid) return;

    // Finer, more compact physical size in VR for high-DPI Pimax / Quest 3
    float widthInMeters = 0.22f;
    if (g_hudScale == 0) widthInMeters = 0.22f;      // 1.0x Compact / Pimax
    else if (g_hudScale == 1) widthInMeters = 0.28f; // 1.5x Balanced
    else if (g_hudScale == 2) widthInMeters = 0.36f; // 2.0x Comfort
    g_pVROverlay->SetOverlayWidthInMeters(g_hVROverlay, widthInMeters);

    float posX = 0.0f;
    float posY = -0.22f; // Sweet spot bas (fpsVR / dashboard)
    float posZ = -0.75f; // 75 cm distance

    switch (g_hudPosIndex % 4) {
    case 0: // Bottom-Center
        posX = 0.0f; posY = -0.22f; posZ = -0.75f;
        break;
    case 1: // Top-Center
        posX = 0.0f; posY = +0.20f; posZ = -0.75f;
        break;
    case 2: // Top-Right
        posX = +0.26f; posY = +0.16f; posZ = -0.75f;
        break;
    case 3: // Top-Left
        posX = -0.26f; posY = +0.16f; posZ = -0.75f;
        break;
    }

    vr::HmdMatrix34_t mat = {};
    mat.m[0][0] = 1.0f;
    mat.m[1][1] = 1.0f;
    mat.m[2][2] = 1.0f;
    mat.m[0][3] = posX;
    mat.m[1][3] = posY;
    mat.m[2][3] = posZ;

    g_pVROverlay->SetOverlayTransformTrackedDeviceRelative(g_hVROverlay, vr::k_unTrackedDeviceIndex_Hmd, &mat);
}

static bool EnsureOpenVROverlay()
{
    if (!ShouldEnableVROverlay()) {
        return false;
    }

    // STRICT LAZY INITIALIZATION:
    // Only connect if the user has requested to open the HUD!
    // NEVER connect in the background during boot or normal play.
    if (!g_hudVisible) {
        return false;
    }

    // Boot grace period: wait at least 15 seconds after launch to ensure
    // the game's VR runtime (OpenXR or OpenVR) has fully initialized its session.
    if (g_bootTick == 0) g_bootTick = GetTickCount64();
    uint64_t elapsedSinceBoot = GetTickCount64() - g_bootTick;
    if (elapsedSinceBoot < 15000) {
        static bool s_loggedGrace = false;
        if (!s_loggedGrace) {
            s_loggedGrace = true;
            char gbuf[128];
            sprintf_s(gbuf, sizeof(gbuf), "[OpenVR-Overlay] Boot grace period active (%llu ms / 15000 ms), postponing overlay connect...", elapsedSinceBoot);
            LogMsg(gbuf);
        }
        return false;
    }

    // If already connected and overlay handle is valid, we are ready!
    if (g_openvrInitialized && g_pVROverlay && g_hVROverlay != vr::k_ulOverlayHandleInvalid) {
        return true;
    }

    if (!g_openvrInitialized) {
        uint64_t now = GetTickCount64();
        if (now - g_lastOpenVRInitAttempt < 2000) {
            return false;
        }
        g_lastOpenVRInitAttempt = now;

        LogMsg("[OpenVR-Overlay] Lazy-init: connecting to SteamVR (VRApplication_Overlay)...");

        vr::EVRInitError err = vr::VRInitError_None;
        vr::IVRSystem* pSys = vr::VR_Init(&err, vr::VRApplication_Overlay);
        if (err != vr::VRInitError_None || !pSys) {
            char buf[128];
            sprintf_s(buf, sizeof(buf), "[OpenVR-Overlay] SteamVR init failed (code %d: %s)", 
                err, vr::VR_GetVRInitErrorAsEnglishDescription(err));
            LogMsg(buf);
            return false;
        }
        g_openvrInitialized = true;
        g_pVRSystem = pSys;
        LogMsg("[OpenVR-Overlay] Successfully connected to SteamVR Compositor (VRApplication_Overlay)");

        // Query native HMD refresh rate from SteamVR
        vr::ETrackedPropertyError propErr = vr::TrackedProp_Success;
        float freq = pSys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &propErr);
        if (propErr == vr::TrackedProp_Success && freq >= 60.0f && freq <= 240.0f) {
            g_hmdHz = (int)(freq + 0.5f);
            g_liveHz = g_hmdHz;
            char hzBuf[128];
            sprintf_s(hzBuf, sizeof(hzBuf), "[OpenVR-Overlay] Native HMD refresh rate detected: %d Hz", g_hmdHz);
            LogMsg(hzBuf);
        }
    }

    if (!g_pVROverlay) {
        g_pVROverlay = vr::VROverlay();
        if (!g_pVROverlay) {
            LogMsg("[OpenVR-Overlay] ERROR: VROverlay interface is NULL");
            return false;
        }
    }

    if (g_hVROverlay == vr::k_ulOverlayHandleInvalid) {
        uint64_t now = GetTickCount64();
        if (now < g_overlayRetryAfterTick)
            return false;

        // Unique key per process: auxiliary child processes also load this proxy
        // from the game folder and would otherwise collide (VROverlayError_KeyInUse).
        char overlayKey[64];
        sprintf_s(overlayKey, sizeof(overlayKey), "VRDLSS5_HUD_%lu", GetCurrentProcessId());

        g_hVROverlay = vr::k_ulOverlayHandleInvalid;
        vr::EVROverlayError ovrErr = g_pVROverlay->CreateOverlay(overlayKey, "DLSS 5 VR Controller", &g_hVROverlay);
        if (ovrErr != vr::VROverlayError_None) {
            // OpenVR can leave a non-invalid handle on failure; never let it
            // reach SetOverlayRaw, or the self-heal loop spams SteamVR forever.
            g_hVROverlay = vr::k_ulOverlayHandleInvalid;

            g_overlayFailCount = (g_overlayFailCount < 30) ? g_overlayFailCount + 1 : 30;
            g_overlayRetryAfterTick = now + (uint64_t)(1000 * g_overlayFailCount);

            char buf[160];
            sprintf_s(buf, sizeof(buf), "[OpenVR-Overlay] CreateOverlay failed: %d (retry in %d ms)",
                      ovrErr, 1000 * g_overlayFailCount);
            LogMsg(buf);
            return false;
        }
        g_overlayFailCount = 0;
        g_pVROverlay->SetOverlayAlpha(g_hVROverlay, 0.96f);
        ApplyOverlayTransformAndScale();
        LogMsg("[OpenVR-Overlay] SteamVR Overlay created and armed successfully!");
    }

    return true;
}

static void UpdateOpenVROverlay(bool visible, bool isDirty)
{
    if (!ShouldEnableVROverlay()) {
        return;
    }

    static bool s_lastVisible = false;
    static int s_lastScale = -1;
    static int s_lastPos = -1;

    if (!visible) {
        if (s_lastVisible) {
            if (g_pVROverlay && g_hVROverlay != vr::k_ulOverlayHandleInvalid) {
                g_pVROverlay->HideOverlay(g_hVROverlay);
            }
            s_lastVisible = false;
        }
        return;
    }

    if (!EnsureOpenVROverlay()) {
        return;
    }

    if (!s_lastVisible) {
        g_pVROverlay->ShowOverlay(g_hVROverlay);
        s_lastVisible = true;
        isDirty = true; // Force fresh texture upload on reveal!
    }

    if (s_lastScale != g_hudScale || s_lastPos != g_hudPosIndex) {
        s_lastScale = g_hudScale;
        s_lastPos = g_hudPosIndex;
        ApplyOverlayTransformAndScale();
    }

    if (isDirty) {
        vr::EVROverlayError ovrErr = g_pVROverlay->SetOverlayRaw(g_hVROverlay, s_hudPixelsVR, HUD_WIDTH, HUD_HEIGHT, 4);
        if (ovrErr != vr::VROverlayError_None) {
            char buf[128];
            sprintf_s(buf, sizeof(buf), "[OpenVR-Overlay] SetOverlayRaw error: %d - initiating self-healing recovery...", ovrErr);
            LogMsg(buf);

            // Self-healing recovery: destroy and recreate overlay handle
            g_pVROverlay->DestroyOverlay(g_hVROverlay);
            g_hVROverlay = vr::k_ulOverlayHandleInvalid;
            s_lastVisible = false;
            s_lastScale = -1;
            s_lastPos = -1;

            if (EnsureOpenVROverlay()) {
                g_pVROverlay->ShowOverlay(g_hVROverlay);
                s_lastVisible = true;
                ApplyOverlayTransformAndScale();
                vr::EVROverlayError retryErr = g_pVROverlay->SetOverlayRaw(g_hVROverlay, s_hudPixelsVR, HUD_WIDTH, HUD_HEIGHT, 4);
                if (retryErr == vr::VROverlayError_None) {
                    LogMsg("[OpenVR-Overlay] Self-healing recovery successful: texture re-uploaded!");
                } else {
                    sprintf_s(buf, sizeof(buf), "[OpenVR-Overlay] Recovery retry error: %d", retryErr);
                    LogMsg(buf);
                    // Back off so a persistent failure cannot hammer SteamVR IPC
                    g_overlayFailCount = (g_overlayFailCount < 30) ? g_overlayFailCount + 1 : 30;
                    g_overlayRetryAfterTick = GetTickCount64() + (uint64_t)(1000 * g_overlayFailCount);
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Input Polling & 500 ms Debounce Manager
// ----------------------------------------------------------------------------
static void UpdateKey(int vk, uint64_t now)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    KeyTracker &k = g_keys[vk];
    if (down) {
        if (!k.isDown) {
            k.isDown = true;
            k.justPressed = true;
            k.downSince = now;
            k.lastRepeat = now;
        } else {
            k.justPressed = false;
        }
    } else {
        k.isDown = false;
        k.justPressed = false;
        k.downSince = 0;
        k.lastRepeat = 0;
    }
}

static bool CheckAction(int vk, uint64_t now, bool &isHolding)
{
    KeyTracker &k = g_keys[vk];
    if (k.justPressed) {
        isHolding = false;
        return true;
    }
    if (k.isDown && (now - k.downSince >= 300) && (now - k.lastRepeat >= 50)) {
        k.lastRepeat = now;
        isHolding = true;
        return true;
    }
    return false;
}

static void PollInput()
{
    uint64_t now = GetTickCount64();

    // 1. Keyboard F6 & Gamepad Select + L3 Toggle (avec cooldown 350ms anti-rebond)
    static uint64_t s_lastToggleTick = 0;
    bool toggleRequested = false;

    UpdateKey(VK_F6, now);
    if (g_keys[VK_F6].justPressed && (now - s_lastToggleTick >= 350)) {
        s_lastToggleTick = now;
        toggleRequested = true;
    }

    // Polling XInput (Xbox, emulators)
    XINPUT_STATE xstate;
    ZeroMemory(&xstate, sizeof(XINPUT_STATE));
    bool xinputConnected = false;
    for (DWORD i = 0; i < 4; i++) {
        DWORD res = ERROR_DEVICE_NOT_CONNECTED;
        if (g_origRealVR_GetState) {
            res = g_origRealVR_GetState(i, &xstate);
        } else if (g_origXInput1_4_GetState) {
            res = g_origXInput1_4_GetState(i, &xstate);
        } else {
            res = XInputGetState(i, &xstate);
        }
        if (res == ERROR_SUCCESS) {
            xinputConnected = true;
            break;
        }
    }
    WORD xButtons = xinputConnected ? xstate.Gamepad.wButtons : 0;

    // Polling DirectInput / winmm (DualSense PS5, manettes HID natives)
    JOYINFOEX jie;
    ZeroMemory(&jie, sizeof(JOYINFOEX));
    jie.dwSize = sizeof(JOYINFOEX);
    jie.dwFlags = JOY_RETURNALL;
    bool dinputConnected = false;
    for (UINT j = 0; j < 4; j++) {
        MMRESULT jres = g_origJoyGetPosEx ? g_origJoyGetPosEx(j, &jie) : joyGetPosEx(j, &jie);
        if (jres == JOYERR_NOERROR) {
            dinputConnected = true;
            break;
        }
    }
    DWORD dButtons = dinputConnected ? jie.dwButtons : 0;

    // Combinaison universelle Select + L3 (Back + Clic Stick Gauche)
    // Xbox: BACK (0x20) | LEFT_THUMB (0x40) = 0x60
    // DualSense DirectInput: Create/Select (bit 8 = 0x100) | L3 (bit 10 = 0x400) = 0x500
    bool comboDown = ((xButtons & 0x0060) == 0x0060) || 
                     ((dButtons & 0x0500) == 0x0500);
    static bool s_prevCombo = false;
    if (comboDown && !s_prevCombo && (now - s_lastToggleTick >= 350)) {
        s_lastToggleTick = now;
        toggleRequested = true;
    }
    s_prevCombo = comboDown;

    if (toggleRequested) {
        g_hudVisible = !g_hudVisible;
        g_hudDirty = true;
        char buf[128];
        sprintf_s(buf, sizeof(buf), "[VR-DLSS5-HUD] Overlay toggled: %s (Source: %s)", 
            g_hudVisible ? "OPEN" : "CLOSED",
            g_keys[VK_F6].justPressed ? "Keyboard F6" : "Gamepad Select+L3");
        LogMsg(buf);

        if (!g_hudVisible) {
            LONG masked = InterlockedExchange(&g_dpadMaskedSamples, 0);
            if (masked > 0) {
                char maskBuf[128];
                sprintf_s(maskBuf, sizeof(maskBuf),
                    "[Proxy-Input] D-Pad samples masked while HUD was open: %ld", masked);
                LogMsg(maskBuf);
            }
        }
    }

    if (!g_hudVisible) return;

    // 2. Active HUD Controls
    UpdateKey(VK_TAB, now);
    UpdateKey(VK_F7, now);
    UpdateKey(VK_ESCAPE, now);
    UpdateKey(VK_UP, now);
    UpdateKey(VK_DOWN, now);
    UpdateKey(VK_LEFT, now);
    UpdateKey(VK_RIGHT, now);
    UpdateKey(VK_SPACE, now);
    UpdateKey(VK_RETURN, now);
    UpdateKey(VK_F8, now);

    // F8: toggle ResidualAcrossRR without occupying a HUD row
    if (g_keys[VK_F8].justPressed) {
        g_residualAcrossRR = !g_residualAcrossRR;
        g_hudDirty = true;
        g_hasPendingSave = true;
        g_lastChangeTick = now;
        char rrBuf[96];
        sprintf_s(rrBuf, sizeof(rrBuf), "[VR-DLSS5-HUD] ResidualAcrossRR toggled: %s",
                  g_residualAcrossRR ? "ON" : "OFF");
        LogMsg(rrBuf);
    }

    // Close HUD: Escape (Keyboard)
    if (g_keys[VK_ESCAPE].justPressed) {
        g_hudVisible = false;
        g_hudDirty = true;
        s_lastToggleTick = now;
        LogMsg("[VR-DLSS5-HUD] Overlay closed via Escape");
        LONG masked = InterlockedExchange(&g_dpadMaskedSamples, 0);
        if (masked > 0) {
            char maskBuf[128];
            sprintf_s(maskBuf, sizeof(maskBuf),
                "[Proxy-Input] D-Pad samples masked while HUD was open: %ld", masked);
            LogMsg(maskBuf);
        }
        return;
    }

    // Cycle Position: Tab or Gamepad Y (Xbox Y: 0x8000 / DualSense Triangle: bit 3 = 0x0008)
    static bool s_prevPadY = false;
    bool padY = ((xButtons & XINPUT_GAMEPAD_Y) != 0) || ((dButtons & 0x0008) != 0);
    if (g_keys[VK_TAB].justPressed || (padY && !s_prevPadY)) {
        g_hudPosIndex = (g_hudPosIndex + 1) % 4;
        g_hudDirty = true;
        static const char* posNames[] = { "Bottom-Center", "Top-Center", "Top-Right", "Top-Left" };
        char buf[128];
        sprintf_s(buf, sizeof(buf), "[VR-DLSS5-HUD] Position changed to: %s", posNames[g_hudPosIndex]);
        LogMsg(buf);
        g_hasPendingSave = true;
        g_lastChangeTick = now;
    }
    s_prevPadY = padY;

    // Scale Toggle: F7 keyboard only. The R3 gamepad binding was removed; scale
    // and position now live in the "VR HUD Display" menu row.
    if (g_keys[VK_F7].justPressed) {
        g_hudScale = (g_hudScale + 1) % 3;
        g_hudDirty = true;
        static const char* scaleNames[] = { "1.0x (Compact)", "1.5x (Balanced Q3)", "2.0x (Comfort Q3)" };
        char buf[128];
        sprintf_s(buf, sizeof(buf), "[VR-DLSS5-HUD] Scale toggled to: %s", scaleNames[g_hudScale]);
        LogMsg(buf);
        g_hasPendingSave = true;
        g_lastChangeTick = now;
    }

    // Gamepad B or Backspace returns to Main Menu from Submenu
    static bool s_prevPadB = false;
    bool padB = ((xButtons & XINPUT_GAMEPAD_B) != 0) || ((dButtons & 0x0004) != 0); // Xbox B or DualSense Circle
    if ((padB && !s_prevPadB) || g_keys[VK_BACK].justPressed) {
        if (g_currentMenu == MENU_RENDERING) {
            g_currentMenu = MENU_MAIN;
            g_activeRow = 1;
            g_hudDirty = true;
        }
    }
    s_prevPadB = padB;

    // Navigate Rows (7 rows in Main: 0 to 6, 9 rows in Submenu: 0 to 8) - PURE DIGITAL D-PAD
    int maxRows = (g_currentMenu == MENU_MAIN) ? 7 : 9;
    static bool s_prevPadUp = false;
    static bool s_prevPadDown = false;
    bool padUp = ((xButtons & XINPUT_GAMEPAD_DPAD_UP) != 0) || 
                 (dinputConnected && (jie.dwPOV == 0 || jie.dwPOV == 31500 || jie.dwPOV == 4500));
    bool padDown = ((xButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0) || 
                   (dinputConnected && (jie.dwPOV == 18000 || jie.dwPOV == 13500 || jie.dwPOV == 22500));

    if (g_keys[VK_UP].justPressed || (padUp && !s_prevPadUp)) {
        g_activeRow = (g_activeRow + maxRows - 1) % maxRows;
        g_hudDirty = true;
    }
    if (g_keys[VK_DOWN].justPressed || (padDown && !s_prevPadDown)) {
        g_activeRow = (g_activeRow + 1) % maxRows;
        g_hudDirty = true;
    }
    s_prevPadUp = padUp;
    s_prevPadDown = padDown;

    bool actLeft = false;
    bool actRight = false;
    bool isHoldLeft = false;
    bool isHoldRight = false;

    if (CheckAction(VK_LEFT, now, isHoldLeft)) actLeft = true;
    if (CheckAction(VK_RIGHT, now, isHoldRight)) actRight = true;

    static uint64_t s_padLeftSince = 0, s_padLeftRepeat = 0;
    static uint64_t s_padRightSince = 0, s_padRightRepeat = 0;
    bool padLeft = ((xButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0) || 
                   (dinputConnected && (jie.dwPOV == 27000 || jie.dwPOV == 22500 || jie.dwPOV == 31500));
    bool padRight = ((xButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0) || 
                    (dinputConnected && (jie.dwPOV == 9000 || jie.dwPOV == 4500 || jie.dwPOV == 13500));

    if (padLeft) {
        if (s_padLeftSince == 0) {
            s_padLeftSince = now; s_padLeftRepeat = now; actLeft = true; isHoldLeft = false;
        } else if ((now - s_padLeftSince >= 300) && (now - s_padLeftRepeat >= 50)) {
            s_padLeftRepeat = now; actLeft = true; isHoldLeft = true;
        }
    } else {
        s_padLeftSince = 0;
    }

    if (padRight) {
        if (s_padRightSince == 0) {
            s_padRightSince = now; s_padRightRepeat = now; actRight = true; isHoldRight = false;
        } else if ((now - s_padRightSince >= 300) && (now - s_padRightRepeat >= 50)) {
            s_padRightRepeat = now; actRight = true; isHoldRight = true;
        }
    } else {
        s_padRightSince = 0;
    }

    static bool s_prevPadA = false;
    bool padA = ((xButtons & XINPUT_GAMEPAD_A) != 0) || ((dButtons & 0x0002) != 0); // Xbox A or DualSense Cross
    bool actionTrigger = g_keys[VK_SPACE].justPressed || g_keys[VK_RETURN].justPressed || (padA && !s_prevPadA);
    s_prevPadA = padA;

    bool valueChanged = false;

    if (g_currentMenu == MENU_MAIN)
    {
        if (g_activeRow == 0) { // Neural Engine Toggle
            if (actionTrigger || actLeft || actRight) {
                g_masterEnable = !g_masterEnable;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 1) { // Rendu Submenu entry
            if (actionTrigger || actRight) {
                g_currentMenu = MENU_RENDERING;
                g_activeRow = 0; // Focus on Back button
                g_hudDirty = true;
            }
        }
        else if (g_activeRow == 2) { // VR WorkingScale (full amplitude 0.25x - 1.50x, step 0.05)
            if (actionTrigger) {
                g_workingScale = 0.75f;
                valueChanged = true;
            } else if (actLeft && g_workingScale > 0.25f) {
                g_workingScale -= 0.05f;
                if (g_workingScale < 0.25f) g_workingScale = 0.25f;
                valueChanged = true;
            } else if (actRight && g_workingScale < 1.50f) {
                g_workingScale += 0.05f;
                if (g_workingScale > 1.50f) g_workingScale = 1.50f;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 3) { // Placement Mode (Pre-SR vs Post-SR)
            if (actionTrigger || actLeft || actRight) {
                g_runBeforeSR = !g_runBeforeSR;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 4) { // AI Model Preset (0, 1, 2)
            if (actionTrigger || actRight) {
                g_nrPreset = (g_nrPreset + 1) % 3;
                valueChanged = true;
            } else if (actLeft) {
                g_nrPreset = (g_nrPreset + 2) % 3;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 5) { // DLSS5 Style (0 Standard, 1 Natural, 2 Cinematic)
            if (actionTrigger || actRight) {
                g_nrStyle = (g_nrStyle + 1) % 3;
                valueChanged = true;
            } else if (actLeft) {
                g_nrStyle = (g_nrStyle + 2) % 3;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 6) { // VR HUD Display (Position & Scale)
            if (actLeft) {
                g_hudPosIndex = (g_hudPosIndex + 3) % 4;
                valueChanged = true;
            } else if (actRight) {
                g_hudPosIndex = (g_hudPosIndex + 1) % 4;
                valueChanged = true;
            }
            if (actionTrigger) {
                g_hudScale = (g_hudScale + 1) % 3;
                valueChanged = true;
            }
        }
    }
    else // MENU_RENDERING
    {
        if (g_activeRow == 0) { // BACK BUTTON AT TOP
            if (actionTrigger || actLeft) {
                g_currentMenu = MENU_MAIN;
                g_activeRow = 1;
                g_hudDirty = true;
            }
        }
        else if (g_activeRow == 1) { // Intensité DLSS5 (0.00 - 2.00, step 0.05)
            if (actionTrigger) {
                g_nrIntensity = 1.00f;
                valueChanged = true;
            } else if (actLeft && g_nrIntensity > 0.0f) {
                g_nrIntensity -= 0.05f;
                if (g_nrIntensity < 0.0f) g_nrIntensity = 0.0f;
                valueChanged = true;
            } else if (actRight && g_nrIntensity < 2.0f) {
                g_nrIntensity += 0.05f;
                if (g_nrIntensity > 2.0f) g_nrIntensity = 2.0f;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 2) { // Structure Décor (LocalStructure 0.00 - 2.00, step 0.05)
            if (actionTrigger) {
                g_nrLocalStructure = 1.00f;
                valueChanged = true;
            } else if (actLeft && g_nrLocalStructure > 0.0f) {
                g_nrLocalStructure -= 0.05f;
                if (g_nrLocalStructure < 0.0f) g_nrLocalStructure = 0.0f;
                valueChanged = true;
            } else if (actRight && g_nrLocalStructure < 2.0f) {
                g_nrLocalStructure += 0.05f;
                if (g_nrLocalStructure > 2.0f) g_nrLocalStructure = 2.0f;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 3) { // Tonalité Ombres (LocalTone 0.00 - 2.00, step 0.05)
            if (actionTrigger) {
                g_nrLocalTone = 0.00f;
                valueChanged = true;
            } else if (actLeft && g_nrLocalTone > 0.0f) {
                g_nrLocalTone -= 0.05f;
                if (g_nrLocalTone < 0.0f) g_nrLocalTone = 0.0f;
                valueChanged = true;
            } else if (actRight && g_nrLocalTone < 2.0f) {
                g_nrLocalTone += 0.05f;
                if (g_nrLocalTone > 2.0f) g_nrLocalTone = 2.0f;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 4) { // Masque Auto (AutoMask)
            if (actionTrigger || actLeft || actRight) {
                g_nrAutoMask = !g_nrAutoMask;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 5) { // Structure Peau (SkinStructure -1.00 to 2.00, step 0.05)
            if (actionTrigger) {
                g_nrSkinStructure = -1.00f;
                valueChanged = true;
            } else if (actLeft && g_nrSkinStructure > -1.0f) {
                g_nrSkinStructure -= 0.05f;
                if (g_nrSkinStructure < -1.0f) g_nrSkinStructure = -1.0f;
                valueChanged = true;
            } else if (actRight && g_nrSkinStructure < 2.0f) {
                g_nrSkinStructure += 0.05f;
                if (g_nrSkinStructure > 2.0f) g_nrSkinStructure = 2.0f;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 6) { // Passes Neuronales (1, 2, 3)
            if (actLeft) {
                g_nrPasses = (g_nrPasses == 1) ? 3 : (g_nrPasses - 1);
                valueChanged = true;
            } else if (actRight || actionTrigger) {
                g_nrPasses = (g_nrPasses % 3) + 1;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 7) { // Force Transfert IA (TransferStrength 0.00 - 2.00, step 0.05)
            if (actionTrigger) {
                g_nrTransferStrength = 1.00f;
                valueChanged = true;
            } else if (actLeft && g_nrTransferStrength > 0.0f) {
                g_nrTransferStrength -= 0.05f;
                if (g_nrTransferStrength < 0.0f) g_nrTransferStrength = 0.0f;
                valueChanged = true;
            } else if (actRight && g_nrTransferStrength < 2.0f) {
                g_nrTransferStrength += 0.05f;
                if (g_nrTransferStrength > 2.0f) g_nrTransferStrength = 2.0f;
                valueChanged = true;
            }
        }
        else if (g_activeRow == 8) { // Force Couleurs IA (ColourStrength 0.00 - 1.00, step 0.05)
            if (actionTrigger) {
                g_nrColourStrength = 1.00f;
                valueChanged = true;
            } else if (actLeft && g_nrColourStrength > 0.0f) {
                g_nrColourStrength -= 0.05f;
                if (g_nrColourStrength < 0.0f) g_nrColourStrength = 0.0f;
                valueChanged = true;
            } else if (actRight && g_nrColourStrength < 1.0f) {
                g_nrColourStrength += 0.05f;
                if (g_nrColourStrength > 1.0f) g_nrColourStrength = 1.0f;
                valueChanged = true;
            }
        }
    }

    // Real-time immediate update & Arm 500ms debounce
    if (valueChanged) {
        g_hudDirty = true;
        g_hasPendingSave = true;
        g_lastChangeTick = now;
        VrCtlPublish(); // Live instant update!
    }
}

// ----------------------------------------------------------------------------
// Desktop & VR Mirror Floating OSD Window (Transparent Layered Per-Pixel Alpha)
// ----------------------------------------------------------------------------
static HWND g_hOSDWnd = NULL;
static HDC g_hOSDDC = NULL;
static HBITMAP g_hOSDBmp = NULL;
static uint32_t* g_pOSDBits = NULL;
static int g_currentOSDScale = -1;

static void UpdateOSDWindow(bool visible, bool isDirty)
{
    if (!visible) {
        if (g_hOSDWnd && IsWindowVisible(g_hOSDWnd)) {
            ShowWindow(g_hOSDWnd, SW_HIDE);
        }
        return;
    }

    // 1. Enregistrer la classe de fenêtre
    static bool s_classRegistered = false;
    if (!s_classRegistered) {
        WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "VR_DLSS5_OSD_WindowClass";
        RegisterClassExA(&wc);
        s_classRegistered = true;
    }

    // 2. Créer la fenêtre layered transparente si nécessaire
    if (!g_hOSDWnd) {
        g_hOSDWnd = CreateWindowExA(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            "VR_DLSS5_OSD_WindowClass", "VR_DLSS5_HUD_OSD",
            WS_POPUP,
            0, 0, 100, 100,
            NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (!g_hOSDWnd) return;
    }

    if (!isDirty && IsWindowVisible(g_hOSDWnd)) {
        return; // Zero CPU / GDI work when HUD is static
    }

    // 3. Déterminer les dimensions actuelles selon l'échelle desktop (1.0x, 1.25x, 1.5x)
    int scaleNum = 1, scaleDen = 1;
    if (g_hudScale == 1) { scaleNum = 5; scaleDen = 4; }
    else if (g_hudScale == 2) { scaleNum = 3; scaleDen = 2; }
    int curW = (HUD_WIDTH * scaleNum) / scaleDen;
    int curH = (HUD_HEIGHT * scaleNum) / scaleDen;

    // 4. Allouer ou réallouer le DIBSection si l'échelle a changé
    if (!g_hOSDDC || !g_hOSDBmp || g_currentOSDScale != g_hudScale) {
        if (g_hOSDBmp) { DeleteObject(g_hOSDBmp); g_hOSDBmp = NULL; }
        if (g_hOSDDC) { DeleteDC(g_hOSDDC); g_hOSDDC = NULL; }

        HDC hdcScreen = GetDC(NULL);
        g_hOSDDC = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = curW;
        bmi.bmiHeader.biHeight = -curH; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        g_hOSDBmp = CreateDIBSection(g_hOSDDC, &bmi, DIB_RGB_COLORS, (void**)&g_pOSDBits, NULL, 0);
        SelectObject(g_hOSDDC, g_hOSDBmp);
        ReleaseDC(NULL, hdcScreen);
        g_currentOSDScale = g_hudScale;
    }

    if (!g_pOSDBits) return;

    // 5. Transférer avec alpha prémultiplié pour UpdateLayeredWindow
    for (int y = 0; y < curH; y++) {
        int srcY = (y * scaleDen) / scaleNum;
        if (srcY >= HUD_HEIGHT) srcY = HUD_HEIGHT - 1;
        uint32_t* pDstRow = g_pOSDBits + y * curW;
        for (int x = 0; x < curW; x++) {
            int srcX = (x * scaleDen) / scaleNum;
            if (srcX >= HUD_WIDTH) srcX = HUD_WIDTH - 1;
            pDstRow[x] = s_hudPixelsOSD[srcY * HUD_WIDTH + srcX];
        }
    }

    // 6. Calculer la position sur l'écran principal
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    if (scrW <= 0) scrW = 1920;
    if (scrH <= 0) scrH = 1080;

    int dstX = (scrW - curW) / 2;
    int dstY = scrH - curH - 80;

    switch (g_hudPosIndex % 4) {
    case 0: // Bas-Centre
        dstX = (scrW - curW) / 2;
        dstY = scrH - curH - 80;
        break;
    case 1: // Haut-Centre
        dstX = (scrW - curW) / 2;
        dstY = 60;
        break;
    case 2: // Haut-Droite
        dstX = scrW - curW - 80;
        dstY = 60;
        break;
    case 3: // Haut-Gauche
        dstX = 80;
        dstY = 60;
        break;
    }

    HDC hdcScreen = GetDC(NULL);
    POINT ptDst = { dstX, dstY };
    SIZE szDst = { curW, curH };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hOSDWnd, hdcScreen, &ptDst, &szDst, g_hOSDDC, &ptSrc, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, hdcScreen);

    // Maintenir en permanence au premier plan absolu au-dessus du jeu plein écran
    SetWindowPos(g_hOSDWnd, HWND_TOPMOST, dstX, dstY, curW, curH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// ----------------------------------------------------------------------------
// Autonomous Input Watcher Thread (100% Découplé, Zero Crash, 0 ms RAM Sync)
// ----------------------------------------------------------------------------
static void ApplyDeferredPriorityBoost()
{
    if (g_priorityBoosted) return;

    // Defer until the app has had time to boot past the D3D12/OpenXR init where
    // altering scheduling under the loader lock triggered XR_ERROR_CALL_ORDER_INVALID
    // in RealVR64. From this thread (not DllMain) it is safe.
    static uint64_t s_attemptTick = 0;
    if (s_attemptTick == 0) s_attemptTick = GetTickCount64();
    if (GetTickCount64() - s_attemptTick < 6000) return;

    if (g_priorityBoostEnabled &&
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
    {
        LogMsg("[Proxy] Process priority elevated to HIGH_PRIORITY_CLASS (deferred, post-boot).");
    }
    g_priorityBoosted = true;
}

// ----------------------------------------------------------------------------
// SteamVR frame timing: the only reliable VR telemetry we have (the NGX
// evaluate export is not on Cyberpunk's path, so g_evalFrameCounter stays 0).
// Drives the measured FPS badge and the Dynamic VR Frame Guard.
// ----------------------------------------------------------------------------
static void UpdateVrFrameTiming(uint64_t now)
{
    if (!ShouldEnableVROverlay()) return;
    static uint64_t s_lastSample = 0;
    if (!g_pVRSystem) return;
    if (now - s_lastSample < 500) return;
    s_lastSample = now;

    vr::IVRCompositor* pCompositor = vr::VRCompositor();
    if (!pCompositor)
        return;

    vr::Compositor_FrameTiming timing = {};
    timing.m_nSize = sizeof(timing);
    if (!pCompositor->GetFrameTiming(&timing, 0))
        return;

    if (timing.m_flClientFrameIntervalMs > 0.01f)
    {
        float fps = 1000.0f / timing.m_flClientFrameIntervalMs;
        if (fps >= 1.0f && fps <= 240.0f)
        {
            int measured = (int)(fps + 0.5f);
            if (measured != g_liveHz)
            {
                g_liveHz = measured;
                if (g_hudVisible) g_hudDirty = true;
            }
        }
    }

    if (timing.m_flTotalRenderGpuMs > 0.0f && timing.m_flTotalRenderGpuMs < 100.0f)
        g_gpuMsAvg = (g_gpuMsAvg * 0.7) + ((double)timing.m_flTotalRenderGpuMs * 0.3);

    const int budgetHz = (g_hmdHz > 0) ? g_hmdHz : 72;
    const double budgetMs = 1000.0 / (double)budgetHz;
    const bool reprojecting = timing.m_nNumFramePresents >= 2;

    // Only react to a sustained condition (~1.5 s) so loading hitches or a
    // single late frame cannot silently drop the model resolution.
    static int s_hotSamples = 0;
    const bool hot = (g_gpuMsAvg > (budgetMs - 0.88)) || reprojecting;
    s_hotSamples = hot ? (s_hotSamples + 1) : 0;

    // Dynamic VR Frame Guard: drop WorkingScale one notch when the GPU frame time
    // approaches the V-Sync cliff (or SteamVR is already synthesizing frames).
    if (g_frameGuardActive && g_workingScale > 0.50f && s_hotSamples >= 3)
    {
        float oldScale = g_workingScale;
        if (g_workingScale > 0.70f) g_workingScale = 0.66f;
        else g_workingScale = 0.50f;

        g_frameGuardTriggered = true;
        g_hudDirty = true;
        g_hasPendingSave = true;
        g_lastChangeTick = now;

        char guardBuf[256];
        sprintf_s(guardBuf, sizeof(guardBuf),
            "[VR-DLSS5-GUARD] GPU %.2f ms / budget %.2f ms (%d Hz, %u presents) -> WorkingScale %.2f -> %.2f",
            g_gpuMsAvg, budgetMs, budgetHz, timing.m_nNumFramePresents, oldScale, g_workingScale);
        LogMsg(guardBuf);
    }
}

static DWORD WINAPI InputWatcherThread(LPVOID lpParam)
{
    LogMsg("[Proxy] Input Watcher Thread started.");

    HDC hdc = GetDC(NULL);
    if (hdc) {
        int vRef = GetDeviceCaps(hdc, VREFRESH);
        ReleaseDC(NULL, hdc);
        if (vRef >= 60 && vRef <= 240) g_liveHz = vRef;
    }

    // Initialize the offscreen GDI rasterizer DC and DIB section
    InitGDIRasterizer();

    LogMsg("[Proxy] Input Watcher Thread ready: polling F6 and Select+L3...");

    while (true)
    {
        uint64_t now = GetTickCount64();
        if (g_bootTick == 0) g_bootTick = now;

        // 1. Initialiser paresseusement les variables au premier lancement
        InitVariablesFromAddonOrIni();

        // 2. Maintenir la connexion OpenVR active et purger la file IPC d'événements uniquement quand le HUD est actif
        if (g_hudVisible && ShouldEnableVROverlay()) {
            EnsureOpenVROverlay();
            if (g_pVROverlay && g_hVROverlay != vr::k_ulOverlayHandleInvalid) {
                vr::VREvent_t vrEvent;
                while (g_pVROverlay->PollNextOverlayEvent(g_hVROverlay, &vrEvent, sizeof(vrEvent))) {
                    if (vrEvent.eventType == vr::VREvent_Quit || vrEvent.eventType == vr::VREvent_ProcessQuit) {
                        LogMsg("[OpenVR-Overlay] SteamVR quit event detected, resetting overlay connection");
                        g_pVROverlay = NULL;
                        g_hVROverlay = vr::k_ulOverlayHandleInvalid;
                        g_openvrInitialized = false;
                        break;
                    }
                }
            }
        }

        // Intercepter dynamiquement de nouveaux modules XInput si charges tardivement
        // et re-verifier le stub RealVR64 (LukeRoss peut le reconstruire).
        static uint64_t s_lastHookCheck = 0;
        static int s_hookChecks = 0;
        if (now - s_lastHookCheck >= 1000) {
            s_lastHookCheck = now;
            if (s_hookChecks < 10) {
                s_hookChecks++;
                InstallXInputHooks();
            }
            MaintainRealVRXInputStubPatch();
        }

        // SteamVR frame timing -> measured FPS badge + Frame Guard
        UpdateVrFrameTiming(now);

        // 3. Écouter les entrées clavier (F6) et manettes (Select+L3)
        PollInput();

        // 4. Mettre à jour les affichages Bureau et Casque VR uniquement lors d'un changement
        bool isDirty = g_hudDirty;
        if (g_hudVisible && isDirty) {
            RenderModernHUD(g_hGdiMemDC, g_pGdiBits, g_masterEnable, g_workingScale, g_runBeforeSR, 
                            g_nrPreset, g_residualAcrossRR, g_nrIntensity, g_nrStyle,
                            g_hudPosIndex, g_hudScale, g_activeRow, g_liveHz, g_frameGuardTriggered);
        }
        UpdateOSDWindow(g_hudVisible, isDirty);
        UpdateOpenVROverlay(g_hudVisible, isDirty);
        if (isDirty) {
            g_hudDirty = false;
        }

        // 5. Persistence différée (500 ms debounce sans micro-stutter)
        if (g_hasPendingSave && (now - g_lastChangeTick >= 500))
        {
            g_hasPendingSave = false;
            CommitSettingsToDisk();
        }

        // Mode ultra-leger zero-stutter pour VR (LukeRoss 72Hz Quest 3 / Pimax) :
        // - HUD ferme : 20 Hz (Sleep 50ms) -> CPU quasi 0.000%, reactivite F6 / Select+L3 instantanee (50ms)
        // - HUD ouvert : 30 Hz (Sleep 33ms) -> navigation fluide, et zero recalcul/blit si inactif (g_hudDirty)
        Sleep(g_hudVisible ? 33 : 50);
    }
    return 0;
}

extern "C" {

int WINAPI Proxy_NVSDK_NGX_D3D12_EvaluateFeature(void* pCmdList, void* pHandle, void* pParameters, void* pCallback)
{
    g_evalFrameCounter++;

    // Ultra-lean zero-overhead pass-through (zero disk I/O, zero string serialization, zero FP math)
    if (g_pfnNGXEvaluateFeature)
    {
        return g_pfnNGXEvaluateFeature(pCmdList, pHandle, pParameters, pCallback);
    }
    return 0;
}



int WINAPI Proxy_NVSDK_NGX_D3D12_ReleaseFeature(void* pHandle)
{
    InitProxy();
    if (!g_pfnNGXReleaseFeature && g_hRealVR)
        g_pfnNGXReleaseFeature = (PFN_NVSDK_NGX_D3D12_ReleaseFeature)GetProcAddress(g_hRealVR, "NVSDK_NGX_D3D12_ReleaseFeature");

    char buf[128];
    sprintf_s(buf, sizeof(buf), "[VR-DLSS5] ReleaseFeature called: handle=%p", pHandle);
    LogMsg(buf);

    if (g_pfnNGXReleaseFeature)
        return g_pfnNGXReleaseFeature(pHandle);
    return 1;
}

DWORD WINAPI Proxy_XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState)
{
    InitProxy();
    DWORD res = ERROR_DEVICE_NOT_CONNECTED;
    if (g_origRealVR_GetState) {
        res = g_origRealVR_GetState(dwUserIndex, pState);
    } else if (g_origXInput1_4_GetState) {
        res = g_origXInput1_4_GetState(dwUserIndex, pState);
    } else if (g_origXInput9_1_0_GetState) {
        res = g_origXInput9_1_0_GetState(dwUserIndex, pState);
    } else {
        res = XInputGetState(dwUserIndex, pState);
    }
    return FilterXInputState(dwUserIndex, pState, res);
}

}


