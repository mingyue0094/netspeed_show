#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>

namespace {
constexpr wchar_t kClassName[] = L"NetSpeedOverlayWindow";
constexpr wchar_t kMutexName[] = L"NetSpeedShow_SingleInstance";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kRefreshMs = 1000;
constexpr UINT kMenuAuto = 1000;
constexpr UINT kMenuNicBase = 2000;
constexpr UINT kMenuPause = 4000;
constexpr UINT kMenuShow = 4001;
constexpr UINT kMenuExit = 4002;
constexpr int kWindowWidth = 360;
constexpr int kWindowHeight = 26;

constexpr COLORREF kBackground = RGB(30, 30, 30);
constexpr COLORREF kDownloadColor = RGB(74, 222, 128);
constexpr COLORREF kUploadColor = RGB(96, 165, 250);

struct Adapter {
    NET_LUID luid{};
    std::wstring name;
    bool connected = false;
};

HWND g_window = nullptr;
HFONT g_font = nullptr;
HICON g_trayIcon = nullptr;
HANDLE g_mutex = nullptr;

std::vector<Adapter> g_menuAdapters;
ULONGLONG g_selectedLuid = 0; // 0 = aggregate active physical adapters
std::wstring g_selectedName = L"自动汇总";
ULONGLONG g_lastIn = 0;
ULONGLONG g_lastOut = 0;
ULONGLONG g_lastTick = 0;
double g_downloadBytesPerSecond = 0.0;
double g_uploadBytesPerSecond = 0.0;
bool g_haveSample = false;
bool g_paused = false;
bool g_displayVisible = true;

std::vector<Adapter> EnumerateAdapters() {
    std::vector<Adapter> adapters;
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr)
        return adapters;

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        Adapter item;
        item.luid = row.InterfaceLuid;
        item.connected = row.OperStatus == IfOperStatusUp;
        item.name = row.Alias[0] ? row.Alias : row.Description;
        if (item.name.empty())
            item.name = L"未命名网卡";
        adapters.push_back(std::move(item));
    }
    FreeMibTable(table);

    std::sort(adapters.begin(), adapters.end(),
        [](const Adapter& a, const Adapter& b) {
            return a.name < b.name;
        });
    return adapters;
}

bool ReadNetworkCounters(ULONGLONG& received, ULONGLONG& sent) {
    received = 0;
    sent = 0;

    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr)
        return false;

    bool found = false;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        if (g_selectedLuid != 0) {
            if (row.InterfaceLuid.Value != g_selectedLuid)
                continue;

            found = true;
            if (row.OperStatus == IfOperStatusUp) {
                received = row.InOctets;
                sent = row.OutOctets;
            }
            break;
        }

        if (!row.HardwareInterface || row.OperStatus != IfOperStatusUp)
            continue;

        received += row.InOctets;
        sent += row.OutOctets;
        found = true;
    }

    FreeMibTable(table);
    return found;
}

void FormatSpeed(double bytesPerSecond, wchar_t* output, size_t capacity) {
    if (bytesPerSecond >= 1024.0 * 1024.0) {
        swprintf(output, capacity, L"%.2f MB/s",
            bytesPerSecond / (1024.0 * 1024.0));
    } else {
        swprintf(output, capacity, L"%.2f KB/s",
            bytesPerSecond / 1024.0);
    }
}

void ResetSampling() {
    g_haveSample = false;
    g_downloadBytesPerSecond = 0.0;
    g_uploadBytesPerSecond = 0.0;
    if (g_window)
        InvalidateRect(g_window, nullptr, TRUE);
}

