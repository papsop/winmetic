#pragma once

#include "seam.hpp"
#include <chrono>
#include <string_view>
#include <thread>

namespace winmetic {

/**
 * @brief High-level fluent test driver for Win32 windows.
 * Drives mouse, keyboard, text input and window messages hermetically
 * without SendInput or physical OS cursor movement.
 */
class Driver {
public:
    explicit Driver(HWND target_hwnd, PlatformSeam* seam = nullptr)
        : target_hwnd_(target_hwnd), seam_(seam) {}

    // Pointer Operations
    Driver& move_to(Point pt) {
        if (seam_) seam_->set_cursor_pos(pt);
        LPARAM lparam = MAKELPARAM(pt.x, pt.y);
        WPARAM wparam = seam_ ? seam_->wparam_mouse_flags() : 0;
        SendMessageW(target_hwnd_, WM_MOUSEMOVE, wparam, lparam);
        return *this;
    }

    Driver& press(MouseButton button = MouseButton::Left) {
        if (seam_) seam_->set_button(button, true);
        Point pt = seam_ ? seam_->cursor_pos() : Point{ 0, 0 };
        LPARAM lparam = MAKELPARAM(pt.x, pt.y);
        WPARAM wparam = seam_ ? seam_->wparam_mouse_flags() : 0;

        UINT msg = WM_LBUTTONDOWN;
        if (button == MouseButton::Right)  msg = WM_RBUTTONDOWN;
        if (button == MouseButton::Middle) msg = WM_MBUTTONDOWN;

        SendMessageW(target_hwnd_, msg, wparam, lparam);
        return *this;
    }

    Driver& release(MouseButton button = MouseButton::Left) {
        if (seam_) seam_->set_button(button, false);
        Point pt = seam_ ? seam_->cursor_pos() : Point{ 0, 0 };
        LPARAM lparam = MAKELPARAM(pt.x, pt.y);
        WPARAM wparam = seam_ ? seam_->wparam_mouse_flags() : 0;

        UINT msg = WM_LBUTTONUP;
        if (button == MouseButton::Right)  msg = WM_RBUTTONUP;
        if (button == MouseButton::Middle) msg = WM_MBUTTONUP;

        SendMessageW(target_hwnd_, msg, wparam, lparam);
        return *this;
    }

    Driver& click(MouseButton button = MouseButton::Left) {
        press(button);
        release(button);
        return *this;
    }

    Driver& double_click(MouseButton button = MouseButton::Left) {
        click(button);
        Point pt = seam_ ? seam_->cursor_pos() : Point{ 0, 0 };
        LPARAM lparam = MAKELPARAM(pt.x, pt.y);
        WPARAM wparam = seam_ ? seam_->wparam_mouse_flags() : 0;

        UINT msg = WM_LBUTTONDBLCLK;
        if (button == MouseButton::Right)  msg = WM_RBUTTONDBLCLK;
        if (button == MouseButton::Middle) msg = WM_MBUTTONDBLCLK;

        SendMessageW(target_hwnd_, msg, wparam, lparam);
        release(button);
        return *this;
    }

    Driver& drag_to(Point end_pt, std::chrono::milliseconds duration = std::chrono::milliseconds(50), int steps = 5) {
        Point start_pt = seam_ ? seam_->cursor_pos() : Point{ 0, 0 };
        press(MouseButton::Left);

        for (int i = 1; i <= steps; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(steps);
            Point current{
                static_cast<int32_t>(start_pt.x + (end_pt.x - start_pt.x) * t),
                static_cast<int32_t>(start_pt.y + (end_pt.y - start_pt.y) * t)
            };
            move_to(current);
            if (duration.count() > 0) {
                std::this_thread::sleep_for(duration / steps);
            }
        }

        release(MouseButton::Left);
        return *this;
    }

    Driver& wheel(int delta_y, bool horizontal = false) {
        Point pt = seam_ ? seam_->cursor_pos() : Point{ 0, 0 };
        LPARAM lparam = MAKELPARAM(pt.x, pt.y);
        WPARAM wparam = MAKEWPARAM(seam_ ? seam_->wparam_mouse_flags() : 0, delta_y);
        UINT msg = horizontal ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL;
        SendMessageW(target_hwnd_, msg, wparam, lparam);
        return *this;
    }

    // Keyboard Operations
    Driver& key_down(int vk) {
        if (seam_) seam_->set_modifier(vk, true);
        SendMessageW(target_hwnd_, WM_KEYDOWN, static_cast<WPARAM>(vk), 0);
        return *this;
    }

    Driver& key_up(int vk) {
        if (seam_) seam_->set_modifier(vk, false);
        SendMessageW(target_hwnd_, WM_KEYUP, static_cast<WPARAM>(vk), 0xC0000001);
        return *this;
    }

    Driver& key(int vk) {
        key_down(vk);
        key_up(vk);
        return *this;
    }

    Driver& type_text(std::wstring_view text) {
        for (wchar_t ch : text) {
            SendMessageW(target_hwnd_, WM_CHAR, static_cast<WPARAM>(ch), 1);
        }
        return *this;
    }

    // Window State
    Driver& resize(int width, int height) {
        SendMessageW(target_hwnd_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(width, height));
        return *this;
    }

    [[nodiscard]] HWND hwnd() const { return target_hwnd_; }
    [[nodiscard]] PlatformSeam* seam() const { return seam_; }

private:
    HWND target_hwnd_;
    PlatformSeam* seam_ = nullptr;
};

} // namespace winmetic
