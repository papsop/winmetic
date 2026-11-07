#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winmetic/driver.hpp>
#include <winmetic/platform_seam.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dwmapi.h>
#include <exception>
#include <fstream>
#include <new>
#include <shellscalingapi.h>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <wincodec.h>
#include <wrl/client.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace winmetic
{

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{

DriverMode g_driver_mode = DriverMode::Physical;
bool g_watch_mode = false;
uint32_t g_hermetic_mouse_buttons = 0;
HWND g_hermetic_current_hwnd = nullptr;

FailureHandler g_fatal_handler = nullptr;
FailureHandler g_check_fail_handler = nullptr;

bool get_env_flag(const char* name1, const char* name2 = nullptr) noexcept
{
  char val[8]{};
  if (GetEnvironmentVariableA(name1, val, sizeof(val)) > 0 && val[0] == '1')
    return true;
  if (name2 && GetEnvironmentVariableA(name2, val, sizeof(val)) > 0 && val[0] == '1')
    return true;
  return false;
}

std::wstring get_env_wstring(const wchar_t* name1, const wchar_t* name2 = nullptr)
{
  wchar_t buf[1024]{};
  DWORD n = GetEnvironmentVariableW(name1, buf, 1024);
  if (n > 0 && n < 1024)
    return std::wstring(buf, n);
  if (name2)
  {
    n = GetEnvironmentVariableW(name2, buf, 1024);
    if (n > 0 && n < 1024)
      return std::wstring(buf, n);
  }
  return {};
}

bool gaming_safe_mode_active() noexcept
{
  if (get_env_flag("WINMETIC_GAMING_SAFE", "ATLAS_GAMING_SAFE"))
    return true;
  std::error_code ec;
  return std::filesystem::exists("build/.gaming-safe", ec) ||
         std::filesystem::exists("../../../.gaming-safe", ec) ||
         std::filesystem::exists("../../../../build/.gaming-safe", ec);
}

class HermeticWatchViewer
{
public:
  static HermeticWatchViewer& instance()
  {
    static HermeticWatchViewer s_viewer;
    return s_viewer;
  }

  void update(HWND target_hwnd, std::wstring_view status_label);
  void shutdown();
  HWND hwnd() const noexcept
  {
    return viewer_hwnd_;
  }

private:
  HermeticWatchViewer() = default;
  ~HermeticWatchViewer()
  {
    shutdown();
  }

  void ensure_window();
  static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  HWND viewer_hwnd_ = nullptr;
  HWND last_target_hwnd_ = nullptr;
  RECT last_target_rc_{};
  HDC cache_dc_ = nullptr;
  HBITMAP cache_bitmap_ = nullptr;
  HGDIOBJ old_cache_bitmap_ = nullptr;
  int cache_w_ = 0;
  int cache_h_ = 0;
};

void HermeticWatchViewer::ensure_window()
{
  if (viewer_hwnd_ && IsWindow(viewer_hwnd_))
    return;

  static bool class_registered = false;
  if (!class_registered)
  {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = &HermeticWatchViewer::wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AtlasHermeticWatchWindow";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);
    class_registered = true;
  }

  RECT wr{0, 0, 1120, 720};
  AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOPMOST | WS_EX_NOACTIVATE);
  const int win_w = wr.right - wr.left;
  const int win_h = wr.bottom - wr.top;

  viewer_hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"AtlasHermeticWatchWindow",
                                 L"Atlas Hermetic Live Preview \u2014 Virtual Input Monitor (Zero Focus Theft)",
                                 WS_OVERLAPPEDWINDOW, 80, 60, win_w, win_h, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);

  if (viewer_hwnd_)
  {
    ShowWindow(viewer_hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(viewer_hwnd_, HWND_TOPMOST, 80, 60, win_w, win_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(viewer_hwnd_);
  }
}

LRESULT CALLBACK HermeticWatchViewer::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
  {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    auto& v = HermeticWatchViewer::instance();
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int vw = cr.right - cr.left;
    const int vh = cr.bottom - cr.top;
    if (v.cache_dc_ && v.cache_w_ > 0 && v.cache_h_ > 0 && vw > 0 && vh > 0)
    {
      const float aspect_src = static_cast<float>(v.cache_w_) / static_cast<float>(v.cache_h_);
      const float aspect_dst = static_cast<float>(vw) / static_cast<float>(vh);
      int dst_x = 0, dst_y = 0, dst_w = vw, dst_h = vh;
      if (aspect_dst > aspect_src)
      {
        dst_h = vh;
        dst_w = static_cast<int>(std::round(vh * aspect_src));
        dst_x = (vw - dst_w) / 2;
      }
      else
      {
        dst_w = vw;
        dst_h = static_cast<int>(std::round(vw / aspect_src));
        dst_y = (vh - dst_h) / 2;
      }
      if (dst_x > 0)
      {
        RECT r_left{0, 0, dst_x, vh};
        RECT r_right{dst_x + dst_w, 0, vw, vh};
        FillRect(hdc, &r_left, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        FillRect(hdc, &r_right, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      }
      if (dst_y > 0)
      {
        RECT r_top{0, 0, vw, dst_y};
        RECT r_bottom{0, dst_y + dst_h, vw, vh};
        FillRect(hdc, &r_top, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        FillRect(hdc, &r_bottom, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      }
      SetStretchBltMode(hdc, HALFTONE);
      SetBrushOrgEx(hdc, 0, 0, nullptr);
      StretchBlt(hdc, dst_x, dst_y, dst_w, dst_h, v.cache_dc_, 0, 0, v.cache_w_, v.cache_h_, SRCCOPY);
    }
    else
    {
      FillRect(hdc, &cr, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_DESTROY:
  {
    auto& v = HermeticWatchViewer::instance();
    v.viewer_hwnd_ = nullptr;
    return 0;
  }
  default:
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

void HermeticWatchViewer::shutdown()
{
  if (viewer_hwnd_ && IsWindow(viewer_hwnd_))
  {
    DestroyWindow(viewer_hwnd_);
    viewer_hwnd_ = nullptr;
  }
  last_target_hwnd_ = nullptr;
  last_target_rc_ = RECT{};
  if (cache_dc_)
  {
    if (old_cache_bitmap_)
      SelectObject(cache_dc_, old_cache_bitmap_);
    if (cache_bitmap_)
      DeleteObject(cache_bitmap_);
    DeleteDC(cache_dc_);
    cache_dc_ = nullptr;
    cache_bitmap_ = nullptr;
    old_cache_bitmap_ = nullptr;
    cache_w_ = 0;
    cache_h_ = 0;
  }
}

void HermeticWatchViewer::update(HWND target_hwnd, std::wstring_view status_label)
{
  if (!is_watch_mode())
    return;

  HWND capture_target = nullptr;
  if (target_hwnd && IsWindow(target_hwnd))
    capture_target = target_hwnd;
  else if (global_platform_seam().active_hwnd && IsWindow(global_platform_seam().active_hwnd))
    capture_target = global_platform_seam().active_hwnd;
  else if (g_hermetic_current_hwnd && IsWindow(g_hermetic_current_hwnd))
    capture_target = g_hermetic_current_hwnd;
  else if (global_platform_seam().focused_hwnd && IsWindow(global_platform_seam().focused_hwnd))
    capture_target = global_platform_seam().focused_hwnd;
  else if (last_target_hwnd_ && IsWindow(last_target_hwnd_))
    capture_target = last_target_hwnd_;

  if (capture_target)
  {
    HWND root = GetAncestor(capture_target, GA_ROOT);
    capture_target = root ? root : capture_target;
    last_target_hwnd_ = capture_target;
  }

  ensure_window();
  if (!viewer_hwnd_ || !IsWindow(viewer_hwnd_))
    return;

  RECT target_rc{};
  if (capture_target && GetWindowRect(capture_target, &target_rc))
  {
    last_target_rc_ = target_rc;
    const int tw = target_rc.right - target_rc.left;
    const int th = target_rc.bottom - target_rc.top;
    if (tw > 0 && th > 0)
    {
      if (cache_dc_ && (cache_w_ != tw || cache_h_ != th))
      {
        if (old_cache_bitmap_)
          SelectObject(cache_dc_, old_cache_bitmap_);
        if (cache_bitmap_)
          DeleteObject(cache_bitmap_);
        DeleteDC(cache_dc_);
        cache_dc_ = nullptr;
        cache_bitmap_ = nullptr;
        old_cache_bitmap_ = nullptr;
        cache_w_ = 0;
        cache_h_ = 0;
      }

      if (!cache_dc_)
      {
        HDC screen_dc = GetDC(nullptr);
        cache_dc_ = CreateCompatibleDC(screen_dc);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = tw;
        bmi.bmiHeader.biHeight = -th; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        cache_bitmap_ = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen_dc);
        if (!cache_bitmap_ || !cache_dc_)
          return;
        old_cache_bitmap_ = SelectObject(cache_dc_, cache_bitmap_);
        cache_w_ = tw;
        cache_h_ = th;
      }

      HBRUSH bg_brush = reinterpret_cast<HBRUSH>(GetClassLongPtrW(capture_target, GCLP_HBRBACKGROUND));
      RECT full_rc{0, 0, tw, th};
      if (bg_brush && reinterpret_cast<UINT_PTR>(bg_brush) <= 31)
        bg_brush = GetSysColorBrush(static_cast<int>(reinterpret_cast<UINT_PTR>(bg_brush) - 1));
      if (!bg_brush)
        bg_brush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
      FillRect(cache_dc_, &full_rc, bg_brush);

      BOOL rendered = PrintWindow(capture_target, cache_dc_, PW_RENDERFULLCONTENT);
      bool has_content = false;
      DIBSECTION ds{};
      if (rendered && GetObject(cache_bitmap_, sizeof(ds), &ds) && ds.dsBm.bmBits)
      {
        const auto* px = static_cast<const uint32_t*>(ds.dsBm.bmBits);
        const size_t total_px = static_cast<size_t>(tw) * th;
        for (size_t i = 0; i < total_px; ++i)
        {
          if ((px[i] & 0x00FFFFFF) != 0)
          {
            has_content = true;
            break;
          }
        }
      }
      if (!has_content)
      {
        rendered = PrintWindow(capture_target, cache_dc_, 0);
        if (rendered && GetObject(cache_bitmap_, sizeof(ds), &ds) && ds.dsBm.bmBits)
        {
          const auto* px = static_cast<const uint32_t*>(ds.dsBm.bmBits);
          const size_t total_px = static_cast<size_t>(tw) * th;
          for (size_t i = 0; i < total_px; ++i)
          {
            if ((px[i] & 0x00FFFFFF) != 0)
            {
              has_content = true;
              break;
            }
          }
        }
      }
      if (!has_content)
      {
        SendMessageW(capture_target, WM_PRINT, reinterpret_cast<WPARAM>(cache_dc_),
                     PRF_CHILDREN | PRF_CLIENT | PRF_ERASEBKGND | PRF_NONCLIENT);
      }
    }
  }
  else
  {
    target_rc = last_target_rc_;
  }

  if (!cache_dc_)
  {
    const int tw = 1120, th = 720;
    HDC screen_dc = GetDC(nullptr);
    cache_dc_ = CreateCompatibleDC(screen_dc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tw;
    bmi.bmiHeader.biHeight = -th; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    cache_bitmap_ = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen_dc);
    if (!cache_bitmap_ || !cache_dc_)
      return;
    old_cache_bitmap_ = SelectObject(cache_dc_, cache_bitmap_);
    cache_w_ = tw;
    cache_h_ = th;
    RECT full_rc{0, 0, tw, th};
    FillRect(cache_dc_, &full_rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  }

  const int tw = cache_w_;
  const int th = cache_h_;

  // Draw overlay at virtual cursor coordinates
  const POINT virt_screen = global_platform_seam().virtual_cursor_screen;
  const int cx = virt_screen.x - target_rc.left;
  const int cy = virt_screen.y - target_rc.top;
  const bool cursor_in_bounds = capture_target && (cx >= -16 && cx <= tw + 16 && cy >= -16 && cy <= th + 16);

  const bool button_held = (g_hermetic_mouse_buttons != 0);

  if (cursor_in_bounds)
  {
    const int radius = 8;
    const COLORREF cursor_color = button_held ? RGB(255, 45, 45) : RGB(0, 230, 255);
    HPEN cursor_pen = CreatePen(PS_SOLID, 2, cursor_color);
    HGDIOBJ old_pen = SelectObject(cache_dc_, cursor_pen);
    HGDIOBJ old_brush = SelectObject(cache_dc_, GetStockObject(HOLLOW_BRUSH));

    // Ring
    Ellipse(cache_dc_, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);

    // Filled center when button is held
    if (button_held)
    {
      HBRUSH fill_brush = CreateSolidBrush(RGB(255, 60, 60));
      SelectObject(cache_dc_, fill_brush);
      Ellipse(cache_dc_, cx - 4, cy - 4, cx + 5, cy + 5);
      SelectObject(cache_dc_, GetStockObject(HOLLOW_BRUSH));
      DeleteObject(fill_brush);
    }

    // Crosshairs
    MoveToEx(cache_dc_, cx - radius - 4, cy, nullptr);
    LineTo(cache_dc_, cx + radius + 5, cy);
    MoveToEx(cache_dc_, cx, cy - radius - 4, nullptr);
    LineTo(cache_dc_, cx, cy + radius + 5);

    SelectObject(cache_dc_, old_brush);
    SelectObject(cache_dc_, old_pen);
    DeleteObject(cursor_pen);
  }

  // Top-left HUD banner strip (24px high)
  const int hud_height = 24;
  RECT hud_rc{0, 0, tw, hud_height};
  HBRUSH hud_bg = CreateSolidBrush(RGB(22, 25, 34));
  FillRect(cache_dc_, &hud_rc, hud_bg);
  DeleteObject(hud_bg);

  HPEN hud_divider = CreatePen(PS_SOLID, 1, RGB(60, 72, 95));
  HGDIOBJ old_pen = SelectObject(cache_dc_, hud_divider);
  MoveToEx(cache_dc_, 0, hud_height - 1, nullptr);
  LineTo(cache_dc_, tw, hud_height - 1);
  SelectObject(cache_dc_, old_pen);
  DeleteObject(hud_divider);

  HFONT hud_font = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HGDIOBJ old_font = SelectObject(cache_dc_, hud_font);
  SetBkMode(cache_dc_, TRANSPARENT);
  SetTextColor(cache_dc_, RGB(235, 242, 255));

  std::wstring hud_str = L"  [LIVE PREVIEW]  ";
  if (!status_label.empty())
  {
    hud_str.append(status_label);
    hud_str += L"  |  ";
  }
  if (cursor_in_bounds && capture_target)
  {
    hud_str += L"Cursor: (" + std::to_wstring(cx) + L", " + std::to_wstring(cy) + L")";

    const Point client_dip = to_client_dip(capture_target, virt_screen);
    wchar_t dip_buf[64];
    swprintf_s(dip_buf, L" [DIP %.1f, %.1f]", client_dip.x, client_dip.y);
    hud_str += dip_buf;
  }
  else
  {
    hud_str += L"Cursor: (standby)";
  }

  if (global_platform_seam().virtual_modifiers != 0)
  {
    hud_str += L"  |  Mod:";
    if (global_platform_seam().is_modifier_down(VK_CONTROL))
      hud_str += L" Ctrl";
    if (global_platform_seam().is_modifier_down(VK_SHIFT))
      hud_str += L" Shift";
    if (global_platform_seam().is_modifier_down(VK_MENU))
      hud_str += L" Alt";
  }
  if (button_held)
  {
    hud_str += L"  [MOUSE DOWN]";
  }

  RECT text_rc{8, 1, tw - 8, hud_height - 1};
  DrawTextW(cache_dc_, hud_str.c_str(), -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectObject(cache_dc_, old_font);
  DeleteObject(hud_font);

  InvalidateRect(viewer_hwnd_, nullptr, FALSE);
  UpdateWindow(viewer_hwnd_);

  // Pause per visual step while pumping viewer messages so human can watch
  // If WINMETIC_WATCH_NO_HOLD=1 or ATLAS_WATCH_NO_HOLD=1, use 1ms deadline for automated fast runs
  const bool no_hold = get_env_flag("WINMETIC_WATCH_NO_HOLD", "ATLAS_WATCH_NO_HOLD");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(no_hold ? 1 : 65);
  while (true)
  {
    MSG msg;
    while (PeekMessageW(&msg, viewer_hwnd_, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (std::chrono::steady_clock::now() >= deadline)
      break;
    Sleep(1);
  }
}

} // namespace

void set_fatal_handler(FailureHandler fn) noexcept
{
  g_fatal_handler = fn;
}

void set_check_fail_handler(FailureHandler fn) noexcept
{
  g_check_fail_handler = fn;
}

void set_driver_mode(DriverMode mode) noexcept
{
  g_driver_mode = mode;
  global_platform_seam().hermetic = (mode == DriverMode::Hermetic);
}

DriverMode driver_mode() noexcept
{
  return g_driver_mode;
}

void set_watch_mode(bool enabled) noexcept
{
  g_watch_mode = enabled;
  if (enabled)
    set_driver_mode(DriverMode::Hermetic);
}

bool is_watch_mode() noexcept
{
  if (g_watch_mode)
    return true;
  if (get_env_flag("WINMETIC_WATCH_UX", "ATLAS_WATCH_UX"))
    return true;
  return false;
}

void update_watch_viewer(HWND target_hwnd, std::wstring_view status_label)
{
  if (is_watch_mode())
    HermeticWatchViewer::instance().update(target_hwnd, status_label);
}

HWND watch_viewer_hwnd() noexcept
{
  return HermeticWatchViewer::instance().hwnd();
}

bool is_hermetic_mode() noexcept
{
  if (g_driver_mode == DriverMode::Hermetic)
    return true;
  if (g_watch_mode)
    return true;
  if (gaming_safe_mode_active())
    return true;
  if (get_env_flag("WINMETIC_HERMETIC_UX", "ATLAS_HERMETIC_UX"))
    return true;
  if (get_env_flag("WINMETIC_WATCH_UX", "ATLAS_WATCH_UX"))
    return true;
  return false;
}

namespace
{

WPARAM hermetic_mouse_wparam(uint32_t button_flags = 0) noexcept
{
  auto& seam = global_platform_seam();
  WPARAM wp = button_flags;
  if (seam.is_modifier_down(VK_CONTROL))
    wp |= MK_CONTROL;
  if (seam.is_modifier_down(VK_SHIFT))
    wp |= MK_SHIFT;
  return wp;
}

constexpr int kSettleMs = 12; // message pump after each injected event

struct MessageCounts
{
  uint64_t paints = 0;
  uint64_t timers = 0;
  uint64_t quits = 0;
};
thread_local MessageCounts message_counts;

void fatal(const std::string& message)
{
  if (g_fatal_handler)
    g_fatal_handler(message);
  throw std::runtime_error(message);
}

std::string narrow(std::wstring_view w)
{
  if (w.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::string describe_window(HWND w)
{
  if (!w)
    return "no window";
  wchar_t cls[96] = {}, title[96] = {};
  GetClassNameW(w, cls, 96);
  GetWindowTextW(w, title, 96);
  return "'" + narrow(title) + "' (" + narrow(cls) + ")";
}

HWND root_owner(HWND w)
{
  return w ? GetAncestor(w, GA_ROOTOWNER) : nullptr;
}

void drain_messages()
{
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
  {
    if (msg.message == WM_QUIT)
    {
      ++message_counts.quits;
      continue;
    }
    if (msg.message == WM_PAINT)
      ++message_counts.paints;
    else if (msg.message == WM_TIMER)
      ++message_counts.timers;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

void send_input(INPUT* inputs, UINT count)
{
  if (get_env_flag("WINMETIC_GAMING_SAFE", "ATLAS_GAMING_SAFE"))
    fatal("SendInput is blocked by WINMETIC_GAMING_SAFE=1 or ATLAS_GAMING_SAFE=1");
  if (is_hermetic_mode())
    fatal("SendInput called while in Hermetic mode");
  if (SendInput(count, inputs, sizeof(INPUT)) != count)
    fatal("SendInput was rejected (error " + std::to_string(GetLastError()) +
          "): input cannot reach the desktop, e.g. a locked session or a higher-integrity foreground window");
}

// ---- mouse ------------------------------------------------------------------
void send_mouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0)
{
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = dx;
  in.mi.dy = dy;
  in.mi.mouseData = data;
  in.mi.dwFlags = flags;
  send_input(&in, 1);
}

LONG normalise(long rel, long extent)
{
  const long long n = (static_cast<long long>(rel) * 65536 + extent - 1) / extent;
  return static_cast<LONG>(std::clamp<long long>(n, 0, 65535));
}

void move_screen(POINT px)
{
  const long vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const long vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (px.x < vx || px.y < vy || px.x >= vx + vw || px.y >= vy + vh)
    fatal("target (" + std::to_string(px.x) + "," + std::to_string(px.y) + ") is outside the virtual screen");
  send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, normalise(px.x - vx, vw),
             normalise(px.y - vy, vh));
  pump(kSettleMs);
  POINT now{};
  GetCursorPos(&now);
  if (std::abs(now.x - px.x) > 1 || std::abs(now.y - px.y) > 1)
    fatal("cursor landed at (" + std::to_string(now.x) + "," + std::to_string(now.y) + "), wanted (" +
          std::to_string(px.x) + "," + std::to_string(px.y) + ")");
}

DWORD button_flag(Button b, bool up)
{
  switch (b)
  {
  case Button::left:
    return up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
  case Button::right:
    return up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
  case Button::middle:
    return up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
  }
  return 0;
}

void require_hit(HWND hwnd, Point at)
{
  const std::string why = check_hit(hwnd, at);
  if (!why.empty())
    fatal("refusing pointer input: " + why);
}

// ---- keyboard ---------------------------------------------------------------
struct Chord
{
  std::vector<WORD> modifiers;
  WORD vk = 0;
};

bool is_extended(WORD vk)
{
  switch (vk)
  {
  case VK_UP:
  case VK_DOWN:
  case VK_LEFT:
  case VK_RIGHT:
  case VK_HOME:
  case VK_END:
  case VK_PRIOR:
  case VK_NEXT:
  case VK_INSERT:
  case VK_DELETE:
  case VK_RCONTROL:
  case VK_RMENU:
    return true;
  default:
    return false;
  }
}

void send_vk(WORD vk, bool up)
{
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0u) | (is_extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
  send_input(&in, 1);
}

std::string lower(std::string_view s)
{
  std::string out(s);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

WORD key_vk(const std::string& token)
{
  static const std::unordered_map<std::string, int> names = {
      {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},    {"enter", VK_RETURN},   {"return", VK_RETURN},
      {"tab", VK_TAB},       {"space", VK_SPACE},   {"backspace", VK_BACK}, {"delete", VK_DELETE},
      {"del", VK_DELETE},    {"insert", VK_INSERT}, {"home", VK_HOME},      {"end", VK_END},
      {"pageup", VK_PRIOR},  {"pagedown", VK_NEXT}, {"up", VK_UP},          {"down", VK_DOWN},
      {"left", VK_LEFT},     {"right", VK_RIGHT},   {"alt", VK_MENU},       {"ctrl", VK_CONTROL},
      {"shift", VK_SHIFT}};
  const auto it = names.find(token);
  if (it != names.end())
    return static_cast<WORD>(it->second);
  if (token.size() >= 2 && token[0] == 'f')
  {
    const int n = std::atoi(token.c_str() + 1);
    if (n >= 1 && n <= 24)
      return static_cast<WORD>(VK_F1 + n - 1);
  }
  if (token.size() == 1)
  {
    const char c = token[0];
    if (c >= 'a' && c <= 'z')
      return static_cast<WORD>('A' + (c - 'a'));
    if (c >= '0' && c <= '9')
      return static_cast<WORD>(c);
    const SHORT scan = VkKeyScanW(static_cast<wchar_t>(c));
    if (scan != -1)
      return static_cast<WORD>(scan & 0xFF);
  }
  fatal("unknown key name '" + token + "'");
  return 0;
}

Chord parse_chord(std::string_view chord)
{
  Chord out;
  std::vector<std::string> tokens;
  size_t start = 0;
  while (true)
  {
    const size_t plus = chord.find('+', start);
    if (plus == std::string_view::npos)
    {
      tokens.push_back(lower(chord.substr(start)));
      break;
    }
    if (plus == start && plus + 1 == chord.size())
    {
      tokens.push_back("+");
      break;
    }
    tokens.push_back(lower(chord.substr(start, plus - start)));
    start = plus + 1;
  }
  if (tokens.empty() || tokens.back().empty())
    fatal("empty chord '" + std::string(chord) + "'");
  for (size_t i = 0; i + 1 < tokens.size(); ++i)
  {
    const WORD vk = key_vk(tokens[i]);
    if (vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU)
      fatal("'" + tokens[i] + "' is not a modifier in chord '" + std::string(chord) + "'");
    out.modifiers.push_back(vk);
  }
  out.vk = key_vk(tokens.back());
  return out;
}

void require_foreground(HWND hwnd)
{
  const std::string why = check_foreground(hwnd);
  if (!why.empty())
    fatal("refusing keyboard input: " + why);
}

void release_stuck_input()
{
  if (is_hermetic_mode())
  {
    g_hermetic_mouse_buttons = 0;
    auto& seam = global_platform_seam();
    seam.virtual_modifiers = 0;
    seam.capture_hwnd = nullptr;
    return;
  }
  static const WORD keys[] = {VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN};
  for (const WORD vk : keys)
  {
    if (!(GetAsyncKeyState(vk) & 0x8000))
      continue;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = KEYEVENTF_KEYUP | (is_extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
    SendInput(1, &in, sizeof(INPUT));
  }
  struct ButtonUp
  {
    int vk;
    DWORD up;
  };
  static const ButtonUp buttons[] = {
      {VK_LBUTTON, MOUSEEVENTF_LEFTUP}, {VK_RBUTTON, MOUSEEVENTF_RIGHTUP}, {VK_MBUTTON, MOUSEEVENTF_MIDDLEUP}};
  for (const ButtonUp& b : buttons)
  {
    if (!(GetAsyncKeyState(b.vk) & 0x8000))
      continue;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = b.up;
    SendInput(1, &in, sizeof(INPUT));
  }
}

struct ReleaseOnExit
{
  int exceptions = std::uncaught_exceptions();
  ~ReleaseOnExit()
  {
    if (std::uncaught_exceptions() > exceptions)
      release_stuck_input();
  }
};

// ---- clipboard ---------------------------------------------------------------
HWND clipboard_owner()
{
  static HWND owner = []
  {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinmeticClipboardOwner";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  }();
  return owner;
}

bool open_clipboard()
{
  for (int i = 0; i < 40; ++i)
  {
    if (OpenClipboard(clipboard_owner()))
      return true;
    pump(10);
  }
  return false;
}

class ClipboardSnapshot final
{
public:
  struct Entry
  {
    UINT format = 0;
    STGMEDIUM medium{};
  };
  ~ClipboardSnapshot()
  {
    for (auto& entry : entries_)
      ReleaseStgMedium(&entry.medium);
  }

  static HRESULT Capture(ClipboardSnapshot** result, UINT* failed_format)
  {
    *result = nullptr;
    if (failed_format)
      *failed_format = 0;
    auto* snapshot = new (std::nothrow) ClipboardSnapshot();
    if (!snapshot)
      return E_OUTOFMEMORY;
    if (!open_clipboard())
    {
      delete snapshot;
      return CLIPBRD_E_CANT_OPEN;
    }
    HRESULT hr = S_OK;
    SetLastError(ERROR_SUCCESS);
    UINT format = EnumClipboardFormats(0);
    if (!format && GetLastError() != ERROR_SUCCESS)
      hr = HRESULT_FROM_WIN32(GetLastError());
    while (format != 0)
    {
      SetLastError(ERROR_SUCCESS);
      HANDLE source = GetClipboardData(format);
      if (!source)
      {
        const DWORD error = GetLastError();
        if (failed_format)
          *failed_format = format;
        hr = error ? HRESULT_FROM_WIN32(error) : DV_E_TYMED;
        break;
      }
      Entry entry{};
      entry.format = format;
      hr = duplicate_native_format(format, source, entry.medium);
      if (FAILED(hr) && format == CF_UNICODETEXT)
      {
        if (failed_format)
          *failed_format = format;
        if (entry.medium.tymed)
          ReleaseStgMedium(&entry.medium);
        break;
      }
      if (SUCCEEDED(hr))
        snapshot->entries_.push_back(entry);
      SetLastError(ERROR_SUCCESS);
      format = EnumClipboardFormats(format);
      if (!format && GetLastError() != ERROR_SUCCESS)
      {
        if (GetLastError() != ERROR_SUCCESS)
        {
          if (failed_format)
            *failed_format = entry.format;
          hr = HRESULT_FROM_WIN32(GetLastError());
        }
      }
      if (FAILED(hr))
        break;
    }
    CloseClipboard();
    if (FAILED(hr))
    {
      snapshot->Release();
      return hr;
    }
    *result = snapshot;
    return S_OK;
  }

  HRESULT RestoreToWin32Clipboard()
  {
    if (!open_clipboard())
      return CLIPBRD_E_CANT_OPEN;
    HRESULT restored = S_OK;
    if (!EmptyClipboard())
    {
      const DWORD error = GetLastError();
      restored = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    for (auto& entry : entries_)
    {
      if (FAILED(restored))
        break;
      HANDLE handle = nullptr;
      switch (entry.medium.tymed)
      {
      case TYMED_HGLOBAL:
        handle = entry.medium.hGlobal;
        break;
      case TYMED_GDI:
        handle = entry.medium.hBitmap;
        break;
      case TYMED_ENHMF:
        handle = entry.medium.hEnhMetaFile;
        break;
      case TYMED_MFPICT:
        handle = entry.medium.hMetaFilePict;
        break;
      default:
        restored = DV_E_TYMED;
        break;
      }
      if (SUCCEEDED(restored))
      {
        if (!SetClipboardData(entry.format, handle))
        {
          const DWORD error = GetLastError();
          restored = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
        }
        else
          entry.medium = {};
      }
    }
    CloseClipboard();
    return restored;
  }

  void Release()
  {
    delete this;
  }

private:
  static HRESULT duplicate_native_format(UINT format, HANDLE source, STGMEDIUM& output)
  {
    output = {};
    if (format == CF_OWNERDISPLAY || (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST))
      return DV_E_TYMED;
    if (format == CF_BITMAP || format == CF_DSPBITMAP)
    {
      output.tymed = TYMED_GDI;
      output.hBitmap = reinterpret_cast<HBITMAP>(OleDuplicateData(source, static_cast<CLIPFORMAT>(format), 0));
      return output.hBitmap ? S_OK : DV_E_TYMED;
    }
    if (format == CF_PALETTE)
    {
      output.tymed = TYMED_GDI;
      output.hBitmap = static_cast<HBITMAP>(OleDuplicateData(source, static_cast<CLIPFORMAT>(format), 0));
      return output.hBitmap ? S_OK : DV_E_TYMED;
    }
    if (format == CF_METAFILEPICT || format == CF_DSPMETAFILEPICT)
    {
      output.tymed = TYMED_MFPICT;
      output.hMetaFilePict = static_cast<HGLOBAL>(OleDuplicateData(source, static_cast<CLIPFORMAT>(format), 0));
      return output.hMetaFilePict ? S_OK : DV_E_TYMED;
    }
    if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE)
    {
      output.tymed = TYMED_ENHMF;
      output.hEnhMetaFile = static_cast<HENHMETAFILE>(OleDuplicateData(source, static_cast<CLIPFORMAT>(format), 0));
      return output.hEnhMetaFile ? S_OK : DV_E_TYMED;
    }
    if (format >= 0xC000)
    {
      output.tymed = TYMED_HGLOBAL;
      output.hGlobal = static_cast<HGLOBAL>(OleDuplicateData(source, static_cast<CLIPFORMAT>(format), 0));
      return output.hGlobal ? S_OK : DV_E_TYMED;
    }
    if (GetObjectType(static_cast<HGDIOBJ>(source)) != 0)
    {
      output.tymed = TYMED_GDI;
      output.hBitmap = static_cast<HBITMAP>(OleDuplicateData(source, static_cast<CLIPFORMAT>(format), 0));
      return output.hBitmap ? S_OK : DV_E_TYMED;
    }
    if (GlobalFlags(static_cast<HGLOBAL>(source)) == GMEM_INVALID_HANDLE)
      return DV_E_TYMED;
    output.tymed = TYMED_HGLOBAL;
    const SIZE_T size = GlobalSize(static_cast<HGLOBAL>(source));
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, (std::max<SIZE_T>)(size, 1));
    if (!copy)
      return E_OUTOFMEMORY;
    if (size)
    {
      const void* from = GlobalLock(static_cast<HGLOBAL>(source));
      void* to = GlobalLock(copy);
      if (!from || !to)
      {
        if (from)
          GlobalUnlock(static_cast<HGLOBAL>(source));
        if (to)
          GlobalUnlock(copy);
        GlobalFree(copy);
        return E_OUTOFMEMORY;
      }
      std::memcpy(to, from, size);
      GlobalUnlock(copy);
      GlobalUnlock(static_cast<HGLOBAL>(source));
    }
    output.hGlobal = copy;
    return S_OK;
  }

  std::vector<Entry> entries_;
};

// ---- failure capture ------------------------------------------------------------
std::vector<HWND>& watched()
{
  static std::vector<HWND> list;
  return list;
}

std::vector<fs::path>& artifacts()
{
  static std::vector<fs::path> list;
  return list;
}

std::string g_test_name = "outside-test";
int g_shot_index = 0;
unsigned g_started_cases = 0, g_skipped_cases = 0;

std::string safe_name(std::string_view s)
{
  std::string out;
  for (const char c : s)
    out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
  if (out.size() > 80)
    out.resize(80);
  return out;
}

} // namespace

void notify_test_case_start(std::string_view name) noexcept
{
  ++g_started_cases;
  g_test_name = std::string(name);
  g_shot_index = 0;
}

void notify_test_case_skipped() noexcept
{
  ++g_skipped_cases;
}

// ---- preconditions ---------------------------------------------------------------
void ensure_interactive()
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (is_hermetic_mode())
    return;
  HWINSTA station = GetProcessWindowStation();
  wchar_t name[64] = {};
  DWORD needed = 0;
  if (!GetUserObjectInformationW(station, UOI_NAME, name, sizeof(name), &needed) || std::wstring(name) != L"WinSta0")
    fatal("not an interactive window station (" + narrow(name) + "): UX tests need a logged-in desktop session");
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input)
    fatal("no input desktop (error " + std::to_string(GetLastError()) +
          "): the session is locked or showing a secure desktop; unlock it and rerun");
  CloseDesktop(input);
  POINT p{};
  if (!GetCursorPos(&p))
    fatal("cannot read the cursor position: the desktop is not accepting input");
}

std::string check_foreground(HWND hwnd)
{
  if (is_hermetic_mode())
  {
    if (!hwnd || !IsWindow(hwnd))
      return "target is not a valid window";
    const HWND active = global_platform_seam().active_hwnd;
    if (active && (root_owner(active) == root_owner(hwnd) || active == hwnd))
      return {};
    return "active hermetic window is " + describe_window(active) + ", expected " + describe_window(root_owner(hwnd));
  }
  const HWND fg = GetForegroundWindow();
  if (fg && root_owner(fg) == root_owner(hwnd))
    return {};
  return "foreground window is " + describe_window(fg) + ", expected " + describe_window(root_owner(hwnd));
}

std::string check_hit(HWND hwnd, Point at)
{
  if (is_hermetic_mode())
  {
    if (!hwnd || !IsWindow(hwnd))
      return "target is not a valid window";
    RECT rc{};
    if (!GetClientRect(hwnd, &rc))
      return "cannot get client rect for window";
    const float scale = dpi_scale(hwnd);
    const long px_x = std::lround(at.x * scale);
    const long px_y = std::lround(at.y * scale);
    if (px_x < rc.left || px_x >= rc.right || px_y < rc.top || px_y >= rc.bottom)
      return "point (" + std::to_string(at.x) + "," + std::to_string(at.y) + ") is outside window client bounds";
    return {};
  }
  const POINT px = to_screen(hwnd, at);
  const HWND under = WindowFromPoint(px);
  if (under && root_owner(under) == root_owner(hwnd))
    return {};
  return "the window at screen (" + std::to_string(px.x) + "," + std::to_string(px.y) + ") is " +
         describe_window(under) + ", not " + describe_window(root_owner(hwnd)) + " (obscured or off-screen?)";
}

void focus_window(HWND hwnd)
{
  ensure_interactive();
  if (is_hermetic_mode())
  {
    if (!hwnd || !IsWindow(hwnd))
      fatal("focus_window target is not a valid window");
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    const HWND target_root = root ? root : hwnd;
    auto& seam = global_platform_seam();
    seam.active_hwnd = target_root;
    seam.focused_hwnd = hwnd;
    SendMessageW(target_root, WM_ACTIVATE, WA_ACTIVE, 0);
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    if (is_watch_mode())
      update_watch_viewer(target_root, L"Focus Window");
    pump(kSettleMs);
    return;
  }
  const HWND root = GetAncestor(hwnd, GA_ROOT);
  if (!IsWindowVisible(root))
    ShowWindow(root, SW_SHOW);
  else if (IsIconic(root))
    ShowWindow(root, SW_RESTORE);
  SetWindowPos(root, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  auto settled = [&]
  {
    for (int i = 0; i < 20; ++i)
    {
      pump(25);
      if (check_foreground(root).empty())
        return true;
    }
    return false;
  };
  SetForegroundWindow(root);
  if (!settled())
  {
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(root);
    if (!settled())
    {
      const HWND fg = GetForegroundWindow();
      const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
      const DWORD me = GetCurrentThreadId();
      const bool attached = fg_thread && fg_thread != me && AttachThreadInput(me, fg_thread, TRUE);
      BringWindowToTop(root);
      SetForegroundWindow(root);
      if (attached)
        AttachThreadInput(me, fg_thread, FALSE);
      settled();
    }
  }
  const std::string why = check_foreground(root);
  if (!why.empty())
    fatal("could not bring the window to the foreground: " + why);
  pump(250);
  const HRESULT composition = DwmFlush();
  if (FAILED(composition))
    fatal("DWM did not finish composing the focused test window (HRESULT " +
          std::to_string(static_cast<unsigned long>(composition)) + ")");
  const std::string foreground_after_settle = check_foreground(root);
  if (!foreground_after_settle.empty())
    fatal("the test window lost foreground while settling: " + foreground_after_settle);
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const float scale = dpi_scale(hwnd);
  const std::string hit =
      check_hit(hwnd, {static_cast<float>(rc.right) / scale / 2, static_cast<float>(rc.bottom) / scale / 2});
  if (!hit.empty())
    fatal("the window is in front but not reachable by pointer input: " + hit);
}

// ---- coordinates ---------------------------------------------------------------------
float dpi_scale(HWND hwnd)
{
  const UINT dpi = GetDpiForWindow(hwnd);
  return static_cast<float>(dpi ? dpi : 96) / 96.0f;
}

POINT to_screen(HWND hwnd, Point dip)
{
  const float s = dpi_scale(hwnd);
  POINT p{static_cast<LONG>(std::lround(dip.x * s)), static_cast<LONG>(std::lround(dip.y * s))};
  ClientToScreen(hwnd, &p);
  return p;
}

Point to_client_dip(HWND hwnd, POINT screen)
{
  ScreenToClient(hwnd, &screen);
  const float s = dpi_scale(hwnd);
  return {static_cast<float>(screen.x) / s, static_cast<float>(screen.y) / s};
}

// ---- pointer ----------------------------------------------------------------------------
void move_to(HWND hwnd, Point at)
{
  if (is_hermetic_mode())
  {
    if (!hwnd || !IsWindow(hwnd))
      fatal("move_to target is not a valid window");
    const POINT screen = to_screen(hwnd, at);
    auto& seam = global_platform_seam();
    seam.virtual_cursor_screen = screen;

    if (g_hermetic_current_hwnd && g_hermetic_current_hwnd != hwnd && IsWindow(g_hermetic_current_hwnd))
    {
      SendMessageW(g_hermetic_current_hwnd, WM_MOUSELEAVE, 0, 0);
    }
    g_hermetic_current_hwnd = hwnd;

    HWND target = hwnd;
    HWND cap = GetCapture();
    if (!cap)
      cap = seam.capture_hwnd;
    POINT pt = screen;
    if (cap && IsWindow(cap))
    {
      target = cap;
      ScreenToClient(target, &pt);
    }
    else
    {
      const float s = dpi_scale(hwnd);
      pt.x = std::lround(at.x * s);
      pt.y = std::lround(at.y * s);
    }
    const LPARAM packed = MAKELPARAM(static_cast<short>(pt.x), static_cast<short>(pt.y));
    SendMessageW(target, WM_MOUSEMOVE, hermetic_mouse_wparam(g_hermetic_mouse_buttons), packed);
    if (is_watch_mode())
    {
      wchar_t buf[64];
      swprintf_s(buf, L"Move (%.1f, %.1f)", at.x, at.y);
      update_watch_viewer(target, buf);
    }
    pump(kSettleMs);
    return;
  }
  move_screen(to_screen(hwnd, at));
}

void hover_message(HWND hwnd, Point at)
{
  if (!IsWindow(hwnd))
    fatal("background hover target is not a window");
  DWORD process = 0;
  if (!GetWindowThreadProcessId(hwnd, &process) || process != GetCurrentProcessId())
    fatal("background hover target must belong to the UX test process");
  if (!std::isfinite(at.x) || !std::isfinite(at.y))
    fatal("background hover coordinates must be finite client DIPs");
  const float scale = dpi_scale(hwnd);
  const long x = std::lround(at.x * scale), y = std::lround(at.y * scale);
  if (x < -32768 || x > 32767 || y < -32768 || y > 32767)
    fatal("background hover coordinates exceed the WM_MOUSEMOVE client range");
  const LPARAM packed =
      static_cast<LPARAM>(static_cast<uint16_t>(static_cast<int16_t>(x)) |
                          (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(y))) << 16));
  SendMessageW(hwnd, WM_MOUSEMOVE, 0, packed);
  if (is_watch_mode())
  {
    auto& seam = global_platform_seam();
    seam.virtual_cursor_screen = to_screen(hwnd, at);
    seam.active_hwnd = hwnd;
    wchar_t buf[64];
    swprintf_s(buf, L"Hover: (%.1f, %.1f)", at.x, at.y);
    update_watch_viewer(hwnd, buf);
  }
}

void click_message(HWND hwnd, Point at)
{
  if (!IsWindow(hwnd))
    fatal("background click target is not a window");
  DWORD process = 0;
  if (!GetWindowThreadProcessId(hwnd, &process) || process != GetCurrentProcessId())
    fatal("background click target must belong to the UX test process");
  if (!std::isfinite(at.x) || !std::isfinite(at.y))
    fatal("background click coordinates must be finite client DIPs");
  const float scale = dpi_scale(hwnd);
  const long x = std::lround(at.x * scale), y = std::lround(at.y * scale);
  if (x < -32768 || x > 32767 || y < -32768 || y > 32767)
    fatal("background click coordinates exceed the WM_LBUTTON client range");
  const LPARAM packed =
      static_cast<LPARAM>(static_cast<uint16_t>(static_cast<int16_t>(x)) |
                          (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(y))) << 16));
  if (is_watch_mode())
  {
    auto& seam = global_platform_seam();
    seam.virtual_cursor_screen = to_screen(hwnd, at);
    seam.active_hwnd = hwnd;
  }
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, packed);
  SendMessageW(hwnd, WM_LBUTTONUP, 0, packed);
  if (is_watch_mode())
  {
    wchar_t buf[64];
    swprintf_s(buf, L"Click: (%.1f, %.1f)", at.x, at.y);
    update_watch_viewer(hwnd, buf);
  }
}

void press(HWND hwnd, Point at, Button button)
{
  require_hit(hwnd, at);
  move_to(hwnd, at);
  if (is_hermetic_mode())
  {
    UINT msg = WM_LBUTTONDOWN;
    uint32_t btn_flag = MK_LBUTTON;
    const wchar_t* btn_name = L"Left";
    switch (button)
    {
    case Button::left:
      msg = WM_LBUTTONDOWN;
      btn_flag = MK_LBUTTON;
      btn_name = L"Left";
      break;
    case Button::right:
      msg = WM_RBUTTONDOWN;
      btn_flag = MK_RBUTTON;
      btn_name = L"Right";
      break;
    case Button::middle:
      msg = WM_MBUTTONDOWN;
      btn_flag = MK_MBUTTON;
      btn_name = L"Middle";
      break;
    }
    g_hermetic_mouse_buttons |= btn_flag;

    auto& seam = global_platform_seam();
    HWND target = hwnd;
    HWND cap = GetCapture();
    if (!cap)
      cap = seam.capture_hwnd;
    POINT pt = seam.virtual_cursor_screen;
    if (cap && IsWindow(cap))
    {
      target = cap;
      ScreenToClient(target, &pt);
    }
    else
    {
      const float s = dpi_scale(hwnd);
      pt.x = std::lround(at.x * s);
      pt.y = std::lround(at.y * s);
    }
    const LPARAM packed = MAKELPARAM(static_cast<short>(pt.x), static_cast<short>(pt.y));
    SendMessageW(target, msg, hermetic_mouse_wparam(g_hermetic_mouse_buttons), packed);
    if (is_watch_mode())
    {
      wchar_t buf[64];
      swprintf_s(buf, L"Press %s (%.1f, %.1f)", btn_name, at.x, at.y);
      update_watch_viewer(target, buf);
    }
    pump(kSettleMs);
    return;
  }
  send_mouse(button_flag(button, false));
  pump(kSettleMs);
}

void release(Button button)
{
  if (is_hermetic_mode())
  {
    UINT msg = WM_LBUTTONUP;
    uint32_t btn_flag = MK_LBUTTON;
    const wchar_t* btn_name = L"Left";
    switch (button)
    {
    case Button::left:
      msg = WM_LBUTTONUP;
      btn_flag = MK_LBUTTON;
      btn_name = L"Left";
      break;
    case Button::right:
      msg = WM_RBUTTONUP;
      btn_flag = MK_RBUTTON;
      btn_name = L"Right";
      break;
    case Button::middle:
      msg = WM_MBUTTONUP;
      btn_flag = MK_MBUTTON;
      btn_name = L"Middle";
      break;
    }
    g_hermetic_mouse_buttons &= ~btn_flag;

    auto& seam = global_platform_seam();
    HWND target = GetCapture();
    if (!target)
      target = seam.capture_hwnd;
    if (!target)
      target = g_hermetic_current_hwnd;
    if (!target)
      target = seam.focused_hwnd;
    if (!target)
      target = seam.active_hwnd;

    if (target && IsWindow(target))
    {
      POINT pt = seam.virtual_cursor_screen;
      ScreenToClient(target, &pt);
      const LPARAM packed = MAKELPARAM(static_cast<short>(pt.x), static_cast<short>(pt.y));
      SendMessageW(target, msg, hermetic_mouse_wparam(g_hermetic_mouse_buttons), packed);
    }
    if (is_watch_mode())
    {
      wchar_t buf[64];
      swprintf_s(buf, L"Release %s", btn_name);
      update_watch_viewer(target, buf);
    }
    pump(kSettleMs);
    return;
  }
  send_mouse(button_flag(button, true));
  pump(kSettleMs);
}

void click(HWND hwnd, Point at, Button button)
{
  ReleaseOnExit release_on_exit;
  if (is_hermetic_mode() && is_watch_mode())
  {
    const wchar_t* btn_name = (button == Button::left) ? L"Left" : (button == Button::right ? L"Right" : L"Middle");
    wchar_t buf[64];
    swprintf_s(buf, L"Click %s (%.1f, %.1f)", btn_name, at.x, at.y);
    update_watch_viewer(hwnd, buf);
  }
  press(hwnd, at, button);
  release(button);
}

void double_click(HWND hwnd, Point at, Button button)
{
  if (is_hermetic_mode() && is_watch_mode())
  {
    wchar_t buf[64];
    swprintf_s(buf, L"Double Click (%.1f, %.1f)", at.x, at.y);
    update_watch_viewer(hwnd, buf);
  }
  click(hwnd, at, button);
  click(hwnd, at, button);
}

void drag(HWND hwnd, Point from, Point to, int steps, Button button)
{
  ReleaseOnExit release_on_exit;
  if (is_hermetic_mode() && is_watch_mode())
  {
    wchar_t buf[64];
    swprintf_s(buf, L"Drag (%.1f, %.1f) -> (%.1f, %.1f)", from.x, from.y, to.x, to.y);
    update_watch_viewer(hwnd, buf);
  }
  press(hwnd, from, button);
  steps = (std::max)(steps, 1);
  for (int i = 1; i < steps; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    move_to(hwnd, {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t});
  }
  move_to(hwnd, to);
  release(button);
}

void wheel(HWND hwnd, Point at, int notches)
{
  require_hit(hwnd, at);
  move_to(hwnd, at);
  if (is_hermetic_mode())
  {
    const POINT screen = to_screen(hwnd, at);
    const WPARAM keys = hermetic_mouse_wparam(g_hermetic_mouse_buttons);
    const WPARAM delta = static_cast<WPARAM>(static_cast<uint16_t>(notches * WHEEL_DELTA)) << 16;
    const LPARAM point = MAKELPARAM(static_cast<short>(screen.x), static_cast<short>(screen.y));
    SendMessageW(hwnd, WM_MOUSEWHEEL, keys | delta, point);
    if (is_watch_mode())
    {
      wchar_t buf[64];
      swprintf_s(buf, L"Wheel: %d notches", notches);
      update_watch_viewer(hwnd, buf);
    }
    pump(kSettleMs);
    return;
  }
  send_mouse(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(notches * WHEEL_DELTA));
  pump(kSettleMs);
}

void wheel_message(HWND hwnd, Point at, int notches, bool control, bool shift)
{
  const POINT screen = to_screen(hwnd, at);
  const WPARAM keys = (control ? MK_CONTROL : 0) | (shift ? MK_SHIFT : 0);
  const WPARAM delta = static_cast<WPARAM>(static_cast<uint16_t>(notches * WHEEL_DELTA)) << 16;
  const LPARAM point = MAKELPARAM(static_cast<short>(screen.x), static_cast<short>(screen.y));
  SendMessageW(hwnd, WM_MOUSEWHEEL, keys | delta, point);
  if (is_watch_mode())
  {
    auto& seam = global_platform_seam();
    seam.virtual_cursor_screen = screen;
    seam.active_hwnd = hwnd;
    wchar_t buf[64];
    swprintf_s(buf, L"Wheel: %d notches", notches);
    update_watch_viewer(hwnd, buf);
  }
}

void hwheel(HWND hwnd, Point at, int notches)
{
  require_hit(hwnd, at);
  move_to(hwnd, at);
  if (is_hermetic_mode())
  {
    const POINT screen = to_screen(hwnd, at);
    const WPARAM keys = hermetic_mouse_wparam(g_hermetic_mouse_buttons);
    const WPARAM delta = static_cast<WPARAM>(static_cast<uint16_t>(notches * WHEEL_DELTA)) << 16;
    const LPARAM point = MAKELPARAM(static_cast<short>(screen.x), static_cast<short>(screen.y));
    SendMessageW(hwnd, WM_MOUSEHWHEEL, keys | delta, point);
    if (is_watch_mode())
    {
      wchar_t buf[64];
      swprintf_s(buf, L"HWheel: %d notches", notches);
      update_watch_viewer(hwnd, buf);
    }
    pump(kSettleMs);
    return;
  }
  send_mouse(MOUSEEVENTF_HWHEEL, 0, 0, static_cast<DWORD>(notches * WHEEL_DELTA));
  pump(kSettleMs);
}

// ---- keyboard ---------------------------------------------------------------------------------
void key_down(HWND hwnd, std::string_view chord)
{
  require_foreground(hwnd);
  const Chord c = parse_chord(chord);
  if (is_hermetic_mode())
  {
    HWND target = hwnd ? hwnd : global_platform_seam().focused_hwnd;
    if (!target)
      target = global_platform_seam().active_hwnd;
    auto& seam = global_platform_seam();
    for (const WORD m : c.modifiers)
    {
      seam.set_modifier(m, true);
      const UINT msg = (m == VK_MENU) ? WM_SYSKEYDOWN : WM_KEYDOWN;
      if (target && IsWindow(target))
        SendMessageW(target, msg, m, 0);
    }
    seam.set_modifier(c.vk, true);
    const UINT msg = (c.vk == VK_F10 || seam.is_modifier_down(VK_MENU)) ? WM_SYSKEYDOWN : WM_KEYDOWN;
    if (target && IsWindow(target))
      SendMessageW(target, msg, c.vk, 0);
    if (is_watch_mode())
    {
      std::string s(chord);
      std::wstring ws(s.begin(), s.end());
      update_watch_viewer(target, L"Key Down: " + ws);
    }
    pump(kSettleMs);
    return;
  }
  for (const WORD m : c.modifiers)
    send_vk(m, false);
  send_vk(c.vk, false);
  pump(kSettleMs);
}

void key_up(std::string_view chord)
{
  const Chord c = parse_chord(chord);
  if (is_hermetic_mode())
  {
    HWND target = global_platform_seam().focused_hwnd;
    if (!target)
      target = global_platform_seam().active_hwnd;
    auto& seam = global_platform_seam();
    const UINT msg = (c.vk == VK_F10 || seam.is_modifier_down(VK_MENU)) ? WM_SYSKEYUP : WM_KEYUP;
    seam.set_modifier(c.vk, false);
    if (target && IsWindow(target))
      SendMessageW(target, msg, c.vk, 0);
    for (auto it = c.modifiers.rbegin(); it != c.modifiers.rend(); ++it)
    {
      seam.set_modifier(*it, false);
      const UINT m_msg = (*it == VK_MENU) ? WM_SYSKEYUP : WM_KEYUP;
      if (target && IsWindow(target))
        SendMessageW(target, m_msg, *it, 0);
    }
    if (is_watch_mode())
    {
      std::string s(chord);
      std::wstring ws(s.begin(), s.end());
      update_watch_viewer(target, L"Key Up: " + ws);
    }
    pump(kSettleMs);
    return;
  }
  send_vk(c.vk, true);
  for (auto it = c.modifiers.rbegin(); it != c.modifiers.rend(); ++it)
    send_vk(*it, true);
  pump(kSettleMs);
}

void key(HWND hwnd, std::string_view chord)
{
  ReleaseOnExit release_on_exit;
  if (is_hermetic_mode() && is_watch_mode())
  {
    HWND target = hwnd ? hwnd : global_platform_seam().focused_hwnd;
    if (!target)
      target = global_platform_seam().active_hwnd;
    std::string s(chord);
    std::wstring ws(s.begin(), s.end());
    update_watch_viewer(target, L"Key: " + ws);
  }
  key_down(hwnd, chord);
  key_up(chord);
}

void type_text(HWND hwnd, std::wstring_view text)
{
  require_foreground(hwnd);
  ReleaseOnExit release_on_exit;
  if (is_hermetic_mode())
  {
    HWND target = hwnd ? hwnd : global_platform_seam().focused_hwnd;
    if (!target)
      target = global_platform_seam().active_hwnd;
    for (size_t i = 0; i < text.size(); ++i)
    {
      const wchar_t c = text[i];
      if (c == L'\n')
      {
        key(target, "Enter");
        continue;
      }
      if (c == L'\t')
      {
        key(target, "Tab");
        continue;
      }
      if (target && IsWindow(target))
        SendMessageW(target, WM_CHAR, static_cast<WPARAM>(c), 0);
      if (is_watch_mode())
      {
        std::wstring label = L"Type: '";
        label += c;
        label += L"'";
        update_watch_viewer(target, label);
      }
      pump(kSettleMs / 2);
    }
    pump(kSettleMs);
    return;
  }
  for (size_t i = 0; i < text.size(); ++i)
  {
    const wchar_t c = text[i];
    if (c == L'\n')
    {
      key(hwnd, "Enter");
      continue;
    }
    if (c == L'\t')
    {
      key(hwnd, "Tab");
      continue;
    }
    const size_t units = (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size()) ? 2 : 1;
    INPUT in[4] = {};
    UINT n = 0;
    for (size_t u = 0; u < units; ++u)
    {
      for (const bool up : {false, true})
      {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wScan = static_cast<WORD>(text[i + u]);
        in[n].ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0u);
        ++n;
      }
    }
    send_input(in, n);
    i += units - 1;
    pump(kSettleMs / 2);
  }
  pump(kSettleMs);
}

InputGuard::InputGuard()
{
  if (is_hermetic_mode())
    return;
  have_cursor_ = GetCursorPos(&cursor_) != 0;
}

InputGuard::~InputGuard()
{
  if (is_hermetic_mode())
  {
    g_hermetic_mouse_buttons = 0;
    auto& seam = global_platform_seam();
    seam.virtual_modifiers = 0;
    seam.capture_hwnd = nullptr;
    return;
  }
  release_stuck_input();
  if (have_cursor_)
    SetCursorPos(cursor_.x, cursor_.y);
}

// ---- waiting ------------------------------------------------------------------------------------
void pump(int ms)
{
  if (is_hermetic_mode() && is_watch_mode() && global_platform_seam().active_hwnd)
  {
    static auto last_pump_watch = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pump_watch).count() >= 20)
    {
      last_pump_watch = now;
      update_watch_viewer(global_platform_seam().active_hwnd, L"Pump");
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds((std::max)(ms, 0));
  while (true)
  {
    drain_messages();
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0)
      return;
    MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>((std::min<long long>)(left.count(), 10)), QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
  }
}

std::string try_wait(const std::function<bool()>& pred, int timeout_ms, std::string_view description)
{
  const auto start = std::chrono::steady_clock::now();
  const MessageCounts before = message_counts;
  while (true)
  {
    drain_messages();
    if (pred())
      return {};
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    if (elapsed.count() >= timeout_ms)
      return "wait_until timed out after " + std::to_string(elapsed.count()) + " ms (limit " +
             std::to_string(timeout_ms) + " ms): " + std::string(description) +
             "; dispatched_paints=" + std::to_string(message_counts.paints - before.paints) +
             "; dispatched_timers=" + std::to_string(message_counts.timers - before.timers) +
             "; intercepted_quits=" + std::to_string(message_counts.quits - before.quits);
    MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms, std::string_view description)
{
  const std::string message = try_wait(pred, timeout_ms, description);
  if (message.empty())
    return true;
  if (g_check_fail_handler)
    g_check_fail_handler(message);
  else
    fatal(message);
  return false;
}

// ---- artefacts ---------------------------------------------------------------------------------------
fs::path output_dir()
{
  std::wstring env = get_env_wstring(L"WINMETIC_UX_OUT", L"ATLAS_UX_OUT");
  fs::path dir = !env.empty() ? fs::path(env) : fs::current_path() / "build" / "ux-test-output";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

static bool capture_rect(const RECT& rc, const fs::path& png);
static bool capture_window_hermetic(HWND hwnd, const fs::path& png);

bool screenshot(HWND hwnd, const fs::path& png)
{
  if (is_hermetic_mode())
    return capture_window_hermetic(hwnd, png);
  RECT rc{};
  if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || !GetWindowRect(hwnd, &rc))
    return false;
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0)
    return false;
  if (!check_foreground(hwnd).empty())
    return false;
  pump(250);
  if (FAILED(DwmFlush()))
    return false;
  const HWND root = root_owner(hwnd);
  const POINT samples[] = {{rc.left + 1, rc.top + 1},
                           {rc.right - 2, rc.top + 1},
                           {rc.left + 1, rc.bottom - 2},
                           {rc.right - 2, rc.bottom - 2},
                           {rc.left + w / 2, rc.top + h / 2}};
  for (const POINT point : samples)
  {
    const HWND under = WindowFromPoint(point);
    if (!under || root_owner(under) != root)
      return false;
  }
  return capture_rect(rc, png);
}

bool screenshot_unchecked(HWND hwnd, const fs::path& png)
{
  if (is_hermetic_mode())
    return capture_window_hermetic(hwnd, png);
  RECT rc{};
  if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || !GetWindowRect(hwnd, &rc))
    return false;
  if (FAILED(DwmFlush()))
    return false;
  return capture_rect(rc, png);
}

static bool encode_dib_to_png(const fs::path& png, int w, int h, BYTE* bits)
{
  bool ok = false;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const UINT uw = static_cast<UINT>(w), uh = static_cast<UINT>(h);
    const bool ready =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(png.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) && SUCCEEDED(frame->Initialize(props.Get())) &&
        SUCCEEDED(frame->SetSize(uw, uh)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA);
    ok = ready && SUCCEEDED(frame->WritePixels(uh, uw * 4, uw * 4 * uh, bits)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit()) && SUCCEEDED(stream->Commit(STGC_DEFAULT));
  }
  if (SUCCEEDED(init))
    CoUninitialize();
  return ok;
}

static bool capture_window_hermetic(HWND hwnd, const fs::path& png)
{
  RECT rc{};
  if (!IsWindow(hwnd) || IsIconic(hwnd) || !GetWindowRect(hwnd, &rc))
    return false;
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0)
    return false;

  HDC mem = CreateCompatibleDC(nullptr);
  if (!mem)
    return false;

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h; // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits)
  {
    DeleteDC(mem);
    return false;
  }

  HGDIOBJ old = SelectObject(mem, bitmap);

  HBRUSH bg_brush = reinterpret_cast<HBRUSH>(GetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND));
  RECT full_rc{0, 0, w, h};
  if (bg_brush && reinterpret_cast<UINT_PTR>(bg_brush) <= 31)
    bg_brush = GetSysColorBrush(static_cast<int>(reinterpret_cast<UINT_PTR>(bg_brush) - 1));
  if (bg_brush)
    FillRect(mem, &full_rc, bg_brush);

  BOOL rendered = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
  bool has_content = false;
  if (rendered)
  {
    auto* px = static_cast<const uint32_t*>(bits);
    const size_t total_px = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < total_px; ++i)
    {
      if ((px[i] & 0x00FFFFFF) != 0)
      {
        has_content = true;
        break;
      }
    }
  }

  if (!has_content)
  {
    rendered = PrintWindow(hwnd, mem, 0);
    if (rendered)
    {
      auto* px = static_cast<const uint32_t*>(bits);
      const size_t total_px = static_cast<size_t>(w) * h;
      for (size_t i = 0; i < total_px; ++i)
      {
        if ((px[i] & 0x00FFFFFF) != 0)
        {
          has_content = true;
          break;
        }
      }
    }
  }

  if (!has_content)
  {
    SendMessageW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(mem),
                 PRF_CHILDREN | PRF_CLIENT | PRF_ERASEBKGND | PRF_NONCLIENT);
  }

  auto* pixels = static_cast<BYTE*>(bits);
  for (size_t i = 3; i < static_cast<size_t>(w) * h * 4; i += 4)
    pixels[i] = 0xFF;

  const bool ok = encode_dib_to_png(png, w, h, pixels);

  SelectObject(mem, old);
  DeleteObject(bitmap);
  DeleteDC(mem);
  return ok;
}

static bool capture_rect(const RECT& rc, const fs::path& png)
{
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0)
    return false;
  bool ok = false;
  HDC screen = GetDC(nullptr);
  if (!screen)
    return false;
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h; // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bitmap && bits)
  {
    HGDIOBJ old = SelectObject(mem, bitmap);
    if (BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY))
    {
      auto* pixels = static_cast<BYTE*>(bits);
      for (size_t i = 3; i < static_cast<size_t>(w) * h * 4; i += 4)
        pixels[i] = 0xFF;
      ok = encode_dib_to_png(png, w, h, pixels);
    }
    SelectObject(mem, old);
  }
  if (bitmap)
    DeleteObject(bitmap);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok;
}

