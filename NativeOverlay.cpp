#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <evntrace.h>
#include <evntcons.h>
#include <iphlpapi.h>
#include <tcpestats.h>
#include <tlhelp32.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr USHORT gamePort = 13328;
constexpr UINT_PTR pollTimer = 1, graphTimer = 2;
constexpr INT reconnectId = 101, minimizeId = 102, closeId = 103, backgroundId = 104;
constexpr LONGLONG ticksPerMs = 10000;
constexpr GUID tcpProvider{0x9a280ac0, 0xc8e0, 0x11d1, {0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2}};
constexpr COLORREF background = RGB(16, 24, 35), panel = RGB(24, 36, 51);
constexpr COLORREF text = RGB(231, 240, 245), grid = RGB(53, 73, 97), line = RGB(114, 231, 222);
constexpr COLORREF action = RGB(42, 79, 99);
constexpr COLORREF transparentKey = RGB(255, 0, 255);
constexpr wchar_t settingsPath[] = L"Software\\AION2 Network Overlay\\Public";

struct Settings { LONG x, y, width, height; DWORD transparent; };

Settings ReadSettings() {
    Settings settings{};
    DWORD bytes = sizeof(settings);
    if (RegGetValueW(HKEY_CURRENT_USER, settingsPath, L"Window", RRF_RT_REG_BINARY,
        nullptr, &settings, &bytes) != ERROR_SUCCESS || bytes != sizeof(settings)) return {};
    return settings;
}

struct Socket {
    MIB_TCPROW row{};
    DWORD pid = 0;
    LONGLONG created = 0;
    explicit operator bool() const { return pid != 0; }
};

bool SameSocket(const Socket& a, const Socket& b) {
    return a.pid == b.pid && a.created == b.created && a.row.dwLocalAddr == b.row.dwLocalAddr &&
        a.row.dwLocalPort == b.row.dwLocalPort && a.row.dwRemoteAddr == b.row.dwRemoteAddr &&
        a.row.dwRemotePort == b.row.dwRemotePort;
}

Socket FindSocket() {
    std::set<DWORD> gameProcesses;
    HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (Process32FirstW(processes, &process)) {
        do {
            if (_wcsicmp(process.szExeFile, L"AION2.exe") == 0)
                gameProcesses.insert(process.th32ProcessID);
        } while (Process32NextW(processes, &process));
    }
    CloseHandle(processes);
    if (gameProcesses.empty()) return {};

    std::vector<BYTE> buffer;
    DWORD bytes = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD result = GetExtendedTcpTable(buffer.empty() ? nullptr : buffer.data(), &bytes,
            FALSE, AF_INET, TCP_TABLE_OWNER_MODULE_CONNECTIONS, 0);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            buffer.resize(bytes);
            continue;
        }
        if (result != NO_ERROR) return {};
        auto table = reinterpret_cast<const MIB_TCPTABLE_OWNER_MODULE*>(buffer.data());
        Socket newest{};
        for (DWORD index = 0; index < table->dwNumEntries; ++index) {
            const auto& row = table->table[index];
            if (row.dwState != MIB_TCP_STATE_ESTAB || ntohs(static_cast<USHORT>(row.dwRemotePort)) != gamePort ||
                gameProcesses.find(row.dwOwningPid) == gameProcesses.end()) continue;
            if (!newest || row.liCreateTimestamp.QuadPart > newest.created) {
                newest = {{row.dwState, row.dwLocalAddr, row.dwLocalPort, row.dwRemoteAddr, row.dwRemotePort},
                    row.dwOwningPid, row.liCreateTimestamp.QuadPart};
            }
        }
        return newest;
    }
    return {};
}

LONGLONG FileTimeNow() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<LONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

