#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace winmetic {

struct MappedFolder {
    std::string host_folder;
    std::string sandbox_folder;
    bool read_only = false;
};

enum class SandboxToggle {
    Default,
    Enable,
    Disable
};

struct SandboxConfig {
    SandboxToggle vgpu = SandboxToggle::Default;
    SandboxToggle networking = SandboxToggle::Default;
    uint32_t memory_in_mb = 0; // 0 = default
    std::vector<MappedFolder> mapped_folders;
    std::string logon_command;

    [[nodiscard]] std::string to_xml() const {
        std::ostringstream ss;
        ss << "<Configuration>\n";

        if (vgpu != SandboxToggle::Default) {
            ss << "  <VGpu>" << (vgpu == SandboxToggle::Enable ? "Enable" : "Disable") << "</VGpu>\n";
        }
        if (networking != SandboxToggle::Default) {
            ss << "  <Networking>" << (networking == SandboxToggle::Enable ? "Enable" : "Disable") << "</Networking>\n";
        }
        if (memory_in_mb > 0) {
            ss << "  <MemoryInMB>" << memory_in_mb << "</MemoryInMB>\n";
        }

        if (!mapped_folders.empty()) {
            ss << "  <MappedFolders>\n";
            for (const auto& folder : mapped_folders) {
                ss << "    <MappedFolder>\n";
                ss << "      <HostFolder>" << folder.host_folder << "</HostFolder>\n";
                if (!folder.sandbox_folder.empty()) {
                    ss << "      <SandboxFolder>" << folder.sandbox_folder << "</SandboxFolder>\n";
                }
                ss << "      <ReadOnly>" << (folder.read_only ? "true" : "false") << "</ReadOnly>\n";
                ss << "    </MappedFolder>\n";
            }
            ss << "  </MappedFolders>\n";
        }

        if (!logon_command.empty()) {
            ss << "  <LogonCommand>\n";
            ss << "    <Command>" << logon_command << "</Command>\n";
            ss << "  </LogonCommand>\n";
        }

        ss << "</Configuration>\n";
        return ss.str();
    }

    bool save_to_file(const std::filesystem::path& path) const {
        std::ofstream out(path);
        if (!out) return false;
        out << to_xml();
        return true;
    }
};

/**
 * @brief Diagnostic check result for Windows Sandbox readiness.
 */
struct SandboxDoctorResult {
    bool is_windows_home = false;
    std::string windows_edition;
    bool virtualization_firmware_enabled = false;
    bool vmcompute_installed = false;
    bool vmcompute_running = false;
    bool sandbox_exe_found = false;
    std::string sandbox_exe_path;

    [[nodiscard]] bool is_ready() const noexcept {
        return !is_windows_home && virtualization_firmware_enabled && vmcompute_installed && sandbox_exe_found;
    }
};

/**
 * @brief Diagnoses the local environment to prevent REGDB_E_CLASSNOTREG (0x80040154)
 * and other common Sandbox initialization crashes.
 */
inline SandboxDoctorResult check_sandbox_readiness() {
    SandboxDoctorResult res;

    // 0. Check Windows Edition
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        wchar_t prod_name[256]{};
        DWORD size = sizeof(prod_name);
        if (RegQueryValueExW(hkey, L"ProductName", nullptr, nullptr, reinterpret_cast<LPBYTE>(prod_name), &size) == ERROR_SUCCESS) {
            std::wstring wname(prod_name);

            // Windows 11 still reports 'Windows 10' in ProductName; correct via CurrentBuildNumber
            wchar_t build_str[64]{};
            DWORD bsize = sizeof(build_str);
            if (RegQueryValueExW(hkey, L"CurrentBuildNumber", nullptr, nullptr, reinterpret_cast<LPBYTE>(build_str), &bsize) == ERROR_SUCCESS) {
                if (_wtoi(build_str) >= 22000 && wname.find(L"Windows 10") != std::wstring::npos) {
                    size_t pos = wname.find(L"Windows 10");
                    wname.replace(pos, 10, L"Windows 11");
                }
            }

            int len = WideCharToMultiByte(CP_UTF8, 0, wname.data(), static_cast<int>(wname.size()), nullptr, 0, nullptr, nullptr);
            res.windows_edition.assign(len, 0);
            WideCharToMultiByte(CP_UTF8, 0, wname.data(), static_cast<int>(wname.size()), res.windows_edition.data(), len, nullptr, nullptr);
            if (wname.find(L"Home") != std::wstring::npos) {
                res.is_windows_home = true;
            }
        }
        RegCloseKey(hkey);
    }

    // 1. Check BIOS / Hardware Virtualization
    res.virtualization_firmware_enabled = IsProcessorFeaturePresent(PF_VIRT_FIRMWARE_ENABLED) != 0;

    // 2. Check Hyper-V Host Compute Service (vmcompute)
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"vmcompute", SERVICE_QUERY_STATUS);
        if (svc) {
            res.vmcompute_installed = true;
            SERVICE_STATUS_PROCESS ssp{};
            DWORD bytes_needed = 0;
            if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes_needed)) {
                res.vmcompute_running = (ssp.dwCurrentState == SERVICE_RUNNING);
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }

    // 3. Check WindowsSandbox.exe presence
    wchar_t sys_dir[MAX_PATH];
    if (GetSystemDirectoryW(sys_dir, MAX_PATH) > 0) {
        std::filesystem::path exe_path = std::filesystem::path(sys_dir) / "WindowsSandbox.exe";
        if (std::filesystem::exists(exe_path)) {
            res.sandbox_exe_found = true;
            res.sandbox_exe_path = exe_path.string();
        }
    }

    return res;
}

/**
 * @brief Launches a .wsb file using the Windows Sandbox executable.
 */
inline bool launch_sandbox(const std::filesystem::path& wsb_path) {
    std::string cmd = "start \"\" \"" + wsb_path.string() + "\"";
    return std::system(cmd.c_str()) == 0;
}

} // namespace winmetic
