<div align="center">

# Winmetic

**The missing *Xvfb* for Windows.**  
Hermetic, zero-interference GUI test runner, virtual platform seam, and Windows Sandbox orchestrator.

[![CI](https://github.com/papsop/winmetic/actions/workflows/ci.yml/badge.svg)](https://github.com/papsop/winmetic/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)]()
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows%2010%2F11-lightgrey.svg)]()

</div>

---

## Why Winmetic?

Running background GUI tests on Linux has been solved for decades with **Xvfb** (X Virtual Framebuffer): tests run silently in background memory without moving your mouse or stealing focus.

On Windows, however, conventional desktop automation tools (`FlaUI`, `WinAppDriver`, `pywinauto`, `AutoIt`) rely on global OS calls (`SendInput`, `SetCursorPos`, `SetForegroundWindow`, `OpenClipboard`). If you run tests in the background while working or gaming:
1. **Physical input hijacking:** `SendInput` jerks your real cursor, drops keystrokes into whatever window you are actively using, and can trip anti-cheat heuristics (Vanguard, BattlEye, EasyAntiCheat) in games.
2. **Foreground focus theft:** Test windows flash and steal active focus, dropping keystrokes or minimizing fullscreen applications.
3. **Clipboard clashes:** Tests overwrite the OS clipboard and crash whenever another application locks a clipboard format.
4. **CI & lock-screen fragility:** Standard UI tests fail completely when running on headless CI runners or whenever your workstation screen is locked.

**Winmetic solves this.** It provides a modular, 3-tier architecture to test native Windows applications hermetically with **zero physical input or focus interference**, complete with a real-time live preview monitor that runs everywhere—including Windows 10/11 Home.

---

## Architecture: 3 Isolation Tiers

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│                   Tier 1: In-Process Hermetic Seam (< 1ms)                       │
│  - Virtual Message Dispatch (WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_KEYDOWN, WM_CHAR)  │
│  - In-Memory Virtual Clipboard & Modifier Tracker (Ctrl / Shift / Alt)           │
│  - Live Watch Preview (HermeticWatchViewer via PrintWindow / WM_PRINT)           │
│  - Offscreen D3D11 WARP Swapchains + WIC Staging Screenshot Encoder              │
│  Best for: Fast local unit & interaction tests, instant dev loops, zero focus    │
└──────────────────────────────────────────────────────────────────────────────────┘
                                        ▲
                                        │
┌──────────────────────────────────────────────────────────────────────────────────┐
│                  Tier 2: Win32 Hidden Desktop Station (~10-50ms)                 │
│  - CreateDesktopW(L"WinmeticDesktop") on WinSta0 without SwitchDesktop           │
│  - Scoped thread switching with winmetic::HiddenDesktopScope                     │
│  - Dedicated HWND hierarchy isolated from physical display                       │
│  - Real OS focus events and IUIAutomation / Accessibility inspection             │
│  Best for: Multi-window integration tests, UIA contract validation, OS menus     │
└──────────────────────────────────────────────────────────────────────────────────┘
                                        ▲
                                        │
