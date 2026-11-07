#include <winmetic/winmetic.hpp>
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
              << "  winmetic sandbox gen --map <host_dir>     Generate a disposable .wsb sandbox profile\n"
              << "  winmetic sandbox launch <profile.wsb>     Launch Windows Sandbox with specified profile\n"
              << "  winmetic --version                        Show version information\n"
              << "  winmetic --help                           Show this help message\n";
}

int main(int argc, char* argv[]) {
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
