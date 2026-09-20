# DLSS 5 <> UEVR : Neural Reconstruction for Unreal Engine VR

[![Platform: Windows 64-bit](https://img.shields.io/badge/Platform-Windows%20x64-blue.svg)](https://github.com/eregnier/uevr-dlss5/releases)
[![Graphics: DirectX 12](https://img.shields.io/badge/Graphics-DirectX%2012-brightgreen.svg)]()
[![VR: OpenXR / SteamVR](https://img.shields.io/badge/VR-OpenXR%20%7C%20SteamVR-orange.svg)]()
[![Framework: Praydog UEVR](https://img.shields.io/badge/Framework-Praydog%20UEVR-purple.svg)](https://github.com/praydog/UEVR)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

Universal bridge and software suite adapting **NVIDIA DLSS 5 Neural Reconstruction** (OptiScaler Pre-SR Multipass engine) for Unreal Engine 4 and 5 games injected with **UEVR (Praydog Universal VR Mod)**, featuring complete in-headset controls in Virtual Reality (OpenXR and SteamVR).

---

## 🚀 Quick Start (For Players)

> [!TIP]
> **No compilation required!**  
> If you simply want to play, download the ready-to-use distribution archive from the [**GitHub Releases**](https://github.com/eregnier/uevr-dlss5/releases) section.

### 1. Download & Preparation
1. Head over to the project's [**Releases page**](https://github.com/eregnier/uevr-dlss5/releases) and download the latest `VR-DLSS5-UEVR-Release.zip`.
2. Extract the archive into a folder of your choice.
3. Obtain the NVIDIA DLSS 5 runtime (`nvngx_dlssnr.dll`, build 310.8+) via official NVIDIA SDK sources or community mirrors.

### 2. One-Click Installation with GUI Installer
1. Launch **`VR-DLSS5-UEVR-Installer.exe`** located inside the extracted folder.
2. **Select your game**:
   - Click **Browse...** (or drag & drop your game's executable into the window).
   - *Smart Detection*: If you select the root game launcher, the installer automatically locates the true Unreal Engine binary inside `Binaries\Win64`.
3. **Select your DLSS 5 model (`nvngx_dlssnr.dll`)**:
   - Click **Select file...** and point to your `nvngx_dlssnr.dll`.
   - The installer verifies its SHA-256 hash automatically and remembers this location for future installs.
4. **Click `Install / Update DLSS 5`**:
   - The installer handles everything: injecting `OptiScaler.dll` into the game directory, writing a VR-tuned `OptiScaler.ini`, and deploying both the native C++ plugin (`VRDLSS5_UEVR_Plugin.dll`) and Lua in-game script (`VRDLSS5.lua`).
   - **Strict Per-Game Isolation**: The plugin is installed specifically to the game's dedicated profile (`%APPDATA%\UnrealVRMod\<Game>\`), leaving your other UEVR games completely untouched.

### 3. Launch & Enable in Virtual Reality
1. Start your VR headset (Meta Quest Link / Virtual Desktop, Valve Index, Bigscreen Beyond, HTC Vive, etc.).
2. Launch the game through Steam, Epic Games, etc.
3. Inject UEVR using `UEVRInjector.exe` as usual.
4. Once in-game:
   - Open the UEVR menu by pressing **`L3 + R3`** (simultaneous click of both controller thumbsticks) or the **`Insert`** key on your keyboard.
   - Select the **`DLSS 5 Neural Reconstruction`** tab.
   - Check **`Enable Neural Engine`** to activate neural reconstruction in real time!

---

## 💡 Architecture & Technical Principles

### 1. Why Pre-SR Multipass is Essential for VR
- **The Naive Approach (Post-SR)** runs the DLSS 5 neural model on the already upscaled 4K stereo image (~29.5 megapixels across both eyes). This pass consumes 8 to 15 ms of GPU time, completely shattering the VR frame budget (8.3 ms @ 120 Hz, 11.1 ms @ 90 Hz, 13.8 ms @ 72 Hz) and triggering severe reprojection.
- **The Pre-SR Multipass Architecture** flips the pipeline: DLSS 5 is executed directly on the internal render resolution color buffer (`WorkingScale = 0.75x` or `0.66x`), evaluating only **~4.15 megapixels** (~0.7 to 1.6 ms on an RTX 4090/5090). The DLSS Super Resolution upscaler then takes over to enlarge the reconstructed image to headset target resolution.

### 2. Full HUD & UI Preservation
In UEVR, the Unreal Engine user interface (Slate / UMG: crosshair, health, compass, inventory) is captured into a separate texture target (`ui_target`) and composited in 3D space. Because this composition occurs **after** DLSS 5 processing, the HUD retains crisp 1:1 vector sharpness **with zero ghosting, blurring, or warping**.

### 3. Neural Engine Disabled by Default
> [!IMPORTANT]
> **The DLSS 5 neural engine is intentionally disabled (`Enabled = false`) upon installation.**  
> This guarantees the game launches with 100% stock performance without unexpected GPU load. You enable it on-demand inside the headset via the UEVR menu.

### 4. DirectX 12 / DirectX 11 Compatibility
- The NVIDIA DLSS 5 runtime (`nvngx_dlssnr.dll`) requires **DirectX 12** or **Vulkan** (asynchronous tensor math).
- For Unreal Engine games that boot in DirectX 11 by default, simply append **`-dx12`** or **`-d3d12`** to your Steam Launch Options to switch the game engine to DirectX 12.

---

## 🎮 In-Game Controls & Shortcuts

| Action | Controller / VR Shortcut | Keyboard Shortcut |
| :--- | :--- | :--- |
| **Open UEVR Menu** | **`L3 + R3`** (Click both sticks simultaneously) | **`Insert`** |
| **Access DLSS 5 Settings** | Tab **`DLSS 5 Neural Reconstruction`** | Same tab |
| **Open Standalone Overlay (Direct)** | - | **`F6`** or **`Home`** |
| **Close Standalone Overlay** | - | **`Escape`** or **`F6`** |
| **Toggle Ray Reconstruction** | - | **`F8`** |

> [!NOTE]
> No background gamepad polling hooks are registered. The plugin relies 100% on UEVR's native input interception (`L3 + R3`), guaranteeing **0% CPU rendering thread overhead**.

---

## ⚙️ DLSS 5 Menu Parameters

| Section | Parameter | Default Value | Description & Recommendation |
| :--- | :--- | :--- | :--- |
| **Header** | **Enable Neural Engine** | `Disabled` | Toggles real-time DLSS 5 neural reconstruction on or off. |
| **General** | **VR WorkingScale** | `0.75x` | Neural inference resolution scale. Lower to `0.66x` or `0.50x` in demanding titles or on 90/120 Hz displays. |
| **General** | **Run Before SR** | `Enabled` | **Required for VR.** Runs DLSS 5 prior to upscaling, reducing compute area by 4x. |
| **General** | **ResidualAcrossRR** | `Disabled` | Preserves residual high-frequency data across Ray Reconstruction passes. Keep off unless active Ray Reconstruction is in use. |
| **General** | **AI Model Preset** | `2 - Performance` | Neural network preset. Mode 2 (Performance) yields the ideal sharpness-to-compute ratio in VR. |
| **General** | **DLSS 5 Style** | `0 - Standard` | Rendering style aesthetic (0: Standard, 1: Natural, 2: Cinematic). |
| **General** | **DLSS 5 Intensity** | `1.00x` | Micro-detail reconstruction intensity (0.00x to 2.00x). |
| **Advanced**| **Structure Décor** | `1.00x` | Accentuation of environment surfaces and geometric boundaries. |
| **Advanced**| **Tonalité Ombres** | `0.00x` | Fine-tuning of deep shadow tonal contrast and specular highlights. |
| **Advanced**| **Auto Face Mask** | `Enabled` | Semantic facial masking preventing skin deformation or facial artifacts. |
| **Advanced**| **Structure Peau** | `-1.00x` | Softening and organic skin texture preservation (-1.00 = automatic). |
| **Advanced**| **AI Passes** | `1 - Single Pass`| Sequential inference passes. Keep at 1 in VR to protect frametimes. |
| **Safety**  | **Dynamic VR Frame Guard** | `Enabled` | Real-time GPU frametime monitor. Dynamically scales resolution if frame delivery approaches deadline. |

---

## 🔄 Uninstallation & Restoration

To remove DLSS 5 from a game cleanly:
1. Launch **`VR-DLSS5-UEVR-Installer.exe`**.
2. Select the game's executable.
3. Click **`Restore Original`**.  
   The installer removes all injected DLLs (`OptiScaler.dll`, `VRDLSS5_UEVR_Plugin.dll`, `VRDLSS5.lua`, etc.) and restores the game to its stock state.

---

## 🛠️ Building from Source (For Developers)

### Prerequisites
- Windows 10/11 x64
- Visual Studio 2022 with C++ Desktop tools (MSVC `cl.exe` v143+ with C++20 support).

### One-Click Global Build
```cmd
cd uevr-dlss5
package_release.bat
```
This script compiles all modules and automatically produces the distribution folder and archive:
- `dist/VR-DLSS5-UEVR-Release/`
- `dist/VR-DLSS5-UEVR-Release.zip`

### Individual Module Compilation
- **UEVR Plugin**:
  ```cmd
  cd plugin
  build.bat
  ```
- **GUI Installer**:
  ```cmd
  cd installer
  build.bat
  ```
- **Standalone Dual-Proxy**:
  ```cmd
  cd proxy
  build.bat
  ```

---

## 📂 Repository Layout

```
uevr-dlss5/
├── installer/             # Standalone Win32 GUI installer (installer.cpp, build.bat)
├── plugin/                # Native UEVR C++ plugin (VRDLSS5_Plugin.cpp, build.bat)
├── scripts/               # Lua script for native UEVR UI integration (VRDLSS5.lua)
├── proxy/                 # Optional dual-proxy (DXGI + OpenVR Overlay)
├── deps/                  # Pre-SR OptiScaler runtime, third-party licenses, VR config
├── dist/                  # Output packages generated by package_release.bat
├── doc/                   # Detailed architectural and engineering documentation
├── package_release.bat    # Automated compilation and packaging script
├── LICENSE                # MIT License + third-party attribution notices
├── VERSION                # Semantic version tag file
└── README.md              # Main documentation
```

---

## ⚖️ Legal & Acknowledgments
- This project is an independent open-source modification under the MIT License and is not affiliated with NVIDIA Corporation, Epic Games, or Valve Corporation.
- The NVIDIA Neural Rendering runtime (`nvngx_dlssnr.dll`) is the intellectual property of NVIDIA Corporation and is not redistributed with this project. Users must obtain it independently under NVIDIA's licensing terms.
- **Praydog UEVR** is developed by [praydog](https://github.com/praydog/UEVR) under the MIT License.
- **OptiScaler** is developed by [OptiScaler contributors](https://github.com/cdozdil/OptiScaler) and the DLSS-NR Pre-SR fork by [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) under the GPL-3.0 License.