┌──────────────────────────────────────────────────────────────────────────────────┐
│              Tier 3: Disposable Windows Sandbox Orchestrator (~5-15s)            │
│  - Dynamic .wsb configuration generation with mapped workspace folders           │
│  - Headless/bounded Hyper-V VM execution with automated logon scripting          │
│  - Process execution boundaries for untrusted code or destructive tasks          │
│  Best for: Autonomous agent runs, clean-machine CI, security isolation           │
└──────────────────────────────────────────────────────────────────────────────────┘
```

---

## Live Preview Mode (`--watch` / `HermeticWatchViewer`)

When running tests completely offscreen (at negative coordinates such as `(-16000, -16000)` or on hidden desktops), you normally cannot see what the test is doing.

**Winmetic solves this with `HermeticWatchViewer`**:
- **Zero Focus Theft:** Opens a passive `WS_EX_NOACTIVATE | WS_EX_TOPMOST` monitor window that displays the offscreen application in real time without ever activating itself or stealing keyboard/mouse focus from your current work.
- **Deep Frame Capture:** Captures the offscreen window via `PrintWindow(PW_RENDERFULLCONTENT)` or `WM_PRINT` (`PRF_CHILDREN | PRF_CLIENT | PRF_ERASEBKGND | PRF_NONCLIENT`), faithfully rendering DirectX, GDI, and DirectComposition surfaces.
- **Live Virtual Cursor & Crosshair:** Renders a cyan crosshair and target ring at the exact virtual cursor position, turning red when mouse buttons are pressed or held during drags.
- **Real-Time HUD Bar:** Displays a dark 24px top status banner showing current action status, client DIP coordinates (`[DIP 100.0, 80.0]`), held mouse buttons, and active modifier keys (`Ctrl`, `Shift`, `Alt`).
- **Runs on All Windows Editions:** Works out-of-the-box on **Windows 10/11 Home, Pro, and Enterprise**—with zero hypervisor or Windows Sandbox requirements.

### Quick Demo & Run Commands

Run the built-in interactive demo to see the watch viewer in action:
```powershell
./build/Release/winmetic.exe demo
```

Run your own test suite with live visual preview:
```powershell
./build/Release/winmetic.exe run --watch ./build/Release/my_tests.exe
```

---

## Comparison Matrix

| Feature | FlaUI / WinAppDriver | Playwright / Puppeteer | Windows Sandbox (`.wsb`) | **Winmetic** |
| :--- | :--- | :--- | :--- | :--- |
| **Application Type** | Native Windows (Win32, WPF) | Web applications | Entire Windows OS | **Native Windows (Win32, DirectX, C++) & Embedded WebViews** |
| **Mouse / Keyboard Hijacking** | **Yes** (`SendInput`) | No (CDP events) | No (isolated in VM) | **Zero hijacking** (virtual input routing; never touches OS input queue) |
| **Focus Stealing & Flashing** | **Yes** (steals foreground focus) | No (headless mode) | Yes (opens full Sandbox window)| **Zero theft** (`WS_EX_NOACTIVATE` / `SW_HIDE` / Hidden Desktops) |
| **Live Visual Monitoring** | No (physical window only) | Browser devtools / headful | Full VM window | **Live Watch Viewer** (`WS_EX_NOACTIVATE` passive overlay + virtual cursor HUD) |
| **Clipboard Cross-talk** | **Yes** (modifies global clipboard)| Isolated to browser | Isolated to VM | **In-memory virtual clipboard** (isolated per test via `ClipboardGuard`) |
| **Runs on Locked Screen / CI** | Fails or needs RDP `tscon` hacks | Yes | Yes | **Yes** (fully offscreen; runs on locked workstations or clean CI) |
| **DirectX / Hardware Acceleration** | Depends on physical GPU | SwiftShader / Software | Hyper-V vGPU | **Offscreen D3D11 WARP** (zero GPU queue stalls or frame drops) |
| **Windows 10/11 Home Support** | Yes | Yes | **No** (Pro/Enterprise only) | **100% Native Support** (Tiers 1 & 2 require zero Hyper-V) |

---

## Getting Started

### Using in CMake Projects

Winmetic can be integrated as a CMake submodule or via `FetchContent`:

```cmake
# Add winmetic to your CMake build
add_subdirectory(path/to/winmetic)

# Link your test target
target_link_libraries(my_ui_tests PRIVATE winmetic)
```

### C++ Test Example

Winmetic exposes a clean C++20 API modeled after native UI test drivers, with automatic coordinate conversion (DIPs to screen pixels), clipboard isolation, and watch viewer support:

```cpp
#include <winmetic/winmetic.hpp>
#include <cassert>

void test_editor_interaction() {
    // 1. Enable hermetic driver mode and live watch preview
    winmetic::set_driver_mode(winmetic::DriverMode::Hermetic);
    winmetic::set_watch_mode(true);

    // 2. Protect host clipboard from any modification
    winmetic::ClipboardGuard clip_guard;

    // 3. Target window (created offscreen or at -16000, -16000)
    HWND hwnd = get_test_window_hwnd();
    winmetic::focus_window(hwnd);

    // 4. Dispatch hermetic gestures directly to the window (zero SendInput!)
    // Coordinates are in device-independent pixels (DIPs)
    winmetic::click(hwnd, { 100.0f, 80.0f });

    // Drag from (50, 50) to (200, 150)
    winmetic::drag(hwnd, { 50.0f, 50.0f }, { 200.0f, 150.0f });

    // Scroll 3 notches
    winmetic::wheel(hwnd, { 100.0f, 100.0f }, 3);

    // 5. Send keyboard shortcuts and text
    winmetic::key(hwnd, "Ctrl+Shift+Tab");
    winmetic::type_text(hwnd, L"Hello Winmetic");

    // 6. Capture offscreen screenshot for verification / CI artifacts
    bool ok = winmetic::screenshot(hwnd, "artifacts/editor_pass.png");
    assert(ok);
}

// Multi-window / UIA test using an isolated desktop station
void test_multi_window_desktop() {
    // Creates and attaches to a secondary desktop on WinSta0 without SwitchDesktop
    winmetic::HiddenDesktopScope desktop_scope(L"WinmeticDesktop");

    HWND root = create_main_window();
    winmetic::focus_window(root);
    winmetic::click(root, { 150.0f, 40.0f });

    // Restores original desktop automatically on scope exit
}
```

---

## CLI Usage

Winmetic includes a standalone CLI (`winmetic.exe`) for running test suites, launching visual previews, and managing Windows Sandbox environments:

```powershell
# 1. Launch the interactive Hermetic Watch Preview demo:
winmetic demo