template <typename T> T ReadValue(const BYTE* bytes) {
    T value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

class ReceiveMonitor {
public:
    ULONG Start() {
        if (FAILED(CoCreateGuid(&params.properties.Wnode.Guid))) return ERROR_GEN_FAILURE;
        name = L"AION2Overlay-" + std::to_wstring(GetCurrentProcessId());
        params.properties.Wnode.BufferSize = sizeof(params);
        params.properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        params.properties.Wnode.ClientContext = 2;
        params.properties.LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        params.properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        params.properties.EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP | EVENT_TRACE_FLAG_NO_SYSCONFIG;
        params.properties.FlushTimer = 1;
        ULONG result = StartTraceW(&session, name.c_str(), &params.properties);
        if (result != ERROR_SUCCESS) { session = 0; return result; }
        EVENT_TRACE_LOGFILEW log{};
        log.LoggerName = &name[0];
        log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        log.EventRecordCallback = OnEvent;
        log.Context = this;
        trace = OpenTraceW(&log);
        if (trace == INVALID_PROCESSTRACE_HANDLE) return GetLastError();
        worker = std::thread([this] { failure = ProcessTrace(&trace, 1, nullptr, nullptr); });
        return ERROR_SUCCESS;
    }

    ~ReceiveMonitor() {
        if (session) ControlTraceW(session, name.c_str(), &params.properties, EVENT_TRACE_CONTROL_STOP);
        if (trace != INVALID_PROCESSTRACE_HANDLE) CloseTrace(trace);
        if (worker.joinable()) worker.join();
    }

    void Select(const Socket& socket) {
        std::lock_guard<std::mutex> lock(gate);
        selected = socket;
        last = 0;
        gaps.clear();
    }

    int Gap() {
        std::lock_guard<std::mutex> lock(gate);
        if (!last || FileTimeNow() - last > 4000 * ticksPerMs) return -1;
        int largest = -1;
        for (const auto& point : gaps) largest = std::max(largest, point.ms);
        return largest;
    }

    ULONG Error() const { return failure.load(); }

private:
    static void WINAPI OnEvent(EVENT_RECORD* event) {
        const auto& header = event->EventHeader;
        if (!IsEqualGUID(header.ProviderId, tcpProvider) || header.EventDescriptor.Opcode != 11 ||
            header.EventDescriptor.Version < 1 || event->UserDataLength < 20 || !event->UserData) return;
        static_cast<ReceiveMonitor*>(event->UserContext)->OnReceive(
            static_cast<const BYTE*>(event->UserData), header.TimeStamp.QuadPart);
    }

    void OnReceive(const BYTE* data, LONGLONG stamp) {
        // IPv4 TCP receive events describe the endpoints from the local socket's perspective.
        if (ReadValue<USHORT>(data + 16) != htons(gamePort) || !ReadValue<ULONG>(data + 4)) return;
        std::lock_guard<std::mutex> lock(gate);
        if (selected.pid != ReadValue<DWORD>(data) || selected.row.dwRemoteAddr != ReadValue<DWORD>(data + 8) ||
            selected.row.dwLocalAddr != ReadValue<DWORD>(data + 12) ||
            static_cast<USHORT>(selected.row.dwLocalPort) != ReadValue<USHORT>(data + 18) ||
            stamp <= last || stamp < selected.created) return;
        if (last) gaps.push_back({stamp, static_cast<int>(std::min<LONGLONG>(INT_MAX, (stamp - last) / ticksPerMs))});
        last = stamp;
        while (!gaps.empty() && last - gaps.front().stamp > 2000 * ticksPerMs) gaps.pop_front();
    }

    struct TraceParameters {
        EVENT_TRACE_PROPERTIES properties{};
        wchar_t loggerName[128]{};
    } params;
    struct GapSample { LONGLONG stamp; int ms; };
    std::wstring name;
    TRACEHANDLE session = 0;
    TRACEHANDLE trace = INVALID_PROCESSTRACE_HANDLE;
    std::thread worker;
    std::atomic<ULONG> failure{0};
    std::mutex gate;
    Socket selected;
    LONGLONG last = 0;
    std::deque<GapSample> gaps;
};

std::wstring Address(DWORD address) {
    IN_ADDR ipv4{};
    ipv4.S_un.S_addr = address;
    wchar_t value[INET_ADDRSTRLEN]{};
    InetNtopW(AF_INET, &ipv4, value, _countof(value));
    return value;
}

void Fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

class Overlay {
public:
    HWND window = nullptr;
    explicit Overlay(Settings settings) : transparent(settings.transparent != 0) {}

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            dpi = GetDpiForWindow(window);
            reconnect = Button(L"Reconnect", reconnectId);
            minimize = Button(L"Collapse overlay", minimizeId);
            close = Button(L"Close", closeId);
            toggle = Button(L"Toggle transparent background", backgroundId);
            SetupTooltip();
            SetTransparency(transparent);
            captureError = receive.Start();
            Poll();
            SetTimer(window, pollTimer, 250, nullptr);
            SetTimer(window, graphTimer, 1000, nullptr);
            return 0;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) { Layout(); InvalidateRect(window, nullptr, FALSE); }
            return 0;
        case WM_DPICHANGED: {
            dpi = HIWORD(wparam);
            CreateFonts();
            const auto rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto sizing = reinterpret_cast<MINMAXINFO*>(lparam);
            sizing->ptMinTrackSize = {Px(350), Px(240)};
            return 0;
        }
        case WM_NCHITTEST:
            return collapsed ? HTCLIENT : HitTest(lparam);
        case WM_NCCALCSIZE:
            return 0;
        case WM_MOUSEACTIVATE:
            if (collapsed && LOWORD(lparam) == HTCLIENT) return MA_NOACTIVATE;
            break;
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED) {
                switch (LOWORD(wparam)) {
                case reconnectId: Reconnect(); return 0;
                case minimizeId: SetCollapsed(!collapsed); return 0;
                case closeId: DestroyWindow(window); return 0;
                case backgroundId: SetTransparency(!transparent); return 0;
                }
            }
            break;
        case WM_DRAWITEM:
            DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        case WM_TIMER:
            if (wparam == pollTimer) Poll();
            if (wparam == graphTimer) SampleGraph();
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_DESTROY:
            SaveSettings();
            KillTimer(window, pollTimer);
            KillTimer(window, graphTimer);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    ~Overlay() { DeleteFonts(); }

