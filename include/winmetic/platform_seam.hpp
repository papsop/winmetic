#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>

namespace winmetic
{

struct PlatformSeam
{
  bool hermetic = false;
  POINT virtual_cursor_screen{0, 0};
  uint32_t virtual_modifiers = 0; // bitmask for VK_CONTROL (1<<0), VK_SHIFT (1<<1), VK_MENU (1<<2)
  HWND active_hwnd = nullptr;
  HWND focused_hwnd = nullptr;
  HWND capture_hwnd = nullptr;
  HCURSOR last_cursor = nullptr;
  std::wstring virtual_clipboard;
  bool has_virtual_clipboard = false;

  void set_modifier(UINT vk, bool down) noexcept
  {
    uint32_t mask = 0;
    switch (vk)
    {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
      mask = 1u << 0;
      break;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
      mask = 1u << 1;
      break;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
      mask = 1u << 2;
      break;
    default:
      return;
    }
    if (down)
      virtual_modifiers |= mask;
    else
      virtual_modifiers &= ~mask;
  }

  bool is_modifier_down(UINT vk) const noexcept
  {
    uint32_t mask = 0;
    switch (vk)
    {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
      mask = 1u << 0;
      break;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
      mask = 1u << 1;
      break;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
      mask = 1u << 2;
      break;
    default:
      return false;
    }
    return (virtual_modifiers & mask) != 0;
  }

  void set_clipboard(std::wstring_view text)
  {
    virtual_clipboard = std::wstring(text);
    has_virtual_clipboard = true;
  }

  std::optional<std::wstring> get_clipboard() const
  {
    if (!has_virtual_clipboard)
      return std::nullopt;
    return virtual_clipboard;
  }

  void reset() noexcept
  {
    virtual_cursor_screen = {0, 0};
    virtual_modifiers = 0;
    active_hwnd = nullptr;
    focused_hwnd = nullptr;
    capture_hwnd = nullptr;
    last_cursor = nullptr;
    virtual_clipboard.clear();
    has_virtual_clipboard = false;
  }
};

inline PlatformSeam& global_platform_seam() noexcept
{
  static PlatformSeam instance;
  return instance;
}

class HiddenDesktopScope
{
public:
  HiddenDesktopScope()
  {
    original_desktop_ = GetThreadDesktop(GetCurrentThreadId());
    desktop_ = CreateDesktopW(L"AtlasTestDesktop", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (desktop_)
    {
      SetThreadDesktop(desktop_);
    }
  }

  ~HiddenDesktopScope()
  {
    if (original_desktop_)
    {
      SetThreadDesktop(original_desktop_);
    }
    if (desktop_)
    {
      CloseDesktop(desktop_);
    }
  }

  HiddenDesktopScope(const HiddenDesktopScope&) = delete;
  HiddenDesktopScope& operator=(const HiddenDesktopScope&) = delete;

  HDESK desktop() const noexcept { return desktop_; }
  bool is_valid() const noexcept { return desktop_ != nullptr; }

  // Convenience methods
  bool attach() noexcept
  {
    return desktop_ && SetThreadDesktop(desktop_) != 0;
  }

  void restore() noexcept
  {
    if (original_desktop_)
    {
      SetThreadDesktop(original_desktop_);
    }
  }

  HDESK handle() const noexcept { return desktop_; }

private:
  HDESK original_desktop_ = nullptr;
  HDESK desktop_ = nullptr;
};

} // namespace winmetic