bool read_png_size(const fs::path& png, unsigned& width, unsigned& height)
{
  std::ifstream in(png, std::ios::binary);
  unsigned char h[24] = {};
  if (!in.read(reinterpret_cast<char*>(h), sizeof(h)))
    return false;
  static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (!std::equal(sig, sig + 8, h))
    return false;
  auto be = [&](int o)
  {
    return (static_cast<unsigned>(h[o]) << 24) | (static_cast<unsigned>(h[o + 1]) << 16) |
           (static_cast<unsigned>(h[o + 2]) << 8) | static_cast<unsigned>(h[o + 3]);
  };
  width = be(16);
  height = be(20);
  return true;
}

FailureCapture::FailureCapture(std::initializer_list<HWND> windows)
{
  for (const HWND w : windows)
    add(w);
}

FailureCapture::~FailureCapture()
{
  if (std::uncaught_exceptions() > 0)
  {
    try
    {
      capture_failure_screenshots("exception");
    }
    catch (...)
    {
    }
  }
  for (const HWND w : mine_)
  {
    auto& list = watched();
    const auto it = std::find(list.begin(), list.end(), w);
    if (it != list.end())
      list.erase(it);
  }
}

void FailureCapture::add(HWND hwnd)
{
  mine_.push_back(hwnd);
  watched().push_back(hwnd);
}

