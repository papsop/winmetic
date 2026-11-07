#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winmetic/winmetic.hpp>
#include <windowsx.h>
#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <filesystem>
#include <cassert>

namespace fs = std::filesystem;

#define CHECK(...) do { \
    if (!(__VA_ARGS__)) { \
        std::cerr << "FAILED CHECK: " #__VA_ARGS__ " at " __FILE__ ":" << __LINE__ << "\n"; \
        g_failures++; \
    } \
} while(0)

#define CHECK_FALSE(...) CHECK(!(__VA_ARGS__))

#define REQUIRE(...) do { \
    if (!(__VA_ARGS__)) { \
        std::cerr << "FATAL REQUIRE FAILED: " #__VA_ARGS__ " at " __FILE__ ":" << __LINE__ << "\n"; \
        std::exit(1); \
    } \
} while(0)

static int g_failures = 0;

struct Event
{
  UINT msg = 0;
  WPARAM wparam = 0;
  int x = 0, y = 0;      // device pixels, client
  bool captured = false; // this window held capture when the event arrived
  bool ctrl = false, shift = false, alt = false;
};

struct TestWindow
{
  HWND hwnd = nullptr;
  std::vector<Event> events;
  std::wstring chars;
  int client_w = 400, client_h = 300;
  bool paint_test_color = false;
  bool capture_on_down = true;

  static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
  {
    auto* self = reinterpret_cast<TestWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
      self = static_cast<TestWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self)
    {
      Event e;
      e.msg = msg;
      e.wparam = wp;
      e.captured = GetCapture() == h;
      if (winmetic::is_hermetic_mode())
      {
        auto& seam = winmetic::global_platform_seam();
        e.ctrl = seam.is_modifier_down(VK_CONTROL);
        e.shift = seam.is_modifier_down(VK_SHIFT);
        e.alt = seam.is_modifier_down(VK_MENU);
      }
      else
      {
        e.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        e.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        e.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
      }
      switch (msg)
      {
      case WM_PAINT:
      case WM_PRINTCLIENT:
        if (self->paint_test_color)
        {
          PAINTSTRUCT paint{};
          HDC dc = (msg == WM_PRINTCLIENT) ? reinterpret_cast<HDC>(wp) : BeginPaint(h, &paint);
          RECT client{};
          GetClientRect(h, &client);
          if (msg == WM_PRINTCLIENT)
          {
            POINT pt{0, 0};
            ClientToScreen(h, &pt);
            RECT win_rc{};
            GetWindowRect(h, &win_rc);
            OffsetRect(&client, pt.x - win_rc.left, pt.y - win_rc.top);
          }
          HBRUSH brush = CreateSolidBrush(RGB(17, 91, 203));
          FillRect(dc, &client, brush);
          DeleteObject(brush);
          if (msg == WM_PAINT)
            EndPaint(h, &paint);
          return 0;
        }
        break;
      case WM_LBUTTONDOWN:
        if (self->capture_on_down)
        {
          SetCapture(h);
          if (winmetic::is_hermetic_mode())
            winmetic::global_platform_seam().capture_hwnd = h;
        }
        [[fallthrough]];
      case WM_LBUTTONUP:
      case WM_MOUSEMOVE:
        e.x = GET_X_LPARAM(lp);
        e.y = GET_Y_LPARAM(lp);
        e.captured = (GetCapture() == h) || (winmetic::is_hermetic_mode() && winmetic::global_platform_seam().capture_hwnd == h);
        self->events.push_back(e);
        if (msg == WM_LBUTTONUP)
        {
          ReleaseCapture();
          if (winmetic::is_hermetic_mode())
            winmetic::global_platform_seam().capture_hwnd = nullptr;
        }
        return 0;
      case WM_MOUSEWHEEL:
        e.wparam = static_cast<WPARAM>(static_cast<int>(GET_WHEEL_DELTA_WPARAM(wp)));
        self->events.push_back(e);
        return 0;
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
        self->events.push_back(e);
        return 0;
      case WM_KEYUP:
      case WM_SYSKEYUP:
        self->events.push_back(e);
        return 0;
      case WM_CHAR:
        self->chars.push_back(static_cast<wchar_t>(wp));
        return 0;
      case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU)
          return 0; // no menu loop on Alt / F10
        break;
      }
    }
    return DefWindowProcW(h, msg, wp, lp);
  }

  explicit TestWindow(bool foreground = false, bool hidden = false)
  {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinmeticTestWindowClass";
    RegisterClassExW(&wc);

    DWORD ex_style = 0;
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (hidden)
    {
      style = WS_POPUP;
      ex_style |= WS_EX_NOACTIVATE;
    }

    hwnd = CreateWindowExW(
      ex_style,
      wc.lpszClassName,
      L"Winmetic Test Target Window",
      style,
      hidden ? -16000 : 100,
      hidden ? -16000 : 100,
      client_w,
      client_h,
      nullptr,
      nullptr,
      wc.hInstance,
      this
    );

    if (!hidden && foreground)
    {
      ShowWindow(hwnd, SW_SHOW);
      UpdateWindow(hwnd);
    }
  }

  ~TestWindow()
  {
    if (hwnd && IsWindow(hwnd))
      DestroyWindow(hwnd);
  }

  std::vector<Event> of(UINT msg) const
  {
    std::vector<Event> res;
    for (const auto& e : events)
    {
      if (e.msg == msg)
        res.push_back(e);
    }
    return res;
  }

  std::vector<WPARAM> keys() const
  {
    std::vector<WPARAM> res;
    for (const auto& e : events)
    {
      if (e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN)
        res.push_back(e.wparam);
    }
    return res;
  }

  bool released_all_modifiers() const
  {
    auto& seam = winmetic::global_platform_seam();
    return !seam.is_modifier_down(VK_CONTROL) &&
           !seam.is_modifier_down(VK_SHIFT) &&
           !seam.is_modifier_down(VK_MENU);
  }
};