private:
    HWND reconnect = nullptr, minimize = nullptr, close = nullptr, toggle = nullptr, tooltip = nullptr;
    HFONT normal = nullptr, heading = nullptr, large = nullptr, addressFont = nullptr;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    ReceiveMonitor receive;
    Socket current{};
    ULONG captureError = 0;
    bool transparent = false, collapsed = false;
    bool statsRequested = false;
    int gap = -1, ping = -1, graphPeak = -1;
    std::wstring error;
    ULONGLONG retryAfter = 0;
    ULONGLONG socketAgeMs = 0, socketSeenAt = 0;
    std::wstring socketTip = L"No active game connection.";
    struct PlotPoint { ULONGLONG time; int value; };
    std::deque<PlotPoint> plot;

    int Px(int px) const { return MulDiv(px, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

    HWND Button(const wchar_t* label, INT id) {
        return CreateWindowW(L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    }

    HFONT Font(int height, LONG weight) const {
        return CreateFontW(-Px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, transparent ? NONANTIALIASED_QUALITY : CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }

    void DeleteFonts() {
        for (auto font : {normal, heading, large, addressFont}) if (font) DeleteObject(font);
    }

    void CreateFonts() {
        DeleteFonts();
        normal = Font(13, FW_NORMAL);
        heading = Font(11, FW_BOLD);
        large = Font(23, FW_BOLD);
        addressFont = Font(17, FW_SEMIBOLD);
    }

    void Layout() {
        RECT rect{};
        GetClientRect(window, &rect);
        MoveWindow(minimize, rect.right - Px(62), Px(5), Px(25), Px(24), TRUE);
        MoveWindow(close, rect.right - Px(34), Px(5), Px(25), Px(24), TRUE);
        MoveWindow(toggle, rect.right - Px(102), Px(5), Px(34), Px(24), TRUE);
        MoveWindow(reconnect, rect.right - Px(116), rect.bottom - Px(36), Px(104), Px(26), TRUE);
        if (tooltip) {
            auto tool = SocketTool();
            SendMessageW(tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        if (collapsed) CropToRestoreButton();
    }

    void SaveSettings() const {
        RECT rect{};
        if (!GetWindowRect(window, &rect)) return;
        const Settings settings{rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, transparent};
        RegSetKeyValueW(HKEY_CURRENT_USER, settingsPath, L"Window", REG_BINARY, &settings, sizeof(settings));
    }

    void SetTransparency(bool enabled) {
        transparent = enabled;
        LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (enabled) {
            SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_LAYERED);
            if (!SetLayeredWindowAttributes(window, transparentKey, 255, LWA_COLORKEY)) {
                transparent = false;
                SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
            }
        } else if (style & WS_EX_LAYERED) {
            SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
        }
        CreateFonts();
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void CropToRestoreButton() const {
        RECT rect{};
        GetWindowRect(minimize, &rect);
        MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rect), 2);
        HRGN region = CreateRectRgnIndirect(&rect);
        if (region && !SetWindowRgn(window, region, TRUE)) DeleteObject(region);
    }

    void SetCollapsed(bool enabled) {
        collapsed = enabled;
        SetWindowTextW(minimize, enabled ? L"Restore overlay" : L"Collapse overlay");
        for (HWND control : {close, reconnect, toggle}) ShowWindow(control, enabled ? SW_HIDE : SW_SHOWNA);
        if (tooltip) SendMessageW(tooltip, TTM_ACTIVATE, !enabled, 0);
        if (enabled) {
            SaveSettings();
            CropToRestoreButton();
        } else {
            SetWindowRgn(window, nullptr, TRUE);
        }
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    TOOLINFOW SocketTool() {
        RECT rect{};
        GetClientRect(window, &rect);
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_SUBCLASS;
        tool.hwnd = window;
        tool.uId = 1;
        tool.rect = {rect.right / 2, Px(38), rect.right - Px(12), Px(79)};
        tool.lpszText = &socketTip[0];
        return tool;
    }

    void SetupTooltip() {
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (tooltip) {
            auto tool = SocketTool();
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }

    LRESULT HitTest(LPARAM lparam) const {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &pt);
        RECT client{};
        GetClientRect(window, &client);
        int border = Px(6);
        bool left = pt.x < border, right = pt.x >= client.right - border;
        if (pt.y < border) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (pt.y >= client.bottom - border) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        return pt.y < Px(33) && pt.x < client.right - Px(65) ? HTCAPTION : HTCLIENT;
    }

    void Poll() {
        Socket next = FindSocket();
        if (!SameSocket(next, current)) {
            current = next;
            receive.Select(current);
            plot.clear();
            graphPeak = -1;
            statsRequested = false;
            retryAfter = 0;
            error.clear();
            socketSeenAt = GetTickCount64();
            socketAgeMs = current ? static_cast<ULONGLONG>(std::max<LONGLONG>(0, FileTimeNow() - current.created) / ticksPerMs) : 0;
            socketTip = current ? L"The local port identifies this connection on your PC." : L"No active game connection.";
            if (tooltip) {
                auto tool = SocketTool();
                SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }
        gap = current ? receive.Gap() : -1;
        graphPeak = std::max(graphPeak, gap);
        ping = -1;
        if (current) {
            TCP_ESTATS_PATH_RW_v0 enabled{};
            TCP_ESTATS_PATH_ROD_v0 stats{};
            ULONG status = GetPerTcpConnectionEStats(&current.row, TcpConnectionEstatsPath,
                reinterpret_cast<PUCHAR>(&enabled), 0, sizeof(enabled), nullptr, 0, 0,
                reinterpret_cast<PUCHAR>(&stats), 0, sizeof(stats));
            if (status == NO_ERROR && enabled.EnableCollection && stats.CountRtt && stats.SmoothedRtt != ULONG_MAX) {
                ping = static_cast<int>(stats.SmoothedRtt);
            } else if (status == NO_ERROR && !enabled.EnableCollection && !statsRequested) {
                enabled.EnableCollection = TRUE;
                SetPerTcpConnectionEStats(&current.row, TcpConnectionEstatsPath,
                    reinterpret_cast<PUCHAR>(&enabled), 0, sizeof(enabled), 0);
                statsRequested = true;
            }
        }
        const bool pending = GetTickCount64() < retryAfter;
        EnableWindow(reconnect, current && !pending);
        const wchar_t* caption = pending ? L"Reconnecting" : L"Reconnect";
        wchar_t oldCaption[32]{};
        GetWindowTextW(reconnect, oldCaption, _countof(oldCaption));
        if (wcscmp(caption, oldCaption) != 0) SetWindowTextW(reconnect, caption);
        if (!collapsed) InvalidateRect(window, nullptr, FALSE);
    }

    void Reconnect() {
        if (!current || GetTickCount64() < retryAfter) return;
        Socket verified = FindSocket();
        if (!SameSocket(verified, current)) { Poll(); return; }
        MIB_TCPROW target = verified.row;
        target.dwState = MIB_TCP_STATE_DELETE_TCB;
        const DWORD code = SetTcpEntry(&target);
        if (code == NO_ERROR) retryAfter = GetTickCount64() + 3000;
        error = code == NO_ERROR ? L"" : L"Reconnect failed (Windows error " + std::to_wstring(code) + L").";
        Poll();
    }

    void SampleGraph() {
        const auto now = GetTickCount64();
        if (current) plot.push_back({now, graphPeak});
        graphPeak = gap;
        while (!plot.empty() && now - plot.front().time > 120000) plot.pop_front();
        if (!collapsed) InvalidateRect(window, nullptr, FALSE);
    }

    void DrawTextLine(HDC dc, const std::wstring& value, HFONT font, RECT rect, UINT align = DT_LEFT) const {
        SelectObject(dc, font);
        SetTextColor(dc, text);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, align | DT_VCENTER | DT_SINGLELINE |
            DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    void DrawButton(const DRAWITEMSTRUCT& item) const {
        wchar_t value[32]{};
        GetWindowTextW(item.hwndItem, value, _countof(value));
        const wchar_t* label = value;
        if (item.CtlID == closeId) label = L"\u00d7";
        if (item.CtlID == minimizeId) label = collapsed ? L"+" : L"\u2212";
        if (item.CtlID == backgroundId) label = L"BG";
        COLORREF color = background;
        if (item.CtlID == reconnectId) color = (item.itemState & ODS_DISABLED) ? panel : action;
        if ((item.CtlID == minimizeId && collapsed) || (item.CtlID == backgroundId && transparent)) color = action;
        if (item.itemState & ODS_SELECTED) color = grid;
        Fill(item.hDC, item.rcItem, color);
        DrawTextLine(item.hDC, label, normal, item.rcItem, DT_CENTER);
        if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS) DrawFocusRect(item.hDC, &item.rcItem);
    }

    static std::wstring Milliseconds(int value) {
        return value < 0 ? L"\u2014" : std::to_wstring(value) + L" ms";
    }

    std::wstring ConnectionAge() const {
        auto seconds = (socketAgeMs + GetTickCount64() - socketSeenAt) / 1000;
        wchar_t value[48]{};
        if (seconds >= 3600) {
            swprintf_s(value, L"Connected %llu:%02llu:%02llu", seconds / 3600, seconds / 60 % 60, seconds % 60);
        } else {
            swprintf_s(value, L"Connected %02llu:%02llu", seconds / 60, seconds % 60);
        }
        return value;
    }

    void DrawGraph(HDC dc, RECT rect) const {
        if (!transparent) Fill(dc, rect, panel);
        const int x = rect.left + Px(8), axis = rect.right - Px(66);
        const int top = rect.top + Px(9), bottom = rect.bottom - Px(10);
        if (bottom <= top || axis <= x) return;
        int minimum = INT_MAX, maximum = 0;
        for (const auto& point : plot) {
            if (point.value < 0) continue;
            minimum = std::min(minimum, point.value);
            maximum = std::max(maximum, point.value);
        }
        int low = minimum == INT_MAX ? 0 : std::max(0, (minimum / 5) * 5 - 5);
        int high = std::max(low + 20, (std::min(maximum, INT_MAX - 10) / 5) * 5 + 5);
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, grid);
        const int lines = bottom - top >= Px(34) ? 2 : 1;
        for (int index = 0; index <= lines; ++index) {
            int y = top + (bottom - top) * index / lines;
            MoveToEx(dc, x, y, nullptr);
            LineTo(dc, axis, y);
            DrawTextLine(dc, Milliseconds(high - (high - low) * index / lines), heading,
                {axis + Px(5), y - Px(8), rect.right - Px(3), y + Px(8)});
        }
        SetDCPenColor(dc, line);
        bool linked = false;
        const auto now = GetTickCount64();
        for (const auto& point : plot) {
            const auto elapsed = now - point.time;
            if (elapsed > 120000 || point.value < 0) { linked = false; continue; }
            int col = axis - static_cast<int>((axis - x) * elapsed / 120000);
            int row = bottom - MulDiv(bottom - top, point.value - low, high - low);
            if (linked) LineTo(dc, col, row);
            else { SetPixelV(dc, col, row, line); MoveToEx(dc, col, row, nullptr); }
            linked = true;
        }
    }

    void Paint() {
        PAINTSTRUCT surface{};
        HDC target = BeginPaint(window, &surface);
        RECT client{};
        GetClientRect(window, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        HGDIOBJ original = SelectObject(dc, bitmap);
        Fill(dc, client, transparent ? transparentKey : background);
        const int right = client.right / 2, margin = Px(12);
        if (transparent) Fill(dc, {margin - Px(4), Px(5), Px(126), Px(29)}, background);
        DrawTextLine(dc, L"AION2 NETWORK", heading, {margin, Px(5), right, Px(29)});
        DrawTextLine(dc, L"SERVER IP", heading, {margin, Px(38), right, Px(53)});
        DrawTextLine(dc, L"SOCKET", heading, {right, Px(38), client.right - margin, Px(53)});
        DrawTextLine(dc, current ? Address(current.row.dwRemoteAddr) : L"\u2014", addressFont,
            {margin, Px(53), right, Px(79)});
        DrawTextLine(dc, current ? std::to_wstring(ntohs(static_cast<USHORT>(current.row.dwLocalPort))) : L"\u2014", addressFont,
            {right, Px(53), client.right - margin, Px(79)});
        DrawTextLine(dc, L"RECEIVE GAP", heading, {margin, Px(85), right, Px(100)});
        DrawTextLine(dc, L"TCP PING", heading, {right, Px(85), client.right - margin, Px(100)});
        DrawTextLine(dc, Milliseconds(gap), large, {margin, Px(100), right, Px(129)});
        DrawTextLine(dc, Milliseconds(ping), large, {right, Px(100), client.right - margin, Px(129)});
        DrawTextLine(dc, L"RECEIVE GAP / LAST 2 MIN", heading, {margin, Px(135), client.right - margin, Px(151)});
        DrawGraph(dc, {margin, Px(152), client.right - margin, client.bottom - Px(44)});
        std::wstring status = error;
        if (status.empty() && (captureError || receive.Error())) status = L"Receive measurement unavailable.";
        if (status.empty() && !current) status = L"Waiting for AION2 connection";
        if (status.empty() && current) status = ConnectionAge();
        DrawTextLine(dc, status, normal, {margin, client.bottom - Px(35), client.right - Px(126), client.bottom - Px(10)});
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, original);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window, &surface);
    }
};

LRESULT CALLBACK Dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto info = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto overlay = static_cast<Overlay*>(info->lpCreateParams);
        overlay->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    }
    auto overlay = reinterpret_cast<Overlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return overlay ? overlay->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\AION2NetworkPublicNative");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&common);
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpfnWndProc = Dispatch;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.lpszClassName = L"AION2NetworkPublicNative";
    RegisterClassW(&type);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    int width = MulDiv(390, dpi, 96), height = MulDiv(273, dpi, 96);
    int x = std::max(work.left, work.right - width - MulDiv(25, dpi, 96)), y = work.top + MulDiv(40, dpi, 96);
    const Settings settings = ReadSettings();
    if (settings.width > 0 && settings.width < 16000 && settings.height > 0 && settings.height < 16000 &&
        settings.x > -100000 && settings.x < 100000 && settings.y > -100000 && settings.y < 100000) {
        RECT previous{settings.x, settings.y, settings.x + settings.width, settings.y + settings.height};
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromRect(&previous, MONITOR_DEFAULTTONEAREST), &monitor)) {
            work = monitor.rcWork;
            width = std::min(std::max(settings.width, 350L), work.right - work.left);
            height = std::min(std::max(settings.height, 240L), work.bottom - work.top);
            x = std::clamp(settings.x, work.left, work.right - width);
            y = std::clamp(settings.y, work.top, work.bottom - height);
        }
    }
    {
        Overlay overlay(settings);
        HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, type.lpszClassName, L"AION2 Network Overlay",
            WS_POPUP | WS_CLIPCHILDREN | WS_MINIMIZEBOX | WS_SYSMENU | WS_THICKFRAME, x, y, width, height,
            nullptr, nullptr, instance, &overlay);
        if (window) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
        }
    }
    CloseHandle(mutex);
    return 0;
}