const std::vector<fs::path>& failure_artifacts()
{
  return artifacts();
}

bool run_had_selected_cases(bool allow_filtered) noexcept
{
  return g_started_cases > 0 && (allow_filtered || g_skipped_cases == 0);
}

void capture_failure_screenshots(std::string_view label)
{
  const int n = g_shot_index++;
  int index = 0;
  for (const HWND w : watched())
  {
    const fs::path file = output_dir() / (safe_name(g_test_name) + "-" + std::to_string(n) + "-" + safe_name(label) +
                                          "-w" + std::to_string(index++) + ".png");
    if (screenshot(w, file))
      artifacts().push_back(file);
  }
}

// ---- clipboard ----------------------------------------------------------------------------------------------
std::optional<std::wstring> read_clipboard_text()
{
  if (is_hermetic_mode())
    return global_platform_seam().get_clipboard();
  if (!open_clipboard())
    return std::nullopt;
  std::optional<std::wstring> out;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
  {
    if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data)))
    {
      out = std::wstring(text);
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return out;
}

bool set_clipboard_text(std::wstring_view text)
{
  if (is_hermetic_mode())
  {
    global_platform_seam().set_clipboard(text);
    return true;
  }
  if (!open_clipboard())
    return false;
  bool ok = false;
  EmptyClipboard();
  if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)))
  {
    if (auto* dst = static_cast<wchar_t*>(GlobalLock(mem)))
    {
      std::copy(text.begin(), text.end(), dst);
      dst[text.size()] = L'\0';
      GlobalUnlock(mem);
      ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    }
    if (!ok)
      GlobalFree(mem);
  }
  CloseClipboard();
  return ok;
}