void test_platform_seam_and_clipboard()
{
  std::cout << "[Test 1/6] PlatformSeam & Virtual Clipboard...";
  auto& seam = winmetic::global_platform_seam();
  seam.reset();

  seam.virtual_cursor_screen = {150, 250};
  CHECK(seam.virtual_cursor_screen.x == 150);
  CHECK(seam.virtual_cursor_screen.y == 250);

  CHECK_FALSE(seam.is_modifier_down(VK_CONTROL));
  seam.set_modifier(VK_CONTROL, true);
  CHECK(seam.is_modifier_down(VK_CONTROL));
  seam.set_modifier(VK_CONTROL, false);
  CHECK_FALSE(seam.is_modifier_down(VK_CONTROL));

  CHECK_FALSE(seam.get_clipboard().has_value());
  seam.set_clipboard(L"Hermetic Test Data");
  REQUIRE(seam.get_clipboard().has_value());
  CHECK(*seam.get_clipboard() == L"Hermetic Test Data");
  seam.reset();
  CHECK_FALSE(seam.get_clipboard().has_value());

  std::cout << " PASSED\n";
}

void test_hidden_desktop_scope()
{
  std::cout << "[Test 2/6] HiddenDesktopScope station isolation...";
  HDESK original = GetThreadDesktop(GetCurrentThreadId());
  REQUIRE(original != nullptr);
  {
    winmetic::HiddenDesktopScope scope;
    REQUIRE(scope.is_valid());
    CHECK(scope.desktop() != original);
    CHECK(GetThreadDesktop(GetCurrentThreadId()) == scope.desktop());
  }
  // Original desktop restored
  CHECK(GetThreadDesktop(GetCurrentThreadId()) == original);
  std::cout << " PASSED\n";
}