# 2. Run any test executable with real-time live preview monitor (zero focus theft):
winmetic run --watch ./build/Release/my_tests.exe

# 3. Run an executable silently on an isolated Win32 hidden desktop:
winmetic run ./build/Release/my_tests.exe --arg1 --arg2

# 4. Diagnose your system's Windows Sandbox readiness:
winmetic sandbox doctor

# 5. Generate a disposable Windows Sandbox profile (.wsb) mapping your workspace:
winmetic sandbox gen --map M:\repos\my-app --out test-sandbox.wsb

# 6. Launch the sandbox profile:
winmetic sandbox launch test-sandbox.wsb
```

---

## Troubleshooting & Windows Sandbox Pitfalls

> [!TIP]
> **Recommended Workflow:** For 99% of UI test automation workflows, **use Winmetic Tier 1 (Hermetic Seam + Live Watch Preview) and Tier 2 (Hidden Desktop)** via `winmetic run` or `winmetic run --watch`. They run natively on **every Windows edition (including Windows 10/11 Home)** with zero hypervisor dependencies, sub-millisecond startup, and total immunity to the container pitfalls detailed below.

If you specifically use **Tier 3 (Windows Sandbox `.wsb`)**, here are the platform errors and their exact technical causes:

### 1. `0x80040154 (REGDB_E_CLASSNOTREG)`
* **Symptom:** Windows Sandbox shows *"Windows Sandbox failed to initialize. Class not registered (0x80040154)"*.
* **Root Cause:**
  1. **Hardware Virtualization (Intel VT-x / AMD SVM)** is disabled in your motherboard's BIOS/UEFI.
  2. The **Hyper-V Host Compute Service (`vmcompute`)** or `Containers-DisposableClientVM` optional feature is not installed or stopped.
* **Resolution:**
  * Run `winmetic sandbox doctor` to inspect virtualization state.
  * Enter your BIOS/UEFI settings and enable **SVM Mode** (AMD) or **Intel Virtualization Technology (VT-x)**.
  * On Windows Pro/Enterprise, ensure the `Containers-DisposableClientVM` feature is installed:
    ```powershell
    Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All
    ```

### 2. Why Windows Sandbox Fails on Windows 10/11 Home Edition
* **Symptoms:**
  * `Feature name Containers-DisposableClientVM is unknown`
  * Error `0x80070534 (ERROR_NONE_MAPPED / No mapping between account names and security IDs was done)`
  * Generic `System.Exception: An error occurred while creating the virtual machine`
* **Root Cause:**
  * **Microsoft officially restricts Windows Sandbox to Windows Pro, Enterprise, and Education editions.**
  * Some community guides suggest force-staging `*Containers*.mum` servicing packages via DISM on Windows Home. However, even if the packages are staged, **the underlying Host Networking Service (HNS), Hyper-V Virtual Switch drivers, and container security policies remain incomplete on Windows Home**.
  * During container initialization, the container host attempts to translate container virtual security accounts and build virtual network bridges. On Windows Home, these subsystems fail, producing `0x80070534` (SID translation error) or crashing with `System.Exception`.
* **Resolution:**
  * **Do not waste time hacking host accounts or forcing DISM packages on Windows Home.**
  * Use **Winmetic Tier 1 (In-Process Hermetic Seam), Live Watch Preview (`--watch`), and Tier 2 (`HiddenDesktopScope`)**. They require **zero Hyper-V, zero container features, and zero Pro licenses**, providing faster, more reliable test execution on Windows 10 and 11 Home out of the box.

### 3. Hyper-V Network Conflicts with VPNs / Proxies (`System.Exception`)
* **Symptom:** Launching a `.wsb` profile on Windows Pro throws `System.Exception` or network adapter error.
* **Root Cause:**
  * Third-party VPN adapters, TAP drivers, or corporate firewalls can intercept or clash with the Hyper-V virtual network switch bridge when `<Networking>Enable</Networking>` is specified.
* **Resolution:** Set `<Networking>Disable</Networking>` in your `.wsb` configuration if network access inside the sandbox is not strictly required.

---

## Building from Source

Requirements: Windows 10/11, Visual Studio 2022 (MSVC) or Clang-cl, CMake 3.22+.

```powershell
git clone https://github.com/papsop/winmetic.git
cd winmetic
cmake -B build -S .
cmake --build build --config Release

# Run tests
./build/Release/winmetic_tests.exe

# Launch live preview demo
./build/Release/winmetic.exe demo
```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