void UpdateSpeed() {
    ULONGLONG in = 0;
    ULONGLONG out = 0;
    if (!ReadNetworkCounters(in, out)) {
        g_downloadBytesPerSecond = 0.0;
        g_uploadBytesPerSecond = 0.0;
        InvalidateRect(g_window, nullptr, TRUE);
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (!g_haveSample) {
        g_lastIn = in;
        g_lastOut = out;
        g_lastTick = now;
        g_haveSample = true;
        return;
    }

    const ULONGLONG elapsed = now - g_lastTick;
    if (elapsed == 0)
        return;

    const double seconds = static_cast<double>(elapsed) / 1000.0;
    g_downloadBytesPerSecond =
        in >= g_lastIn ? static_cast<double>(in - g_lastIn) / seconds : 0.0;
    g_uploadBytesPerSecond =
        out >= g_lastOut ? static_cast<double>(out - g_lastOut) / seconds : 0.0;

    g_lastIn = in;
    g_lastOut = out;
    g_lastTick = now;
    InvalidateRect(g_window, nullptr, TRUE);
}

void UpdateTrayTooltip() {
    if (!g_window)
        return;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = 1;
    data.uFlags = NIF_TIP;

    wchar_t down[64]{};
    wchar_t up[64]{};
    FormatSpeed(g_downloadBytesPerSecond, down, _countof(down));
    FormatSpeed(g_uploadBytesPerSecond, up, _countof(up));

    swprintf(data.szTip, _countof(data.szTip),
        L"%s\n下载: %s\n上传: %s",
        g_selectedName.c_str(), down, up);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void PositionDisplay() {
    if (!g_window)
        return;

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT taskRect{};
    RECT trayRect{};
    int x = GetSystemMetrics(SM_CXSCREEN) - kWindowWidth - 220;
    int y = GetSystemMetrics(SM_CYSCREEN) - kWindowHeight - 8;

    if (taskbar && GetWindowRect(taskbar, &taskRect)) {
        HWND tray = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
        if (tray && GetWindowRect(tray, &trayRect)) {
            // Typical horizontal taskbar: reserve a compact strip immediately
            // to the left of the notification area.
            x = trayRect.left - kWindowWidth - 8;
            y = taskRect.top + (taskRect.bottom - taskRect.top - kWindowHeight) / 2;
        } else {
            x = taskRect.right - kWindowWidth - 220;
            y = taskRect.top + (taskRect.bottom - taskRect.top - kWindowHeight) / 2;
        }
    }

    SetWindowPos(g_window, HWND_TOPMOST, x, y, kWindowWidth, kWindowHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PaintDisplay(HDC dc) {
    RECT client{};
    GetClientRect(g_window, &client);
    HBRUSH brush = CreateSolidBrush(kBackground);
    FillRect(dc, &client, brush);
    DeleteObject(brush);

    SetBkMode(dc, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, g_font));

    wchar_t down[64]{};
    wchar_t up[64]{};
    wchar_t downText[80]{};
    wchar_t upText[80]{};
    FormatSpeed(g_downloadBytesPerSecond, down, _countof(down));
    FormatSpeed(g_uploadBytesPerSecond, up, _countof(up));
    swprintf(downText, _countof(downText), L"\u2193 %s", down);
    swprintf(upText, _countof(upText), L"\u2191 %s", up);

    RECT left{ 8, 0, 180, kWindowHeight };
    RECT right{ 185, 0, kWindowWidth - 5, kWindowHeight };

    SetTextColor(dc, kDownloadColor);
    DrawTextW(dc, downText, -1, &left,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    SetTextColor(dc, kUploadColor);
    DrawTextW(dc, upText, -1, &right,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    SelectObject(dc, oldFont);
}

void AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = g_trayIcon;
    wcsncpy(data.szTip, L"NetSpeed", _countof(data.szTip) - 1);
    data.szTip[_countof(data.szTip) - 1] = L'\0';
    Shell_NotifyIconW(NIM_ADD, &data);

}

void RemoveTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void SelectAdapter(ULONGLONG luid, const std::wstring& name) {
    g_selectedLuid = luid;
    g_selectedName = name;
    ResetSampling(); // New interface gets a fresh baseline; no false spikes.
    UpdateTrayTooltip();
}

void AddAdapterSubmenu(HMENU root) {
    HMENU submenu = CreatePopupMenu();
    if (!submenu)
        return;

    g_menuAdapters = EnumerateAdapters();

    AppendMenuW(submenu,
        MF_STRING | (g_selectedLuid == 0 ? MF_CHECKED : 0),
        kMenuAuto, L"自动汇总活动物理网卡");
    AppendMenuW(submenu, MF_SEPARATOR, 0, nullptr);

    size_t count = 0;
    for (const Adapter& adapter : g_menuAdapters) {
        if (count >= 1900)
            break;

        const UINT id = kMenuNicBase + static_cast<UINT>(count);
        wchar_t label[512]{};
        swprintf(label, _countof(label), L"%s  [%s]",
            adapter.name.c_str(), adapter.connected ? L"已连接" : L"未连接");

        UINT flags = MF_STRING;
        if (adapter.luid.Value == g_selectedLuid)
            flags |= MF_CHECKED;
        if (!adapter.connected)
            flags |= MF_GRAYED;

        AppendMenuW(submenu, flags, id, label);
        ++count;
    }

    if (g_menuAdapters.empty())
        AppendMenuW(submenu, MF_STRING | MF_GRAYED, 0, L"未发现网络适配器");

    AppendMenuW(root, MF_POPUP | MF_STRING,
        reinterpret_cast<UINT_PTR>(submenu), L"选择网卡");
}

void ShowTrayMenu() {
    POINT point{};
    GetCursorPos(&point);

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AddAdapterSubmenu(menu);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuPause,
        g_paused ? L"继续刷新" : L"暂停刷新");
    AppendMenuW(menu, MF_STRING, kMenuShow,
        g_displayVisible ? L"隐藏网速显示" : L"显示网速显示");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

    SetForegroundWindow(g_window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
        point.x, point.y, 0, g_window, nullptr);
    PostMessageW(g_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        SetTimer(hwnd, kTimerId, kRefreshMs, nullptr);
        return 0;

    case WM_TIMER:
        if (!g_paused) {
            UpdateSpeed();
            UpdateTrayTooltip();
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        PaintDisplay(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    // The speed strip is display-only; right-click the tray icon for controls.
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case kTrayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            ShowTrayMenu();
        } else if (lParam == WM_LBUTTONUP) {
            g_displayVisible = !g_displayVisible;
            ShowWindow(hwnd, g_displayVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        return 0;

    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (id == kMenuAuto) {
            SelectAdapter(0, L"自动汇总");
            return 0;
        }

        if (id >= kMenuNicBase &&
            id < kMenuNicBase + g_menuAdapters.size()) {
            const size_t index = id - kMenuNicBase;
            const Adapter& adapter = g_menuAdapters[index];
            if (adapter.connected)
                SelectAdapter(adapter.luid.Value, adapter.name);
            return 0;
        }

        switch (id) {
        case kMenuPause:
            g_paused = !g_paused;
            ResetSampling();
            return 0;
        case kMenuShow:
            g_displayVisible = !g_displayVisible;
            ShowWindow(hwnd, g_displayVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
            return 0;
        case kMenuExit:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        PositionDisplay();
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutex)
            CloseHandle(g_mutex);
        return 0;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    if (!RegisterClassW(&wc)) {
        CloseHandle(g_mutex);
        return 1;
    }

    g_trayIcon = LoadIconW(nullptr, IDI_APPLICATION);
    g_font = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    g_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kClassName, L"NetSpeed", WS_POPUP,
        0, 0, kWindowWidth, kWindowHeight,
        nullptr, nullptr, instance, nullptr);

    if (!g_window) {
        if (g_font) DeleteObject(g_font);
        CloseHandle(g_mutex);
        return 1;
    }

    AddTrayIcon();
    PositionDisplay();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_font)
        DeleteObject(g_font);
    if (g_mutex)
        CloseHandle(g_mutex);
    return 0;
}