void test_hermetic_driver_operations()
{
  std::cout << "[Test 3/6] Hermetic driver operations on hidden WS_EX_NOACTIVATE window...";
  winmetic::set_driver_mode(winmetic::DriverMode::Hermetic);
  REQUIRE(winmetic::is_hermetic_mode());

  POINT physical_cursor_before{};
  const bool has_cursor = GetCursorPos(&physical_cursor_before) != 0;
  const HWND physical_fg_before = GetForegroundWindow();

  TestWindow w(false, true); // hidden WS_EX_NOACTIVATE window
  SetWindowLongPtrW(w.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(w.hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);

  // 1. Focus window in hermetic mode
  winmetic::focus_window(w.hwnd);
  CHECK(winmetic::global_platform_seam().active_hwnd == w.hwnd);
  CHECK(winmetic::global_platform_seam().focused_hwnd == w.hwnd);
  CHECK(GetForegroundWindow() == physical_fg_before); // Host foreground untouched

  // 2. Click in hermetic mode
  const winmetic::Point click_target{100.0f, 80.0f};
  winmetic::click(w.hwnd, click_target);
  const float scale = winmetic::dpi_scale(w.hwnd);
  const auto downs = w.of(WM_LBUTTONDOWN);
  const auto ups = w.of(WM_LBUTTONUP);
  REQUIRE(downs.size() == 1);
  REQUIRE(ups.size() == 1);
  CHECK(std::abs(downs[0].x - click_target.x * scale) <= 1.0f);
  CHECK(std::abs(downs[0].y - click_target.y * scale) <= 1.0f);
  CHECK(downs[0].captured);
  const POINT expected_click_screen = winmetic::to_screen(w.hwnd, click_target);
  CHECK(winmetic::global_platform_seam().virtual_cursor_screen.x == expected_click_screen.x);
  CHECK(winmetic::global_platform_seam().virtual_cursor_screen.y == expected_click_screen.y);

  if (has_cursor)
  {
    POINT cursor_after_click{};
    REQUIRE(GetCursorPos(&cursor_after_click));
    CHECK(cursor_after_click.x == physical_cursor_before.x);
    CHECK(cursor_after_click.y == physical_cursor_before.y);
  }
  CHECK(GetForegroundWindow() == physical_fg_before);

  // 3. Drag in hermetic mode with capture
  w.events.clear();
  const winmetic::Point drag_from{50.0f, 50.0f};
  const winmetic::Point drag_to{200.0f, 150.0f};
  winmetic::drag(w.hwnd, drag_from, drag_to, 6);
  REQUIRE(w.of(WM_LBUTTONDOWN).size() == 1);
  REQUIRE(w.of(WM_LBUTTONUP).size() == 1);
  std::vector<Event> moves;
  bool pressed = false;
  for (const Event& e : w.events)
  {
    if (e.msg == WM_LBUTTONDOWN)
      pressed = true;
    if (e.msg == WM_LBUTTONUP)
      break;
    if (pressed && e.msg == WM_MOUSEMOVE)
      moves.push_back(e);
  }
  CHECK(moves.size() >= 5);
  for (const Event& m : moves)
    CHECK(m.captured);
  CHECK(std::abs(moves.back().x - drag_to.x * scale) <= 1.0f);
  CHECK(std::abs(moves.back().y - drag_to.y * scale) <= 1.0f);
  const POINT expected_drag_screen = winmetic::to_screen(w.hwnd, drag_to);
  CHECK(winmetic::global_platform_seam().virtual_cursor_screen.x == expected_drag_screen.x);
  CHECK(winmetic::global_platform_seam().virtual_cursor_screen.y == expected_drag_screen.y);

  if (has_cursor)
  {
    POINT cursor_after_drag{};
    REQUIRE(GetCursorPos(&cursor_after_drag));
    CHECK(cursor_after_drag.x == physical_cursor_before.x);
    CHECK(cursor_after_drag.y == physical_cursor_before.y);
  }

  // 4. Wheel in hermetic mode
  w.events.clear();
  winmetic::wheel(w.hwnd, {100.0f, 100.0f}, 3);
  winmetic::wheel(w.hwnd, {100.0f, 100.0f}, -2);
  const auto wheels = w.of(WM_MOUSEWHEEL);
  REQUIRE(wheels.size() == 2);
  CHECK(static_cast<int>(wheels[0].wparam) == 360);
  CHECK(static_cast<int>(wheels[1].wparam) == -240);

  // 5. Key chords in hermetic mode
  w.events.clear();
  winmetic::key(w.hwnd, "Ctrl+Shift+Tab");
  const std::vector<WPARAM> expected_chord{VK_CONTROL, VK_SHIFT, VK_TAB};
  CHECK(w.keys() == expected_chord);
  const auto keydowns = w.of(WM_KEYDOWN);
  REQUIRE(keydowns.size() == 3);
  CHECK(keydowns[2].ctrl);
  CHECK(keydowns[2].shift);
  CHECK_FALSE(keydowns[2].alt);
  CHECK(w.released_all_modifiers());

  w.events.clear();
  winmetic::key(w.hwnd, "Escape");
  winmetic::key(w.hwnd, "Enter");
  winmetic::key(w.hwnd, "Left");
  winmetic::key(w.hwnd, "F10");
  winmetic::key(w.hwnd, "ctrl+a");
  const std::vector<WPARAM> expected_keys{VK_ESCAPE, VK_RETURN, VK_LEFT, VK_F10, VK_CONTROL, 'A'};
  CHECK(w.keys() == expected_keys);

  // 6. Type text in hermetic mode
  w.chars.clear();
  const std::wstring test_unicode = L"Hermetic \u2764\ufe0f \U0001F680 Test";
  winmetic::type_text(w.hwnd, test_unicode);
  CHECK(w.chars == test_unicode);
  CHECK(w.released_all_modifiers());

  // 7. Clipboard operations and ClipboardGuard in hermetic mode
  CHECK(winmetic::set_clipboard_text(L"outer-hermetic-clip"));
  CHECK(winmetic::read_clipboard_text() == std::optional<std::wstring>(L"outer-hermetic-clip"));
  {
    winmetic::ClipboardGuard guard;
    CHECK(winmetic::set_clipboard_text(L"inner-hermetic-clip"));
    CHECK(winmetic::read_clipboard_text() == std::optional<std::wstring>(L"inner-hermetic-clip"));
  }
  CHECK(winmetic::read_clipboard_text() == std::optional<std::wstring>(L"outer-hermetic-clip"));

  // Verify that physical cursor and foreground remained unchanged throughout
  if (has_cursor)
  {
    POINT final_cursor{};
    REQUIRE(GetCursorPos(&final_cursor));
    CHECK(final_cursor.x == physical_cursor_before.x);
    CHECK(final_cursor.y == physical_cursor_before.y);
  }
  CHECK(GetForegroundWindow() == physical_fg_before);

  std::cout << " PASSED\n";
}

void test_screenshot_on_hidden_window()
{
  std::cout << "[Test 4/6] Screenshot capture on hidden window...";
  TestWindow w(false, true);
  w.paint_test_color = true;

  fs::path out_file = winmetic::output_dir() / "winmetic_test_hidden_capture.png";
  std::error_code ec;
  fs::remove(out_file, ec);

  bool ok = winmetic::screenshot(w.hwnd, out_file);
  CHECK(ok);
  CHECK(fs::exists(out_file));

  unsigned w_px = 0, h_px = 0;
  bool read_ok = winmetic::read_png_size(out_file, w_px, h_px);
  CHECK(read_ok);
  RECT wr{};
  GetWindowRect(w.hwnd, &wr);
  CHECK(w_px == static_cast<unsigned>(wr.right - wr.left));
  CHECK(h_px == static_cast<unsigned>(wr.bottom - wr.top));

  fs::remove(out_file, ec);
  std::cout << " PASSED\n";
}

void test_watch_mode_preview_window()
{
  std::cout << "[Test 5/6] Watch mode preview window creation with zero focus theft...";
  const bool prev_watch = winmetic::is_watch_mode();
  const winmetic::DriverMode prev_mode = winmetic::driver_mode();

  struct WatchScopeGuard
  {
    bool watch;
    winmetic::DriverMode mode;
    ~WatchScopeGuard()
    {
      winmetic::set_watch_mode(watch);
      winmetic::set_driver_mode(mode);
      SetEnvironmentVariableA("WINMETIC_WATCH_NO_HOLD", nullptr);
    }
  } guard{prev_watch, prev_mode};

  SetEnvironmentVariableA("WINMETIC_WATCH_NO_HOLD", "1");
  winmetic::set_watch_mode(true);
  CHECK(winmetic::is_watch_mode());
  CHECK(winmetic::is_hermetic_mode());

  HWND fg_before = GetForegroundWindow();

  TestWindow w(false, true);
  w.paint_test_color = true;
  SetWindowLongPtrW(w.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(w.hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
  SetWindowPos(w.hwnd, HWND_BOTTOM, -16000, -16000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
  REQUIRE(w.hwnd != nullptr);

  winmetic::focus_window(w.hwnd);
  winmetic::move_to(w.hwnd, {100.0f, 100.0f});
  winmetic::click(w.hwnd, {100.0f, 100.0f});

  HWND viewer = winmetic::watch_viewer_hwnd();
  REQUIRE(viewer != nullptr);
  CHECK(IsWindow(viewer));
  CHECK(IsWindowVisible(viewer));

  RECT vr{};
  REQUIRE(GetWindowRect(viewer, &vr));
  CHECK(vr.left == 80);
  CHECK(vr.top == 60);

  const LONG_PTR ex = GetWindowLongPtrW(viewer, GWL_EXSTYLE);
  CHECK((ex & WS_EX_NOACTIVATE) != 0);
  CHECK((ex & WS_EX_TOPMOST) != 0);

  wchar_t title[128]{};
  GetWindowTextW(viewer, title, 128);
  CHECK(std::wstring(title).find(L"Winmetic Hermetic Live Preview") != std::wstring::npos);

  // Viewer window must NEVER steal foreground focus
  CHECK(GetForegroundWindow() == fg_before);
  CHECK(GetForegroundWindow() != viewer);

  winmetic::set_watch_mode(false);
  CHECK_FALSE(winmetic::is_watch_mode());
  std::cout << " PASSED\n";
}

void test_sandbox_config()
{
  std::cout << "[Test 6/6] Windows Sandbox Profile Generator...";
  winmetic::SandboxConfig cfg;
  cfg.vgpu = winmetic::SandboxToggle::Enable;
  cfg.networking = winmetic::SandboxToggle::Disable;
  cfg.memory_in_mb = 4096;
  cfg.mapped_folders.push_back({
      .host_folder = "M:\\repos\\atlas",
      .sandbox_folder = "C:\\work",
      .read_only = true
  });
  cfg.logon_command = "cmd.exe /c echo hello";

  std::string xml = cfg.to_xml();
  CHECK(xml.find("<VGpu>Enable</VGpu>") != std::string::npos);
  CHECK(xml.find("<Networking>Disable</Networking>") != std::string::npos);
  CHECK(xml.find("<MemoryInMB>4096</MemoryInMB>") != std::string::npos);
  CHECK(xml.find("<HostFolder>M:\\repos\\atlas</HostFolder>") != std::string::npos);
  CHECK(xml.find("<ReadOnly>true</ReadOnly>") != std::string::npos);
  CHECK(xml.find("<Command>cmd.exe /c echo hello</Command>") != std::string::npos);

  std::cout << " PASSED\n";
}

int main()
{
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  std::cout << "============================================================\n";
  std::cout << "=== Running Winmetic Production-Grade C++20 Test Suite   ===\n";
  std::cout << "============================================================\n";

  test_platform_seam_and_clipboard();
  test_hidden_desktop_scope();
  test_hermetic_driver_operations();
  test_screenshot_on_hidden_window();
  test_watch_mode_preview_window();
  test_sandbox_config();

  if (g_failures == 0)
  {
    std::cout << "\n>>> ALL 6 WINMETIC TEST SUITES PASSED SUCCESSFULLY! (0 failures) <<<\n";
    return 0;
  }
  else
  {
    std::cerr << "\n>>> " << g_failures << " WINMETIC TESTS FAILED! <<<\n";
    return 1;
  }
}
