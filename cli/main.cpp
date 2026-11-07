#include <winmetic/winmetic.hpp>
#include <shellapi.h>
#include <iostream>
#include <string>
#include <vector>
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
              << "  winmetic run <executable> [args...]       Run an application on an isolated hidden desktop\n"
              << "  winmetic sandbox doctor                   Diagnose Windows Sandbox environment readiness\n"
              << "  winmetic sandbox setup                    Install missing Windows Sandbox optional features\n"
              << "  winmetic sandbox gen --map <host_dir>     Generate a disposable .wsb sandbox profile\n"
              << "  winmetic sandbox launch <profile.wsb>     Launch Windows Sandbox with specified profile\n"
              << "  winmetic --version                        Show version information\n"
              << "  winmetic --help                           Show this help message\n";
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

    if (command == "run") {
        if (argc < 3) {
            std::cerr << "Error: 'run' requires an executable target.\n";
            return 1;
        }

        std::string exe_path = argv[2];
        std::cout << "[winmetic] Attaching to isolated secondary desktop station...\n";

        try {
            winmetic::HiddenDesktop desktop(L"WinmeticRunDesktop");
            desktop.attach();

            std::string desktop_utf8 = to_utf8(desktop.name());
            std::cout << "[winmetic] Running: " << exe_path << " on desktop "
                      << desktop_utf8 << "\n";

            STARTUPINFOA si{};
            si.cb = sizeof(si);
            std::string desktop_str = "WinSta0\\" + desktop_utf8;
            si.lpDesktop = desktop_str.data();

            PROCESS_INFORMATION pi{};
            std::string cmd_line = exe_path;
            for (int i = 3; i < argc; ++i) {
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

        } catch (const std::exception& ex) {
            std::cerr << "[winmetic] Exception: " << ex.what() << "\n";
            return 1;
        }
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
                              << "     Microsoft officially limits Windows Sandbox to Pro/Enterprise editions,\n"
                              << "     which is why 'Feature name Containers-DisposableClientVM is unknown'.\n\n"
                              << "Recommended Alternatives:\n"
                              << "  * Use Winmetic Tier 1 & 2 ('winmetic run')! Virtual Seam and Hidden Desktop\n"
                              << "    work natively on Windows 11 Home with zero Hyper-V or Pro requirements.\n\n"
                              << "  * If you specifically need Windows Sandbox on Home, run:\n"
                              << "    winmetic sandbox setup --home\n"
                              << "    (which stages the servicing container packages via DISM).\n";
                } else if (!report.vmcompute_installed) {
                    std::cout << "  -> The Hyper-V Host Compute Service ('vmcompute') is not installed.\n"
                              << "     Windows Sandbox requires the 'Containers-DisposableClientVM' optional feature.\n\n"
                              << "Resolution:\n"
                              << "  Run 'winmetic sandbox setup' (prompts for Admin) or run in an Admin PowerShell:\n"
                              << "  Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All\n"
                              << "  (A system reboot is required after installation).\n";
                }
                return 1;
            }
        }

        if (argc >= 3 && std::string(argv[2]) == "setup") {
            bool for_home = (argc >= 4 && std::string(argv[3]) == "--home");
            std::cout << "[winmetic] Requesting administrator privileges to install Windows Sandbox feature"
                      << (for_home ? " (Windows Home Mode)..." : "...") << "\n";

            std::wstring ps_cmd;
            if (for_home) {
                ps_cmd = L"-NoExit -Command \"Write-Host 'Staging Container packages on Windows Home via DISM...' -ForegroundColor Yellow; "
                         L"Get-ChildItem -Path $env:SystemRoot\\servicing\\Packages\\*Containers*.mum | ForEach-Object { & dism.exe /online /norestart /add-package:$($_.FullName) }; "
                         L"& dism.exe /online /enable-feature /featurename:Containers-DisposableClientVM /LimitAccess /ALL; "
                         L"Write-Host 'Installation finished! Please restart your PC.' -ForegroundColor Green\"";
            } else {
                ps_cmd = L"-NoExit -Command \"Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All; "
                         L"Write-Host 'Installation finished. Please restart your PC if prompted.' -ForegroundColor Cyan\"";
            }

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
