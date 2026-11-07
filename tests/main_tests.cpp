#include <winmetic/winmetic.hpp>
#include <iostream>
#include <cassert>
#include <vector>

// Mock window procedure to capture messages during tests
static std::vector<UINT> g_received_messages;
static std::vector<WPARAM> g_received_wparams;
static std::vector<LPARAM> g_received_lparams;

LRESULT CALLBACK MockTestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    g_received_messages.push_back(msg);
    g_received_wparams.push_back(wp);
    g_received_lparams.push_back(lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void test_seam_and_clipboard() {
    std::cout << "[Test] PlatformSeam & Virtual Clipboard...";
    winmetic::PlatformSeam seam;

    // Pointer
    seam.set_cursor_pos({ 150, 250 });
    assert(seam.cursor_pos().x == 150);
    assert(seam.cursor_pos().y == 250);

    // Modifiers
    assert(!seam.is_modifier_down(VK_CONTROL));
    seam.set_modifier(VK_CONTROL, true);
    assert(seam.is_modifier_down(VK_CONTROL));
    assert(seam.wparam_mouse_flags() & MK_CONTROL);

    // Clipboard
    auto& clip = seam.clipboard();
    assert(!clip.has_text());
    clip.set_text(L"Hermetic Test Data");
    assert(clip.has_text());
    assert(clip.get_text() == L"Hermetic Test Data");
    clip.clear();
    assert(!clip.has_text());

    std::cout << " PASSED\n";
}

void test_driver_synthetic_input() {
    std::cout << "[Test] Driver Synthetic Input Dispatch (Zero SendInput)...";

    // Register a simple test window class
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MockTestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinmeticMockWindowClass";
    RegisterClassExW(&wc);

    // Create a hidden window with WS_EX_NOACTIVATE
    HWND hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"Winmetic Test",
        WS_POPUP,
        0, 0, 400, 300,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    assert(hwnd != nullptr);

    winmetic::PlatformSeam seam;
    winmetic::Driver driver(hwnd, &seam);

    g_received_messages.clear();

    // Dispatch pointer move and click
    driver.move_to({ 50, 100 });
    driver.click(winmetic::MouseButton::Left);
    driver.key(VK_RETURN);
    driver.type_text(L"A");

    // Verify received messages
    bool found_move = false;
    bool found_down = false;
    bool found_up = false;
    bool found_key = false;
    bool found_char = false;

    for (size_t i = 0; i < g_received_messages.size(); ++i) {
        if (g_received_messages[i] == WM_MOUSEMOVE) found_move = true;
        if (g_received_messages[i] == WM_LBUTTONDOWN) found_down = true;
        if (g_received_messages[i] == WM_LBUTTONUP) found_up = true;
        if (g_received_messages[i] == WM_KEYDOWN && g_received_wparams[i] == VK_RETURN) found_key = true;
        if (g_received_messages[i] == WM_CHAR && g_received_wparams[i] == L'A') found_char = true;
    }

    assert(found_move);
    assert(found_down);
    assert(found_up);
    assert(found_key);
    assert(found_char);

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    std::cout << " PASSED\n";
}

void test_hidden_desktop() {
    std::cout << "[Test] Hidden Desktop Station...";
    try {
        winmetic::HiddenDesktop desktop(L"WinmeticUnitTestDesktop");
        assert(desktop.handle() != nullptr);
        bool attached = desktop.attach();
        (void)attached;
        assert(attached);
        desktop.restore();
        std::cout << " PASSED\n";
    } catch (const std::exception& ex) {
        std::cout << " FAILED (" << ex.what() << ")\n";
        assert(false);
    }
}

void test_sandbox_config() {
    std::cout << "[Test] Windows Sandbox Profile Generator...";
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
    assert(xml.find("<VGpu>Enable</VGpu>") != std::string::npos);
    assert(xml.find("<Networking>Disable</Networking>") != std::string::npos);
    assert(xml.find("<MemoryInMB>4096</MemoryInMB>") != std::string::npos);
    assert(xml.find("<HostFolder>M:\\repos\\atlas</HostFolder>") != std::string::npos);
    assert(xml.find("<ReadOnly>true</ReadOnly>") != std::string::npos);
    assert(xml.find("<Command>cmd.exe /c echo hello</Command>") != std::string::npos);

    std::cout << " PASSED\n";
}

int main() {
    std::cout << "=== Running Winmetic Test Suite ===\n";
    test_seam_and_clipboard();
    test_driver_synthetic_input();
    test_hidden_desktop();
    test_sandbox_config();
    std::cout << "=== All Winmetic Tests Passed Successfully! ===\n";
    return 0;
}
