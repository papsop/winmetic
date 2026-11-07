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

On Windows, however, desktop automation tools (`FlaUI`, `WinAppDriver`, `pywinauto`, `AutoIt`) rely on global OS calls (`SendInput`, `SetCursorPos`, `SetForegroundWindow`, `OpenClipboard`). If you run tests in the background while coding or gaming:
1. **Physical input hijacking:** `SendInput` jerks your real cursor, drops keystrokes into whatever window you are actively using, and can trip anti-cheat heuristics (Vanguard, BattlEye, EasyAntiCheat) in multiplayer games.
2. **Foreground focus theft:** Test windows flash and steal active focus, dropping your keystrokes or minimizing fullscreen apps.
3. **Clipboard clashes:** Tests overwrite the OS clipboard and crash whenever another app locks a clipboard format.
4. **CI & lock-screen fragility:** Standard UI tests fail completely when running on headless CI runners or whenever your workstation screen is locked.

**Winmetic solves this.** It provides a modular, 3-tier architecture to test native Windows applications hermetically with **zero physical input or focus interference**.

---

## Architecture: 3 Isolation Tiers

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│                   Tier 1: In-Process Hermetic Seam (< 1ms)                       │
│  - Virtual Message Dispatch (WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_KEYDOWN)           │
│  - In-Memory Virtual Clipboard & Modifier Tracker (Ctrl/Shift/Alt)               │
│  - Offscreen D3D11 WARP Swapchains + WIC Staging Screenshot Encoder              │
│  Best for: Fast local unit & interaction tests, instant dev loops                │
└──────────────────────────────────────────────────────────────────────────────────┘
                                        ▲
                                        │
┌──────────────────────────────────────────────────────────────────────────────────┐
│                  Tier 2: Win32 Hidden Desktop Station (~10-50ms)                 │
│  - CreateDesktopW(L"WinmeticDesktop") on WinSta0 without SwitchDesktop           │
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

## Comparison Matrix

| Feature | FlaUI / WinAppDriver | Playwright / Puppeteer | Windows Sandbox (`.wsb`) | **Winmetic** |
| :--- | :--- | :--- | :--- | :--- |
| **Application Type** | Native Windows (Win32, WPF) | Web applications | Entire Windows OS | **Native Windows (Win32, DirectX, C++) & Embedded WebViews** |
| **Mouse / Keyboard Hijacking** | **Yes** (`SendInput`) | No (CDP events) | No (isolated in VM) | **Zero hijacking** (virtual input routing; never touches OS input queue) |
| **Focus Stealing & Flashing** | **Yes** (steals foreground focus) | No (headless mode) | Yes (opens full Sandbox window)| **Zero theft** (`WS_EX_NOACTIVATE` / `SW_HIDE` / Hidden Desktops) |
| **Clipboard Cross-talk** | **Yes** (modifies global clipboard)| Isolated to browser | Isolated to VM | **In-memory virtual clipboard** (isolated per test) |
| **Runs on Locked Screen / CI** | Fails or needs RDP `tscon` hacks | Yes | Yes | **Yes** (fully offscreen; runs on locked workstations or clean CI) |
| **DirectX / Hardware Acceleration** | Depends on physical GPU | SwiftShader / Software | Hyper-V vGPU | **Offscreen D3D11 WARP** (zero GPU queue stalls or frame drops) |
| **Failure Screenshot Capture** | `BitBlt` from desktop screen DC | DOM / CDP capture | VM desktop capture | **DirectX staging texture readback** (works on hidden/offscreen windows) |

---

## Getting Started

### Using in CMake Projects (e.g. Atlas or external apps)

Winmetic is designed to be cleanly included as a CMake submodule or via `FetchContent`:

```cmake
# Add winmetic to your CMake build
add_subdirectory(path/to/winmetic)

# Link your test target
target_link_libraries(my_ui_tests PRIVATE winmetic)
```

### C++ Test Example

```cpp
#include <winmetic/winmetic.hpp>
#include <cassert>

void test_tab_drag() {
    // 1. Set up in-memory virtual platform seam
    winmetic::PlatformSeam seam;
    seam.clipboard().set_text(L"Test Data");

    // 2. Wrap window in fluent Driver (zero SendInput!)
    HWND hwnd = get_test_window_hwnd();
    winmetic::Driver driver(hwnd, &seam);

    // 3. Dispatch hermetic gestures directly to window queue
    driver.move_to({ 100, 20 });
    driver.press(winmetic::MouseButton::Left);
    driver.drag_to({ 300, 20 }, std::chrono::milliseconds(50));
    driver.release(winmetic::MouseButton::Left);

    // 4. Send keyboard and text
    driver.key(VK_RETURN);
    driver.type_text(L"Hello Winmetic");

    // 5. Verify isolated clipboard
    assert(seam.clipboard().get_text() == L"Test Data");
}
```

### Capturing Offscreen Screenshots (No `BitBlt`!)

```cpp
#include <winmetic/render.hpp>

void capture_frame(ID3D11Texture2D* backbuffer) {
    winmetic::OffscreenRenderer renderer(1280, 720);
    renderer.save_texture_to_png(backbuffer, L"artifacts/failure_frame.png");
}
```

---

## CLI Usage

Winmetic also bundles a standalone CLI (`winmetic.exe`) for running tests on hidden desktop stations or spinning up disposable Windows Sandbox environments:

```bash
# Run any executable on an isolated hidden desktop station:
winmetic run ./build/Release/my_tests.exe --arg1 --arg2

# Generate a disposable Windows Sandbox profile (.wsb) mapping your workspace:
winmetic sandbox gen --map M:\repos\my-app --out test-sandbox.wsb

# Launch the sandbox profile:
winmetic sandbox launch test-sandbox.wsb
```

---

## Building from Source

Requirements: Windows 10/11, Visual Studio 2022 (MSVC) or Clang-cl, CMake 3.22+.

```bash
git clone https://github.com/papsop/winmetic.git
cd winmetic
cmake -B build -S .
cmake --build build --config Release

# Run tests
./build/Release/winmetic_tests.exe
```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
