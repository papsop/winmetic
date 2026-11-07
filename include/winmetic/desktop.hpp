#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <string_view>
#include <stdexcept>

namespace winmetic {

/**
 * @brief RAII Manager for isolated Win32 Desktop stations.
 * Creates a secondary desktop on the interactive WindowStation (WinSta0)
 * WITHOUT calling SwitchDesktop. Tests running on this desktop can create real
 * HWND trees and process UIA events without displaying on the user's screen
 * or stealing focus.
 */
class HiddenDesktop {
public:
    explicit HiddenDesktop(std::wstring_view name = L"WinmeticTestDesktop")
        : name_(name) {
        prev_desktop_ = GetThreadDesktop(GetCurrentThreadId());

        desktop_ = CreateDesktopW(
            name_.c_str(),
            nullptr,
            nullptr,
            0,
            GENERIC_ALL,
            nullptr
        );

        if (!desktop_) {
            throw std::runtime_error("Winmetic: Failed to create hidden desktop station.");
        }
    }

    ~HiddenDesktop() {
        restore();
        if (desktop_) {
            CloseDesktop(desktop_);
            desktop_ = nullptr;
        }
    }

    // Non-copyable, movable
    HiddenDesktop(const HiddenDesktop&) = delete;
    HiddenDesktop& operator=(const HiddenDesktop&) = delete;

    HiddenDesktop(HiddenDesktop&& other) noexcept
        : name_(std::move(other.name_)),
          desktop_(other.desktop_),
          prev_desktop_(other.prev_desktop_),
          is_attached_(other.is_attached_) {
        other.desktop_ = nullptr;
        other.prev_desktop_ = nullptr;
        other.is_attached_ = false;
    }

    HiddenDesktop& operator=(HiddenDesktop&& other) noexcept {
        if (this != &other) {
            restore();
            if (desktop_) CloseDesktop(desktop_);
            name_ = std::move(other.name_);
            desktop_ = other.desktop_;
            prev_desktop_ = other.prev_desktop_;
            is_attached_ = other.is_attached_;
            other.desktop_ = nullptr;
            other.prev_desktop_ = nullptr;
            other.is_attached_ = false;
        }
        return *this;
    }

    /**
     * @brief Attach the current calling thread to this isolated desktop.
     */
    bool attach() {
        if (!desktop_) return false;
        if (SetThreadDesktop(desktop_)) {
            is_attached_ = true;
            return true;
        }
        return false;
    }

    /**
     * @brief Restore the calling thread to its original desktop station.
     */
    void restore() {
        if (is_attached_ && prev_desktop_) {
            SetThreadDesktop(prev_desktop_);
            is_attached_ = false;
        }
    }

    [[nodiscard]] HDESK handle() const noexcept { return desktop_; }
    [[nodiscard]] const std::wstring& name() const noexcept { return name_; }
    [[nodiscard]] bool is_attached() const noexcept { return is_attached_; }

private:
    std::wstring name_;
    HDESK desktop_ = nullptr;
    HDESK prev_desktop_ = nullptr;
    bool is_attached_ = false;
};

} // namespace winmetic
