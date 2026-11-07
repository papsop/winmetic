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
#include <vector>
#include <cstdint>

namespace winmetic {

enum class MouseButton {
    Left,
    Right,
    Middle
};

struct Point {
    int32_t x = 0;
    int32_t y = 0;
};

/**
 * @brief Thread-safe in-memory virtual clipboard.
 * Prevents OS clipboard corruption and clashes with external applications.
 */
class VirtualClipboard {
public:
    void set_text(std::wstring_view text) {
        text_ = std::wstring(text);
        has_content_ = true;
    }

    [[nodiscard]] std::wstring get_text() const {
        return text_;
    }

    [[nodiscard]] bool has_text() const {
        return has_content_;
    }

    void clear() {
        text_.clear();
        has_content_ = false;
    }

private:
    std::wstring text_;
    bool has_content_ = false;
};

/**
 * @brief In-memory platform seam tracking virtual pointer, modifier, and focus states.
 * Replaces global Win32 queries (GetKeyState, GetCursorPos, OpenClipboard) in test targets.
 */
class PlatformSeam {
public:
    // Virtual Pointer
    void set_cursor_pos(Point pt) { cursor_pos_ = pt; }
    [[nodiscard]] Point cursor_pos() const { return cursor_pos_; }

    void set_last_cursor(HCURSOR cursor) { last_cursor_ = cursor; }
    [[nodiscard]] HCURSOR last_cursor() const { return last_cursor_; }

    // Virtual Modifiers
    void set_modifier(int vk, bool down) {
        switch (vk) {
            case VK_CONTROL: ctrl_down_ = down; break;
            case VK_SHIFT:   shift_down_ = down; break;
            case VK_MENU:    alt_down_ = down; break;
            default: break;
        }
    }

    [[nodiscard]] bool is_modifier_down(int vk) const {
        switch (vk) {
            case VK_CONTROL: return ctrl_down_;
            case VK_SHIFT:   return shift_down_;
            case VK_MENU:    return alt_down_;
            default: return false;
        }
    }

    [[nodiscard]] WPARAM wparam_mouse_flags() const {
        WPARAM flags = 0;
        if (left_down_)  flags |= MK_LBUTTON;
        if (right_down_) flags |= MK_RBUTTON;
        if (mid_down_)   flags |= MK_MBUTTON;
        if (ctrl_down_)  flags |= MK_CONTROL;
        if (shift_down_) flags |= MK_SHIFT;
        return flags;
    }

    void set_button(MouseButton button, bool down) {
        switch (button) {
            case MouseButton::Left:   left_down_ = down; break;
            case MouseButton::Right:  right_down_ = down; break;
            case MouseButton::Middle: mid_down_ = down; break;
        }
    }

    [[nodiscard]] bool is_button_down(MouseButton button) const {
        switch (button) {
            case MouseButton::Left:   return left_down_;
            case MouseButton::Right:  return right_down_;
            case MouseButton::Middle: return mid_down_;
        }
        return false;
    }

    // Virtual Clipboard
    VirtualClipboard& clipboard() { return clipboard_; }
    const VirtualClipboard& clipboard() const { return clipboard_; }

    // Active Window / Focus Tracking
    void set_active_window(HWND hwnd) { active_hwnd_ = hwnd; }
    [[nodiscard]] HWND active_window() const { return active_hwnd_; }

    void set_captured_window(HWND hwnd) { capture_hwnd_ = hwnd; }
    [[nodiscard]] HWND captured_window() const { return capture_hwnd_; }

private:
    Point cursor_pos_{ 0, 0 };
    HCURSOR last_cursor_ = nullptr;
    HWND active_hwnd_ = nullptr;
    HWND capture_hwnd_ = nullptr;

    bool ctrl_down_ = false;
    bool shift_down_ = false;
    bool alt_down_ = false;
    bool left_down_ = false;
    bool right_down_ = false;
    bool mid_down_ = false;

    VirtualClipboard clipboard_;
};

} // namespace winmetic
