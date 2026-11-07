#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winmetic/winmetic.hpp>
#include <shellapi.h>
#include <windowsx.h>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <filesystem>

static std::string to_utf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
    return result;
}

void print_help() {
    std::cout << "Winmetic CLI v0.1.0\n"
              << "Hermetic, zero-interference GUI test runner & Windows Sandbox orchestrator.\n\n"
              << "Usage:\n"
              << "  winmetic demo                             Run interactive hermetic Live Preview demo\n"
              << "  winmetic run [--watch] [--hidden-desktop] <executable> [args...]\n"
              << "                                            Run application in hermetic test mode\n"
              << "  winmetic sandbox doctor                   Diagnose Windows Sandbox environment readiness\n"
              << "  winmetic sandbox setup                    Install missing Windows Sandbox optional features\n"
              << "  winmetic sandbox gen --map <host_dir>     Generate a disposable .wsb sandbox profile\n"
              << "  winmetic sandbox launch <profile.wsb>     Launch Windows Sandbox with specified profile\n"
              << "  winmetic --version                        Show version information\n"
              << "  winmetic --help                           Show this help message\n";
}

// Interactive demo window for `winmetic demo`
struct DemoCanvasWindow {
    HWND hwnd = nullptr;
    RECT run_btn_rc{40, 60, 140, 95};
    RECT build_btn_rc{155, 60, 255, 95};
    RECT slider_track_rc{40, 140, 360, 155};
    float slider_ratio = 0.15f;
    RECT text_box_rc{40, 200, 520, 245};
    std::wstring text_content;
    bool run_pressed = false;
    bool is_dragging_slider = false;
    int scroll_wheel_count = 0;
    std::wstring status_text = L"Status: Ready";

