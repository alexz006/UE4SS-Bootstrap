#include "progress.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mutex>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace progress {

namespace {

constexpr wchar_t kClassName[] = L"UE4SSBootstrapProgressWnd";
constexpr int kWidth = 440;
constexpr int kHeight = 150;
constexpr int kBarMargin = 24;
constexpr int kBarHeight = 22;
constexpr UINT kTimerId = 1;
constexpr UINT WM_APP_CLOSE = WM_APP + 1;

// Window state (the window runs on its own thread with a timer-driven repaint).
HANDLE g_thread = nullptr;
HWND g_wnd = nullptr;
HANDLE g_ready = nullptr;
std::wstring g_title;

std::mutex g_mx;        // guards g_status / g_percent
std::wstring g_status;
int g_percent = -1;     // -1 = indeterminate
int g_marquee = 0;      // UI-thread only
HFONT g_font = nullptr;
HFONT g_fontBold = nullptr;

void paint(HDC hdc, const RECT& rc)
{
    std::wstring status;
    int percent;
    { std::lock_guard<std::mutex> lk(g_mx); status = g_status; percent = g_percent; }

    HBRUSH bg = CreateSolidBrush(RGB(32, 34, 37));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);

    SelectObject(hdc, g_fontBold);
    SetTextColor(hdc, RGB(235, 235, 235));
    RECT titleRc{kBarMargin, 18, rc.right - kBarMargin, 42};
    DrawTextW(hdc, L"UE4SS Updater", -1, &titleRc, DT_LEFT | DT_SINGLELINE);

    SelectObject(hdc, g_font);
    SetTextColor(hdc, RGB(190, 192, 195));
    RECT statusRc{kBarMargin, 48, rc.right - kBarMargin, 72};
    DrawTextW(hdc, status.c_str(), -1, &statusRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT bar{kBarMargin, kHeight - kBarMargin - kBarHeight, rc.right - kBarMargin, kHeight - kBarMargin};
    HBRUSH track = CreateSolidBrush(RGB(20, 21, 23));
    FillRect(hdc, &bar, track);
    DeleteObject(track);
    HBRUSH border = CreateSolidBrush(RGB(60, 62, 66));
    FrameRect(hdc, &bar, border);
    DeleteObject(border);

    HBRUSH fill = CreateSolidBrush(RGB(88, 166, 255));
    RECT inner = bar;
    InflateRect(&inner, -2, -2);
    int trackW = inner.right - inner.left;
    if (percent >= 0)
    {
        int w = trackW * (percent > 100 ? 100 : percent) / 100;
        RECT f{inner.left, inner.top, inner.left + w, inner.bottom};
        FillRect(hdc, &f, fill);
    }
    else
    {
        int chunk = trackW / 4;
        int pos = (g_marquee % (trackW + chunk)) - chunk;
        int left = inner.left + (pos < 0 ? 0 : pos);
        int right = inner.left + pos + chunk;
        if (right > inner.right) right = inner.right;
        if (left < right)
        {
            RECT f{left, inner.top, right, inner.bottom};
            FillRect(hdc, &f, fill);
        }
    }
    DeleteObject(fill);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
    {
        int percent;
        { std::lock_guard<std::mutex> lk(g_mx); percent = g_percent; }
        if (percent < 0) g_marquee += 12;  // animate the indeterminate bar
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_APP_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:        // ignore user close
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Creates the window and runs its message loop.
DWORD WINAPI ui_thread(LPVOID)
{
    HINSTANCE hinst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontBold = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName,
                            g_title.empty() ? L"UE4SS Updater" : g_title.c_str(),
                            WS_POPUP | WS_BORDER, (sw - kWidth) / 2, (sh - kHeight) / 2, kWidth, kHeight,
                            nullptr, nullptr, hinst, nullptr);
    if (g_wnd)
    {
        ShowWindow(g_wnd, SW_SHOW);
        UpdateWindow(g_wnd);
        SetTimer(g_wnd, kTimerId, 50, nullptr);
    }
    if (g_ready) SetEvent(g_ready);

    MSG msg;
    while (g_wnd && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = nullptr; }
    g_wnd = nullptr;
    return 0;
}

} // namespace

void show(const std::wstring& title)
{
    if (g_thread) return;
    g_title = title;
    { std::lock_guard<std::mutex> lk(g_mx); g_status.clear(); g_percent = -1; }
    g_marquee = 0;
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, ui_thread, nullptr, 0, nullptr);
    if (g_ready) WaitForSingleObject(g_ready, 5000);  // wait until the window exists
}

void set_status(const std::wstring& text)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_status = text;
}

void set_progress(int percent)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_percent = percent;
}

void pump()
{
    // No-op: the UI thread runs its own message loop.
}

void close()
{
    if (g_wnd) PostMessageW(g_wnd, WM_APP_CLOSE, 0, 0);
    if (g_thread)
    {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_ready) { CloseHandle(g_ready); g_ready = nullptr; }
}

} // namespace progress