ClipboardGuard::ClipboardGuard()
{
  if (is_hermetic_mode())
  {
    hermetic_ = true;
    saved_hermetic_clipboard_ = global_platform_seam().get_clipboard();
    return;
  }
  const HRESULT initialized = OleInitialize(nullptr);
  if (initialized == S_OK || initialized == S_FALSE)
    uninitialize_com_ = true;
  else
    throw std::runtime_error("ClipboardGuard could not initialize COM (HRESULT " +
                             std::to_string(static_cast<unsigned long>(initialized)) + ")");

  ClipboardSnapshot* owned = nullptr;
  UINT failed_format = 0;
  const HRESULT captured = ClipboardSnapshot::Capture(&owned, &failed_format);
  if (FAILED(captured) || !owned)
  {
    if (uninitialize_com_)
      OleUninitialize();
    uninitialize_com_ = false;
    throw std::runtime_error("ClipboardGuard cannot safely duplicate every clipboard format (HRESULT " +
                             std::to_string(static_cast<unsigned long>(captured)) +
                             (failed_format ? ", format ID " + std::to_string(failed_format) : std::string()) + ")");
  }
  saved_ = owned;
}

ClipboardGuard::~ClipboardGuard()
{
  if (hermetic_)
  {
    if (saved_hermetic_clipboard_.has_value())
      global_platform_seam().set_clipboard(*saved_hermetic_clipboard_);
    else
    {
      global_platform_seam().virtual_clipboard.clear();
      global_platform_seam().has_virtual_clipboard = false;
    }
    return;
  }
  if (saved_)
  {
    const HRESULT restored = static_cast<ClipboardSnapshot*>(saved_)->RestoreToWin32Clipboard();
    if (FAILED(restored))
    {
      if (g_check_fail_handler)
      {
        std::string msg = "ClipboardGuard could not restore the original clipboard formats (HRESULT " +
                          std::to_string(static_cast<unsigned long>(restored)) + ")";
        g_check_fail_handler(msg);
      }
    }
    static_cast<ClipboardSnapshot*>(saved_)->Release();
    saved_ = nullptr;
  }
  if (uninitialize_com_)
    OleUninitialize();
}

} // namespace winmetic
