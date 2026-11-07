#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Winmetic Native UX Driver: hermetic, trusted input, waiting, live inspection viewer,
// and failure artefacts for desktop testing with zero focus theft and zero interference.

#include "platform_seam.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace winmetic
{

// Default is Hermetic (no physical input). Choose Physical explicitly via set_driver_mode()
// or by setting WINMETIC_PHYSICAL=1.
enum class DriverMode
{
  Physical,
  Hermetic
};

void set_driver_mode(DriverMode mode) noexcept;
DriverMode driver_mode() noexcept;
bool is_hermetic_mode() noexcept;

void set_watch_mode(bool enabled) noexcept;
bool is_watch_mode() noexcept;
void set_record_frames_dir(std::string_view dir) noexcept;
void update_watch_viewer(HWND target_hwnd, std::wstring_view status_label);
HWND watch_viewer_hwnd() noexcept;

struct Point
{
  float x = 0.0f;
  float y = 0.0f;
};

enum class Button
{
  left,
  right,
  middle
};

using MouseButton = Button;
constexpr Button Left = Button::left;
constexpr Button Right = Button::right;
constexpr Button Middle = Button::middle;

// ---- Decoupled failure handling --------------------------------------------
using FailureHandler = void (*)(const std::string& message);
void set_fatal_handler(FailureHandler fn) noexcept;
void set_check_fail_handler(FailureHandler fn) noexcept;
void notify_test_case_start(std::string_view name) noexcept;
void notify_test_case_skipped() noexcept;

// ---- Desktop preconditions ------------------------------------------------
void ensure_interactive();
void focus_window(HWND hwnd);
std::string check_foreground(HWND hwnd);
std::string check_hit(HWND hwnd, Point at);

// ---- Coordinates -----------------------------------------------------------
float dpi_scale(HWND hwnd);            // device pixels per DIP
POINT to_screen(HWND hwnd, Point dip); // client DIPs -> screen pixels
Point to_client_dip(HWND hwnd, POINT screen);

// ---- Pointer ---------------------------------------------------------------
void move_to(HWND hwnd, Point at);
void hover_message(HWND hwnd, Point at);
void click_message(HWND hwnd, Point at);
void click(HWND hwnd, Point at, Button button = Button::left);
void double_click(HWND hwnd, Point at, Button button = Button::left);
void press(HWND hwnd, Point at, Button button = Button::left);
void release(Button button = Button::left);
void drag(HWND hwnd, Point from, Point to, int steps = 8, Button button = Button::left);
void wheel(HWND hwnd, Point at, int notches);
void wheel_message(HWND hwnd, Point at, int notches, bool control = false, bool shift = false);
void hwheel(HWND hwnd, Point at, int notches);

// ---- Keyboard --------------------------------------------------------------
void key(HWND hwnd, std::string_view chord);
void key_down(HWND hwnd, std::string_view chord);
void key_up(std::string_view chord);
void type_text(HWND hwnd, std::wstring_view text);

class InputGuard
{
public:
  InputGuard();
  ~InputGuard();
  InputGuard(const InputGuard&) = delete;
  InputGuard& operator=(const InputGuard&) = delete;

private:
  POINT cursor_{};
  bool have_cursor_ = false;
};

// ---- Waiting ---------------------------------------------------------------
void pump(int ms);
std::string try_wait(const std::function<bool()>& pred, int timeout_ms, std::string_view description);
bool wait_until(const std::function<bool()>& pred, int timeout_ms, std::string_view description);

// ---- Artefacts -------------------------------------------------------------
std::filesystem::path output_dir();
bool screenshot(HWND hwnd, const std::filesystem::path& png);
bool screenshot_unchecked(HWND hwnd, const std::filesystem::path& png);
bool read_png_size(const std::filesystem::path& png, unsigned& width, unsigned& height);

class FailureCapture
{
public:
  FailureCapture(std::initializer_list<HWND> windows);
  ~FailureCapture();
  void add(HWND hwnd);
  FailureCapture(const FailureCapture&) = delete;
  FailureCapture& operator=(const FailureCapture&) = delete;

private:
  std::vector<HWND> mine_;
};
const std::vector<std::filesystem::path>& failure_artifacts();
bool run_had_selected_cases(bool allow_filtered) noexcept;
void capture_failure_screenshots(std::string_view label);

// ---- Clipboard -------------------------------------------------------------
std::optional<std::wstring> read_clipboard_text();
bool set_clipboard_text(std::wstring_view text);

class ClipboardGuard
{
public:
  ClipboardGuard();
  ~ClipboardGuard();
  ClipboardGuard(const ClipboardGuard&) = delete;
  ClipboardGuard& operator=(const ClipboardGuard&) = delete;

private:
  void* saved_ = nullptr;
  bool uninitialize_com_ = false;
  bool hermetic_ = false;
  std::optional<std::wstring> saved_hermetic_clipboard_;
};

// Fluent driver wrapper
class Driver
{
public:
  explicit Driver(HWND target_hwnd, PlatformSeam* seam = nullptr)
      : target_hwnd_(target_hwnd), seam_(seam) {}

  Driver& move_to(Point pt)
  {
    winmetic::move_to(target_hwnd_, pt);
    return *this;
  }

  Driver& click(Button btn = Button::left)
  {
    if (seam_)
    {
      Point pt = winmetic::to_client_dip(target_hwnd_, seam_->virtual_cursor_screen);
      winmetic::click(target_hwnd_, pt, btn);
    }
    else
    {
      winmetic::click(target_hwnd_, {0, 0}, btn);
    }
    return *this;
  }

  Driver& key(std::string_view chord)
  {
    winmetic::key(target_hwnd_, chord);
    return *this;
  }

  Driver& key(int vk)
  {
    if (vk == VK_RETURN)
      winmetic::key(target_hwnd_, "Enter");
    else if (vk == VK_ESCAPE)
      winmetic::key(target_hwnd_, "Escape");
    else if (vk == VK_TAB)
      winmetic::key(target_hwnd_, "Tab");
    else
    {
      char buf[32];
      snprintf(buf, sizeof(buf), "0x%X", vk);
      winmetic::key(target_hwnd_, buf);
    }
    return *this;
  }

  Driver& type_text(std::wstring_view text)
  {
    winmetic::type_text(target_hwnd_, text);
    return *this;
  }

  HWND hwnd() const noexcept { return target_hwnd_; }
  PlatformSeam* seam() const noexcept { return seam_; }

private:
  HWND target_hwnd_ = nullptr;
  PlatformSeam* seam_ = nullptr;
};

namespace ux = winmetic;

} // namespace winmetic
