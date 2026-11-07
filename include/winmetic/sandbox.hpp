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
 * @brief Launches a .wsb file using the Windows Sandbox executable.
 */
inline bool launch_sandbox(const std::filesystem::path& wsb_path) {
    std::string cmd = "start \"\" \"" + wsb_path.string() + "\"";
    return std::system(cmd.c_str()) == 0;
}

} // namespace winmetic