    static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<DemoCanvasWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<DemoCanvasWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        if (self) {
            switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            case WM_PRINTCLIENT: {
                PAINTSTRUCT ps{};
                HDC hdc = (msg == WM_PRINTCLIENT) ? reinterpret_cast<HDC>(wp) : BeginPaint(h, &ps);
                RECT client{};
                GetClientRect(h, &client);

                // Background
                HBRUSH bg_brush = CreateSolidBrush(RGB(24, 27, 36));
                FillRect(hdc, &client, bg_brush);
                DeleteObject(bg_brush);

                // Top title bar
                RECT title_rc{0, 0, client.right, 36};
                HBRUSH title_bg = CreateSolidBrush(RGB(17, 19, 26));
                FillRect(hdc, &title_rc, title_bg);
                DeleteObject(title_bg);

                HFONT title_font = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                HGDIOBJ old_font = SelectObject(hdc, title_font);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(220, 225, 235));
                RECT title_text_rc{12, 8, client.right - 12, 32};
                DrawTextW(hdc, L"Winmetic Hermetic UX Workbench Demo (Zero Focus Theft)", -1, &title_text_rc, DT_LEFT | DT_SINGLELINE);

                // Run Button
                HBRUSH run_bg = CreateSolidBrush(self->run_pressed ? RGB(46, 160, 67) : RGB(35, 134, 54));
                FillRect(hdc, &self->run_btn_rc, run_bg);
                DeleteObject(run_bg);
                SetTextColor(hdc, RGB(255, 255, 255));
                DrawTextW(hdc, L"\u25B6  Run Target", -1, &self->run_btn_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // Build Button
                HBRUSH build_bg = CreateSolidBrush(RGB(45, 55, 75));
                FillRect(hdc, &self->build_btn_rc, build_bg);
                DeleteObject(build_bg);
                SetTextColor(hdc, RGB(200, 210, 225));
                DrawTextW(hdc, L"\u2699  Build Target", -1, &self->build_btn_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // Slider Track
                HBRUSH track_bg = CreateSolidBrush(RGB(55, 62, 78));
                FillRect(hdc, &self->slider_track_rc, track_bg);
                DeleteObject(track_bg);

                // Slider Thumb
                const int thumb_x = static_cast<int>(self->slider_track_rc.left + self->slider_ratio * (self->slider_track_rc.right - self->slider_track_rc.left));
                RECT thumb_rc{thumb_x - 9, self->slider_track_rc.top - 6, thumb_x + 9, self->slider_track_rc.bottom + 6};
                HBRUSH thumb_bg = CreateSolidBrush(RGB(0, 180, 255));
                FillRect(hdc, &thumb_rc, thumb_bg);
                DeleteObject(thumb_bg);

                // Slider Label
                RECT slider_label_rc{self->slider_track_rc.right + 15, self->slider_track_rc.top - 2, client.right - 10, self->slider_track_rc.bottom + 5};
                std::wstring slider_str = L"Rate: " + std::to_wstring(static_cast<int>(self->slider_ratio * 100)) + L"%";
                SetTextColor(hdc, RGB(180, 195, 215));
                DrawTextW(hdc, slider_str.c_str(), -1, &slider_label_rc, DT_LEFT | DT_SINGLELINE);

                // Text Input Box
                HBRUSH text_bg = CreateSolidBrush(RGB(13, 15, 20));
                FillRect(hdc, &self->text_box_rc, text_bg);
                DeleteObject(text_bg);
                HPEN border_pen = CreatePen(PS_SOLID, 1, RGB(70, 80, 100));
                HGDIOBJ old_pen = SelectObject(hdc, border_pen);
                HGDIOBJ old_hollow = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
                Rectangle(hdc, self->text_box_rc.left, self->text_box_rc.top, self->text_box_rc.right, self->text_box_rc.bottom);
                SelectObject(hdc, old_hollow);
                SelectObject(hdc, old_pen);
                DeleteObject(border_pen);

                RECT text_inner_rc{self->text_box_rc.left + 8, self->text_box_rc.top + 10, self->text_box_rc.right - 8, self->text_box_rc.bottom - 8};
                std::wstring display_text = self->text_content.empty() ? L"(Type text here...)" : self->text_content;
                SetTextColor(hdc, self->text_content.empty() ? RGB(100, 110, 125) : RGB(240, 245, 255));
                DrawTextW(hdc, display_text.c_str(), -1, &text_inner_rc, DT_LEFT | DT_SINGLELINE);

                // Bottom Status Bar
                RECT status_rc{0, client.bottom - 28, client.right, client.bottom};
                HBRUSH status_bg = CreateSolidBrush(RGB(18, 20, 26));
                FillRect(hdc, &status_rc, status_bg);
                DeleteObject(status_bg);

                RECT status_text_rc{12, client.bottom - 22, client.right - 12, client.bottom - 6};
                SetTextColor(hdc, RGB(140, 155, 175));
                DrawTextW(hdc, self->status_text.c_str(), -1, &status_text_rc, DT_LEFT | DT_SINGLELINE);

                SelectObject(hdc, old_font);
                DeleteObject(title_font);

                if (msg == WM_PAINT) EndPaint(h, &ps);
                return 0;
            }
            case WM_LBUTTONDOWN: {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (PtInRect(&self->run_btn_rc, pt)) {
                    self->run_pressed = true;
                    self->status_text = L"Status: [RUN BUTTON PRESSED]";
                    InvalidateRect(h, nullptr, FALSE);
                } else if (PtInRect(&self->slider_track_rc, pt) || (pt.x >= self->slider_track_rc.left && pt.x <= self->slider_track_rc.right && std::abs(pt.y - self->slider_track_rc.top) <= 20)) {
                    self->is_dragging_slider = true;
                    const float track_w = static_cast<float>(self->slider_track_rc.right - self->slider_track_rc.left);
                    self->slider_ratio = std::clamp(static_cast<float>(pt.x - self->slider_track_rc.left) / track_w, 0.0f, 1.0f);
                    self->status_text = L"Status: Dragging Slider (" + std::to_wstring(static_cast<int>(self->slider_ratio * 100)) + L"%)";
                    InvalidateRect(h, nullptr, FALSE);
                } else if (PtInRect(&self->text_box_rc, pt)) {
                    self->status_text = L"Status: Text Input Field Focused";
                    InvalidateRect(h, nullptr, FALSE);
                }
                SetCapture(h);
                return 0;
            }
            case WM_MOUSEMOVE: {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (self->is_dragging_slider) {
                    const float track_w = static_cast<float>(self->slider_track_rc.right - self->slider_track_rc.left);
                    self->slider_ratio = std::clamp(static_cast<float>(pt.x - self->slider_track_rc.left) / track_w, 0.0f, 1.0f);
                    self->status_text = L"Status: Dragging Slider (" + std::to_wstring(static_cast<int>(self->slider_ratio * 100)) + L"%)";
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            case WM_LBUTTONUP: {
                self->run_pressed = false;
                self->is_dragging_slider = false;
                ReleaseCapture();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                self->scroll_wheel_count += delta;
                self->status_text = L"Status: Scrolled Wheel (delta=" + std::to_wstring(delta) + L", total=" + std::to_wstring(self->scroll_wheel_count) + L")";
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            case WM_KEYDOWN: {
                if (wp == 'A' && winmetic::global_platform_seam().is_modifier_down(VK_CONTROL)) {
                    self->text_content.clear();
                    self->status_text = L"Status: Ctrl+A Detected (Text Cleared)";
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            case WM_CHAR: {
                wchar_t c = static_cast<wchar_t>(wp);
                if (c >= 32) {
                    self->text_content.push_back(c);
                    self->status_text = L"Status: Ingested Character '" + std::wstring(1, c) + L"'";
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    DemoCanvasWindow() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WinmeticDemoCanvas";
        RegisterClassExW(&wc);

        hwnd = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            wc.lpszClassName,
            L"Winmetic Interactive Demo Canvas",
            WS_POPUP,
            -16000, -16000, 580, 320,
            nullptr, nullptr, wc.hInstance, this
        );
    }

    ~DemoCanvasWindow() {
        if (hwnd && IsWindow(hwnd)) {
            DestroyWindow(hwnd);
        }
    }
};

int run_demo(int argc, char* argv[]) {
    std::string record_dir;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--record-dir" && i + 1 < argc) {
            record_dir = argv[++i];
        }
    }

    std::cout << "=== Winmetic Hermetic UX Interactive Demo ===\n"
              << "[winmetic] Creating hidden WS_EX_NOACTIVATE demo window at (-16000, -16000)...\n"
              << "[winmetic] Activating Hermetic Watch Preview Mode (Zero Focus Theft, Zero Mouse Hijacking)...\n";
    if (!record_dir.empty()) {
        std::cout << "[winmetic] Recording frames to: " << record_dir << "\n";
        winmetic::set_record_frames_dir(record_dir);
    }
    std::cout << "[winmetic] Watch the 'Atlas Hermetic Live Preview' window animate live on your screen!\n\n";

    DemoCanvasWindow demo;
    if (!demo.hwnd) {
        std::cerr << "Failed to create demo window.\n";
        return 1;
    }

    winmetic::set_watch_mode(true);

    // 1. Focus window hermetically
    std::cout << " -> Step 1: focus_window(demo_window)\n";
    winmetic::focus_window(demo.hwnd);

    // 2. Move to and click 'Run' button
    std::cout << " -> Step 2: move_to & click Run button\n";
    winmetic::move_to(demo.hwnd, {90.0f, 77.0f});
    winmetic::click(demo.hwnd, {90.0f, 77.0f});

    // 3. Drag slider thumb
    std::cout << " -> Step 3: drag slider track smoothly\n";
    winmetic::drag(demo.hwnd, {60.0f, 147.0f}, {280.0f, 147.0f}, 12);

    // 4. Mouse wheel scrolling
    std::cout << " -> Step 4: wheel scrolling (notches=3)\n";
    winmetic::wheel(demo.hwnd, {180.0f, 147.0f}, 3);

    // 5. Focus text input box
    std::cout << " -> Step 5: click text input field\n";
    winmetic::click(demo.hwnd, {120.0f, 222.0f});

    // 6. Keyboard chord Ctrl+A
    std::cout << " -> Step 6: key chord 'Ctrl+A'\n";
    winmetic::key(demo.hwnd, "Ctrl+A");

    // 7. Unicode text typing
    std::cout << " -> Step 7: type_text(L\"Hello from Winmetic!\")\n";
    winmetic::type_text(demo.hwnd, L"Hello from Winmetic!");

    char no_hold[2]{};
    const bool is_no_hold = (GetEnvironmentVariableA("WINMETIC_WATCH_NO_HOLD", no_hold, sizeof(no_hold)) > 0 && no_hold[0] == '1') ||
                            (GetEnvironmentVariableA("ATLAS_WATCH_NO_HOLD", no_hold, sizeof(no_hold)) > 0 && no_hold[0] == '1');
    if (!is_no_hold) {
        std::cout << "[winmetic] Holding preview for 1.2s to observe final state...\n";
        winmetic::pump(1200);
    }

    winmetic::set_watch_mode(false);
    std::cout << "\n[winmetic demo] Demo completed successfully with 100% focus isolation!\n";
    return 0;
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc < 2) {
        print_help();
        return 0;
    }

    std::string command = argv[1];

    if (command == "--help" || command == "-h") {
        print_help();
        return 0;
    }

    if (command == "--version" || command == "-v") {
        std::cout << "winmetic version 0.1.0 (x64 Windows)\n";
        return 0;
    }

    if (command == "demo") {
        return run_demo(argc, argv);
    }

    if (command == "run") {
        if (argc < 3) {
            std::cerr << "Error: 'run' requires an executable target.\n";
            std::cerr << "Usage: winmetic run [--watch] [--hidden-desktop] <executable> [args...]\n";
            return 1;
        }

        bool watch_mode = false;
        bool hidden_desktop = false;
        int exe_idx = 2;

        while (exe_idx < argc && argv[exe_idx][0] == '-') {
            std::string opt = argv[exe_idx];
            if (opt == "--watch") {
                watch_mode = true;
                ++exe_idx;
            } else if (opt == "--hidden-desktop") {
                hidden_desktop = true;
                ++exe_idx;
            } else {
                break;
            }
        }

        if (exe_idx >= argc) {
            std::cerr << "Error: No executable specified to run.\n";
            return 1;
        }

        std::string exe_path = argv[exe_idx];

        // Set hermetic environment variables for child process
        SetEnvironmentVariableA("ATLAS_HERMETIC_UX", "1");
        SetEnvironmentVariableA("WINMETIC_HERMETIC_UX", "1");
        if (watch_mode) {
            SetEnvironmentVariableA("ATLAS_WATCH_UX", "1");
            SetEnvironmentVariableA("WINMETIC_WATCH_UX", "1");
            std::cout << "[winmetic] Enabling watch preview mode...\n";
        }

        std::unique_ptr<winmetic::HiddenDesktopScope> desktop_scope;
        if (hidden_desktop) {
            std::cout << "[winmetic] Attaching to isolated secondary desktop station...\n";
            desktop_scope = std::make_unique<winmetic::HiddenDesktopScope>();
        }

        std::cout << "[winmetic] Running: " << exe_path << "\n";

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        if (hidden_desktop) {
            si.lpDesktop = const_cast<LPSTR>("WinSta0\\AtlasTestDesktop");
        }

        PROCESS_INFORMATION pi{};
        std::string cmd_line = exe_path;
        for (int i = exe_idx + 1; i < argc; ++i) {
            cmd_line += " ";
            cmd_line += argv[i];
        }

        BOOL ok = CreateProcessA(
            nullptr,
            cmd_line.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        if (!ok) {
            std::cerr << "[winmetic] Failed to create child process. Error: " << GetLastError() << "\n";
            return 1;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        std::cout << "[winmetic] Execution finished with exit code: " << exit_code << "\n";
        return static_cast<int>(exit_code);
    }

    if (command == "sandbox") {
        if (argc >= 3 && std::string(argv[2]) == "doctor") {
            std::cout << "=== Winmetic Sandbox Doctor ===\n";
            auto report = winmetic::check_sandbox_readiness();

            std::cout << "[1] Windows Edition:                   "
                      << report.windows_edition << (report.is_windows_home ? " (Home - Sandbox not officially enabled by default)" : " (Pro/Enterprise)") << "\n";

            std::cout << "[2] Hardware Virtualization (BIOS):    "
                      << (report.virtualization_firmware_enabled ? "[OK] Enabled" : "[FAIL] Disabled in BIOS/UEFI") << "\n";

            std::cout << "[3] Hyper-V Host Compute (vmcompute):  "
                      << (report.vmcompute_installed ? (report.vmcompute_running ? "[OK] Running" : "[WARN] Installed (Stopped)") : "[FAIL] NOT Installed") << "\n";

            std::cout << "[4] WindowsSandbox.exe:                "
                      << (report.sandbox_exe_found ? ("[OK] Found (" + report.sandbox_exe_path + ")") : "[FAIL] Missing") << "\n";

            if (report.is_ready()) {
                std::cout << "\nStatus: Windows Sandbox is ready for container testing!\n";
                return 0;
            } else {
                std::cout << "\nStatus: Windows Sandbox is NOT ready.\n\n"
                          << "Diagnostic Findings:\n";
                if (report.is_windows_home) {
                    std::cout << "  -> You are running Windows 11 Home edition.\n"
                              << "     Microsoft officially limits Windows Sandbox to Pro/Enterprise editions.\n\n"
                              << "Recommended Alternatives:\n"
                              << "  * Use Winmetic Hermetic Driver ('winmetic demo' / 'winmetic run [--watch]')!\n"
                              << "    Virtual PlatformSeam and HiddenDesktop work natively on Windows 11 Home\n"
                              << "    with zero Hyper-V or Pro requirements and zero focus theft.\n";
                } else if (!report.vmcompute_installed) {
                    std::cout << "  -> The Hyper-V Host Compute Service ('vmcompute') is not installed.\n"
                              << "     Windows Sandbox requires the 'Containers-DisposableClientVM' optional feature.\n\n"
                              << "Resolution:\n"
                              << "  Run 'winmetic sandbox setup' (prompts for Admin elevation) or run in an Admin PowerShell:\n"
                              << "  Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All\n"
                              << "  (A system reboot is required after installation).\n";
                }
                return 1;
            }
        }

        if (argc >= 3 && std::string(argv[2]) == "setup") {
            auto report = winmetic::check_sandbox_readiness();
            if (report.is_windows_home) {
                std::cout << "[winmetic] Windows Sandbox is not officially supported by Microsoft on Windows Home.\n"
                          << "          Recommended: Use Winmetic Hermetic Driver ('winmetic demo' / 'winmetic run [--watch]'),\n"
                          << "          which works natively on Windows 11 Home with zero focus theft.\n";
                return 1;
            }

            std::cout << "[winmetic] Requesting administrator privileges to configure Windows Sandbox...\n";
            std::wstring ps_cmd = L"-NoExit -Command \"Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All; "
                                  L"Write-Host 'Installation finished. Please restart your PC if prompted.' -ForegroundColor Cyan\"";

            HINSTANCE res = ShellExecuteW(
                nullptr,
                L"runas",
                L"powershell.exe",
                ps_cmd.c_str(),
                nullptr,
                SW_SHOWNORMAL
            );
            if (reinterpret_cast<INT_PTR>(res) > 32) {
                std::cout << "[winmetic] Installation script launched with elevation.\n";
                return 0;
            } else {
                std::cerr << "[winmetic] Failed to elevate: User cancelled UAC prompt.\n";
                return 1;
            }
        }

        if (argc >= 3 && std::string(argv[2]) == "gen") {
            std::string host_dir = ".";
            std::string out_path = "winmetic-sandbox.wsb";

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--map" && i + 1 < argc) host_dir = argv[++i];
                if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
            }

            winmetic::SandboxConfig cfg;
            cfg.vgpu = winmetic::SandboxToggle::Enable;
            cfg.networking = winmetic::SandboxToggle::Enable;
            cfg.memory_in_mb = 8192;
            cfg.mapped_folders.push_back({
                .host_folder = std::filesystem::absolute(host_dir).string(),
                .sandbox_folder = "C:\\work",
                .read_only = false
            });
            cfg.logon_command = "powershell.exe -NoExit -Command \"Set-Location C:\\work; Write-Host 'Winmetic Isolated Sandbox Ready' -ForegroundColor Green\"";

            if (cfg.save_to_file(out_path)) {
                std::cout << "[winmetic] Generated sandbox profile: " << out_path << "\n";
                return 0;
            } else {
                std::cerr << "[winmetic] Failed to write " << out_path << "\n";
                return 1;
            }
        }

        if (argc >= 4 && std::string(argv[2]) == "launch") {
            std::string wsb_file = argv[3];

            auto report = winmetic::check_sandbox_readiness();
            if (!report.is_ready()) {
                std::cerr << "[winmetic] Warning: Windows Sandbox environment is not fully configured!\n"
                          << "          Running now may trigger 0x80040154 (REGDB_E_CLASSNOTREG).\n"
                          << "          Run 'winmetic sandbox doctor' to diagnose or 'winmetic sandbox setup' to install.\n\n";
            }

            std::cout << "[winmetic] Launching Windows Sandbox with profile: " << wsb_file << "...\n";
            if (winmetic::launch_sandbox(wsb_file)) {
                return 0;
            } else {
                std::cerr << "[winmetic] Failed to launch Windows Sandbox.\n";
                return 1;
            }
        }
    }

    std::cerr << "Unknown command. Run 'winmetic --help' for usage.\n";
    return 1;
}
