#include "progress.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace progress {

namespace {

constexpr wchar_t kClassName[] = L"UE4SSUpdaterProgressWnd";
constexpr int kWidth = 440;
constexpr int kHeight = 150;
constexpr int kBarMargin = 24;
constexpr int kBarHeight = 22;

HWND g_wnd = nullptr;
std::wstring g_status = L"";
int g_percent = -1;      // -1 = indeterminate
int g_marquee = 0;       // animation phase for indeterminate bar
HFONT g_font = nullptr;
HFONT g_fontBold = nullptr;

void paint(HDC hdc, const RECT& rc)
{
    // Background.
    HBRUSH bg = CreateSolidBrush(RGB(32, 34, 37));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);

    // Title line.
    SelectObject(hdc, g_fontBold);
    SetTextColor(hdc, RGB(235, 235, 235));
    RECT titleRc{kBarMargin, 18, rc.right - kBarMargin, 42};
    DrawTextW(hdc, L"UE4SS Updater", -1, &titleRc, DT_LEFT | DT_SINGLELINE);

    // Status line.
    SelectObject(hdc, g_font);
    SetTextColor(hdc, RGB(190, 192, 195));
    RECT statusRc{kBarMargin, 48, rc.right - kBarMargin, 72};
    DrawTextW(hdc, g_status.c_str(), -1, &statusRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Progress bar track.
    RECT bar{kBarMargin, kHeight - kBarMargin - kBarHeight, rc.right - kBarMargin, kHeight - kBarMargin};
    HBRUSH track = CreateSolidBrush(RGB(20, 21, 23));
    FillRect(hdc, &bar, track);
    DeleteObject(track);
    HBRUSH border = CreateSolidBrush(RGB(60, 62, 66));
    FrameRect(hdc, &bar, border);
    DeleteObject(border);

    // Fill.
    HBRUSH fill = CreateSolidBrush(RGB(88, 166, 255));
    RECT inner = bar;
    InflateRect(&inner, -2, -2);
    int trackW = inner.right - inner.left;
    if (g_percent >= 0)
    {
        int w = trackW * (g_percent > 100 ? 100 : g_percent) / 100;
        RECT f{inner.left, inner.top, inner.left + w, inner.bottom};
        FillRect(hdc, &f, fill);
    }
    else
    {
        // indeterminate: a moving chunk
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
        // double buffer to avoid flicker
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
    case WM_CLOSE:
        // ignore user close: the updater controls the lifetime
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void show(const std::wstring& title)
{
    if (g_wnd) return;

    HINSTANCE hinst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontBold = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - kWidth) / 2;
    int y = (sh - kHeight) / 2;

    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName,
                            title.empty() ? L"UE4SS Updater" : title.c_str(),
                            WS_POPUP | WS_BORDER, x, y, kWidth, kHeight,
                            nullptr, nullptr, hinst, nullptr);
    if (g_wnd)
    {
        ShowWindow(g_wnd, SW_SHOW);
        UpdateWindow(g_wnd);
    }
    pump();
}

void set_status(const std::wstring& text)
{
    g_status = text;
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
    pump();
}

void set_progress(int percent)
{
    g_percent = percent;
    if (percent < 0) g_marquee += 12;
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
    pump();
}

void pump()
{
    if (!g_wnd) return;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void close()
{
    if (g_wnd)
    {
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = nullptr; }
}

} // namespace progress
