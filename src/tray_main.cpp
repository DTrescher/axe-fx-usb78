#include "tray_settings.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"AxeFxUsb78TrayPanelWindow";
constexpr wchar_t kWindowTitle[] = L"Axe-Fx III USB 7/8 Audio";
constexpr wchar_t kPanelMutex[] = L"Local\\AxeFxUsb78Panel";
constexpr UINT kActivateMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT kShutdownMessage = WM_APP + 3;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMilliseconds = 250;
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuShow = 1001;
constexpr UINT kMenuEnable = 1002;
constexpr UINT kMenuDisable = 1003;
constexpr UINT kMenuRestart = 1004;
constexpr UINT kMenuExit = 1005;
constexpr int kControlEnable = 2001;
constexpr int kControlSound = 2002;
constexpr int kControlExit = 2003;
constexpr int kControlStatus = 2004;
constexpr int kControlDetails = 2005;

std::wstring format_error(const wchar_t* action, DWORD error) {
    return std::wstring(action) + L" (Windows error " + std::to_wstring(error) + L")";
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        const int fallbackCount = MultiByteToWideChar(CP_ACP, 0, text.data(),
            static_cast<int>(text.size()), nullptr, 0);
        if (fallbackCount <= 0) return L"(unreadable child output)";
        std::wstring result(static_cast<std::size_t>(fallbackCount), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()),
                            result.data(), fallbackCount);
        return result;
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), count);
    return result;
}

bool parse_integer_after(std::string_view line, std::string_view prefix, long long& value) noexcept {
    const auto position = line.find(prefix);
    if (position == std::string_view::npos) return false;
    const char* first = line.data() + position + prefix.size();
    const char* last = line.data() + line.size();
    const auto parsed = std::from_chars(first, last, value, 10);
    return parsed.ec == std::errc{} && parsed.ptr != first;
}

bool parse_decimal_after(std::string_view line, std::string_view prefix, double& value) noexcept {
    const auto position = line.find(prefix);
    if (position == std::string_view::npos) return false;
    const char* first = line.data() + position + prefix.size();
    const char* last = line.data() + line.size();
    const auto parsed = std::from_chars(first, last, value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr != first && std::isfinite(value);
}

HICON create_usb_audio_icon() noexcept {
    constexpr LONG size = 32;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    void* rawPixels{};
    HBITMAP colorBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!colorBitmap || !rawPixels) {
        if (colorBitmap) DeleteObject(colorBitmap);
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(rawPixels);
    std::fill_n(pixels, size * size, 0u);
    const auto color = [](BYTE red, BYTE green, BYTE blue) noexcept {
        return static_cast<std::uint32_t>(blue) |
               (static_cast<std::uint32_t>(green) << 8) |
               (static_cast<std::uint32_t>(red) << 16) | 0xff000000u;
    };
    const auto put = [&](int x, int y, std::uint32_t value) noexcept {
        if (x >= 0 && x < size && y >= 0 && y < size)
            pixels[static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)] = value;
    };
    const auto rectangle = [&](int left, int top, int right, int bottom, std::uint32_t value) noexcept {
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x) put(x, y, value);
    };
    const auto line = [&](int x0, int y0, int x1, int y1, int thickness,
                          std::uint32_t value) noexcept {
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            rectangle(x0 - thickness / 2, y0 - thickness / 2,
                      x0 + (thickness + 1) / 2, y0 + (thickness + 1) / 2, value);
            if (x0 == x1 && y0 == y1) break;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x0 += sx; }
            if (twice <= dx) { error += dx; y0 += sy; }
        }
    };
    const auto triangle = [&](POINT a, POINT b, POINT c, std::uint32_t value) noexcept {
        const auto edge = [](POINT p, POINT q, int x, int y) noexcept {
            return (x - p.x) * (q.y - p.y) - (y - p.y) * (q.x - p.x);
        };
        const int minX = std::min({a.x, b.x, c.x});
        const int maxX = std::max({a.x, b.x, c.x});
        const int minY = std::min({a.y, b.y, c.y});
        const int maxY = std::max({a.y, b.y, c.y});
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const int ab = edge(a, b, x, y);
                const int bc = edge(b, c, x, y);
                const int ca = edge(c, a, x, y);
                if ((ab >= 0 && bc >= 0 && ca >= 0) || (ab <= 0 && bc <= 0 && ca <= 0))
                    put(x, y, value);
            }
        }
    };

    const auto background = color(24, 75, 112);
    for (int y = 2; y < 30; ++y) {
        for (int x = 2; x < 30; ++x) {
            const int nearestX = std::clamp(x, 6, 25);
            const int nearestY = std::clamp(y, 6, 25);
            const int dx = x - nearestX, dy = y - nearestY;
            if (dx * dx + dy * dy <= 16) put(x, y, background);
        }
    }

    const auto white = color(250, 252, 255);
    const auto cyan = color(89, 218, 226);
    line(11, 9, 11, 24, 2, white);
    triangle({8, 11}, {14, 11}, {11, 6}, white);
    line(11, 15, 7, 12, 2, white);
    line(11, 18, 16, 14, 2, white);
    line(11, 23, 15, 26, 2, white);
    rectangle(5, 10, 8, 13, white);
    rectangle(15, 12, 18, 15, white);
    triangle({13, 25}, {18, 25}, {16, 29}, white);
    rectangle(21, 12, 23, 23, cyan);
    rectangle(25, 9, 27, 26, cyan);

    std::array<BYTE, 128> maskBits{};
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, maskBits.data());
    if (!maskBitmap) {
        DeleteObject(colorBitmap);
        return nullptr;
    }
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.xHotspot = size / 2;
    iconInfo.yHotspot = size / 2;
    iconInfo.hbmMask = maskBitmap;
    iconInfo.hbmColor = colorBitmap;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return icon;
}

std::wstring quote_argument(std::wstring_view argument) {
    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');
    std::size_t backslashes{};
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }
private:
    HANDLE handle_{};
};

class AttributeList {
public:
    explicit AttributeList(const std::vector<HANDLE>& handles) {
        initialize();
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       const_cast<HANDLE*>(handles.data()),
                                       handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const DWORD error = GetLastError();
            DeleteProcThreadAttributeList(list_);
            initialized_ = false;
            throw std::wstring(format_error(L"Could not restrict inherited child handles", error));
        }
    }
    ~AttributeList() { if (initialized_) DeleteProcThreadAttributeList(list_); }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }
private:
    void initialize() {
        SIZE_T bytes{};
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (bytes == 0) throw std::wstring(format_error(L"Could not size the process attribute list", GetLastError()));
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &bytes))
            throw std::wstring(format_error(L"Could not initialize the process attribute list", GetLastError()));
        initialized_ = true;
    }
    std::vector<std::uint8_t> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_{};
    bool initialized_{};
};

class TrayPanel {
public:
    explicit TrayPanel(bool minimized) : minimized_(minimized) {
        const auto loaded = axe::panel::settings::load();
        if (!loaded.ok) {
            settingsNotice_ = loaded.error;
            desiredEnabled_ = false;
            status_ = L"Settings could not be read. Routing is disabled until you save a choice.";
        } else if (!loaded.found) {
            desiredEnabled_ = true;
            std::wstring error;
            if (!axe::panel::settings::save(true, error)) {
                desiredEnabled_ = false;
                settingsNotice_ = L"Could not save the initial routing preference: " + error;
                status_ = L"Routing stayed off because the preference could not be saved.";
            } else {
                settingsNotice_.clear();
                status_ = L"Starting routing…";
            }
        } else {
            desiredEnabled_ = loaded.enabled;
            status_ = desiredEnabled_ ? L"Starting routing…" : L"Routing disabled.";
        }
    }

    ~TrayPanel() {
        removeTrayIcon();
        if (process_) {
            if (stopEvent_) SetEvent(stopEvent_.get());
            if (job_) TerminateJobObject(job_.get(), ERROR_CANCELLED);
        }
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        if (ownsTrayIcon_ && trayIcon_) DestroyIcon(trayIcon_);
        if (ownsFont_ && font_) DeleteObject(font_);
    }

    HWND window() const noexcept { return window_; }

    bool create(HINSTANCE instance) {
        instance_ = instance;
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &TrayPanel::windowProc;
        windowClass.hInstance = instance_;
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        windowClass.lpszClassName = kWindowClass;
        windowClass.hIconSm = windowClass.hIcon;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::wstring(format_error(L"Could not register the control window", GetLastError()));

        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        window_ = CreateWindowExW(0, kWindowClass, kWindowTitle,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 590, 340, nullptr, nullptr, instance_, this);
        return window_ != nullptr;
    }

    void finishInitialization() {
        createControls();
        updateControls();
        // Explorer may not have created the notification area at logon yet.
        // Preserve the requested visibility and retry independently of routing.
        addTrayIcon();
        if (!SetTimer(window_, kTimerId, kTimerMilliseconds, nullptr))
            throw std::wstring(format_error(L"Could not start the child-process monitor", GetLastError()));
        if (desiredEnabled_) startChild();
        if (!minimized_) {
            ShowWindow(window_, SW_SHOWNORMAL);
            UpdateWindow(window_);
        }
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        TrayPanel* self = reinterpret_cast<TrayPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<TrayPanel*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        if (self->taskbarCreatedMessage_ != 0 && message == self->taskbarCreatedMessage_) {
            self->trayIconAdded_ = false;
            self->addTrayIcon();
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            self->trayIcon_ = create_usb_audio_icon();
            self->ownsTrayIcon_ = self->trayIcon_ != nullptr;
            if (!self->trayIcon_) self->trayIcon_ = LoadIconW(nullptr, IDI_APPLICATION);
            return 0;
        case kActivateMessage:
            self->showWindow();
            return 0;
        case kShutdownMessage:
            self->beginExit();
            return 0;
        case WM_COMMAND:
            self->onCommand(LOWORD(wParam));
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(30, 38, 46));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        case WM_TIMER:
            if (wParam == kTimerId) {
                if (!self->trayIconAdded_ && !self->exitRequested_ &&
                    std::chrono::steady_clock::now() >= self->nextTrayAttempt_)
                    self->addTrayIcon();
                self->pollChild();
            }
            return 0;
        case kTrayMessage:
            self->onTrayNotification(lParam);
            return 0;
        case WM_CLOSE:
            self->hideWindow();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) {
                self->exitRequested_ = true;
                self->requestStop();
                if (!self->process_) DestroyWindow(window);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, kTimerId);
            self->removeTrayIcon();
            if (self->process_ && self->job_)
                TerminateJobObject(self->job_.get(), ERROR_CANCELLED);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

private:
    HWND window_{};
    HWND enableCheck_{};
    HWND deviceText_{};
    HWND routeText_{};
    HWND settingsText_{};
    HWND statusText_{};
    HWND detailsText_{};
    HWND soundButton_{};
    HWND exitButton_{};
    HINSTANCE instance_{};
    HICON trayIcon_{};
    HFONT font_{};
    UINT taskbarCreatedMessage_{};
    bool minimized_{};
    bool trayIconAdded_{};
    bool ownsTrayIcon_{};
    bool ownsFont_{};
    bool desiredEnabled_{};
    bool blocked_{};
    bool exitRequested_{};
    bool stopping_{};
    bool pipeEof_{};
    bool bridgeConflict_{};
    bool hasStats_{};
    std::chrono::steady_clock::time_point nextTrayAttempt_{};
#ifdef AXE_PANEL_TEST_TRAY_DELAY_MS
    const std::chrono::steady_clock::time_point testTrayAvailableAt_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(AXE_PANEL_TEST_TRAY_DELAY_MS);
#endif
    std::wstring status_;
    std::wstring settingsNotice_;
    std::wstring latestDiagnostic_;
    std::wstring streamInfo_;
    std::wstring asioInfo_;
    std::wstring statsLine_;
    std::string pendingLine_;
    UniqueHandle process_;
    UniqueHandle job_;
    UniqueHandle stopEvent_;
    UniqueHandle pipeRead_;
    std::chrono::steady_clock::time_point stopRequestedAt_{};

    HWND makeControl(DWORD extendedStyle,
                     const wchar_t* className,
                     const wchar_t* text,
                     DWORD style,
                     int x, int y, int width, int height,
                     int controlId) {
        HWND control = CreateWindowExW(extendedStyle, className, text,
            WS_CHILD | WS_VISIBLE | style, x, y, width, height,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), instance_, nullptr);
        if (!control) throw std::wstring(format_error(L"Could not create a control", GetLastError()));
        const HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    }

    void createControls() {
        HDC dc = GetDC(window_);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(window_, dc);
        font_ = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ownsFont_ = font_ != nullptr;

        enableCheck_ = makeControl(0, L"BUTTON", L"Enable Windows audio routing",
            BS_AUTOCHECKBOX | WS_TABSTOP, 18, 15, 540, 28, kControlEnable);
        deviceText_ = makeControl(0, L"STATIC", L"Device: Speakers (Axe-Fx III)",
            SS_LEFT | SS_NOPREFIX, 20, 56, 540, 22, 0);
        routeText_ = makeControl(0, L"STATIC", L"Route: Windows shared audio at 48 kHz → FAS ASIO USB 7/8",
            SS_LEFT | SS_NOPREFIX, 20, 82, 540, 22, 0);
        settingsText_ = makeControl(0, L"STATIC", L"ASIO buffer: 128 frames · target queue: 13 ms",
            SS_LEFT | SS_NOPREFIX, 20, 108, 540, 22, 0);
        statusText_ = makeControl(0, L"STATIC", L"Status: Starting…",
            SS_LEFT | SS_NOPREFIX, 20, 145, 540, 40, kControlStatus);
        detailsText_ = makeControl(0, L"STATIC", L"",
            SS_LEFT | SS_NOPREFIX, 20, 188, 540, 55, kControlDetails);
        soundButton_ = makeControl(0, L"BUTTON", L"Open Windows Sound settings",
            BS_PUSHBUTTON | WS_TABSTOP, 20, 254, 210, 30, kControlSound);
        exitButton_ = makeControl(0, L"BUTTON", L"Exit",
            BS_PUSHBUTTON | WS_TABSTOP, 465, 254, 95, 30, kControlExit);
    }

    void updateControls() {
        if (enableCheck_) {
            SendMessageW(enableCheck_, BM_SETCHECK,
                         desiredEnabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow(enableCheck_, !exitRequested_);
        }
        if (statusText_) {
            std::wstring text = L"Status: " + status_;
            if (!settingsNotice_.empty()) text += L"\r\n" + settingsNotice_;
            SetWindowTextW(statusText_, text.c_str());
        }
        if (detailsText_) {
            std::wstring text;
            if (!streamInfo_.empty()) text = streamInfo_;
            if (!asioInfo_.empty()) {
                if (!text.empty()) text += L"\r\n";
                text += asioInfo_;
            }
            const std::wstring& detail = !latestDiagnostic_.empty()
                ? latestDiagnostic_ : (hasStats_ ? statsLine_ : std::wstring{});
            if (!detail.empty()) {
                if (!text.empty()) text += L"\r\n";
                text += detail;
            }
            SetWindowTextW(detailsText_, text.c_str());
        }
        updateTrayTip();
    }

    void showWindow() {
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOWNORMAL);
        SetForegroundWindow(window_);
    }

    void hideWindow() {
        if (!trayIconAdded_) {
            beginExit();
            return;
        }
        ShowWindow(window_, SW_HIDE);
    }

    bool addTrayIcon() {
        nextTrayAttempt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
#ifdef AXE_PANEL_TEST_TRAY_DELAY_MS
        // Only the optional integration-test binary injects Explorer delay.
        if (std::chrono::steady_clock::now() < testTrayAvailableAt_) return false;
#endif
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
        updateTrayTipText(data.szTip, ARRAYSIZE(data.szTip));
        trayIconAdded_ = Shell_NotifyIconW(trayIconAdded_ ? NIM_MODIFY : NIM_ADD, &data) != FALSE;
        // A repeated TaskbarCreated notification may leave our icon present.
        if (!trayIconAdded_) trayIconAdded_ = Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
        return trayIconAdded_;
    }

    void removeTrayIcon() noexcept {
        if (!trayIconAdded_ || !window_) return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayIconAdded_ = false;
    }

    void updateTrayTipText(wchar_t* destination, std::size_t capacity) const {
        if (capacity == 0) return;
        const std::wstring tip = desiredEnabled_
            ? (process_ ? L"Axe-Fx III audio routing enabled" : L"Axe-Fx III routing enabled; bridge stopped")
            : L"Axe-Fx III audio routing disabled";
        wcsncpy_s(destination, capacity, tip.c_str(), _TRUNCATE);
    }

    void updateTrayTip() {
        if (!trayIconAdded_ || !window_) return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_TIP;
        updateTrayTipText(data.szTip, ARRAYSIZE(data.szTip));
        if (!Shell_NotifyIconW(NIM_MODIFY, &data)) trayIconAdded_ = false;
    }

    void onTrayNotification(LPARAM notification) {
        if (notification == WM_LBUTTONDBLCLK || notification == NIN_SELECT ||
            notification == NIN_KEYSELECT) {
            showWindow();
        } else if (notification == WM_RBUTTONUP || notification == WM_CONTEXTMENU) {
            showTrayMenu();
        }
    }

    void showTrayMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kMenuShow, L"Show window");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (desiredEnabled_ ? MF_GRAYED : 0),
                    kMenuEnable, L"Enable routing");
        AppendMenuW(menu, MF_STRING | (!desiredEnabled_ ? MF_GRAYED : 0),
                    kMenuDisable, L"Disable routing");
        if (desiredEnabled_ && blocked_)
            AppendMenuW(menu, MF_STRING, kMenuRestart, L"Retry routing");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                       point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
    }

    void onCommand(UINT id) {
        if (id == kControlEnable) {
            const bool requested = SendMessageW(enableCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            setEnabled(requested);
            return;
        }
        switch (id) {
        case kControlSound: {
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open",
                L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) {
                status_ = L"Windows Sound settings could not be opened (error " +
                          std::to_wstring(result) + L").";
                updateControls();
            }
            break;
        }
        case kControlExit:
        case kMenuExit:
            beginExit();
            break;
        case kMenuShow:
            showWindow();
            break;
        case kMenuEnable:
            setEnabled(true);
            break;
        case kMenuDisable:
            setEnabled(false);
            break;
        case kMenuRestart:
            blocked_ = false;
            if (process_) requestStop();
            else startChild();
            break;
        default:
            break;
        }
    }

    void setEnabled(bool enabled) {
        if (exitRequested_) return;
        if (enabled == desiredEnabled_) {
            updateControls();
            return;
        }
        std::wstring error;
        if (!axe::panel::settings::save(enabled, error)) {
            status_ = L"Could not save the routing preference; the previous setting remains active.";
            settingsNotice_ = error;
            updateControls();
            return;
        }

        settingsNotice_.clear();
        desiredEnabled_ = enabled;
        blocked_ = false;
        bridgeConflict_ = false;
        if (enabled) {
            if (process_) {
                status_ = stopping_ ? L"Restarting routing after the current session stops…"
                                   : L"Routing is enabled; the bridge is already running.";
            } else {
                startChild();
            }
        } else {
            requestStop();
            if (!process_) status_ = L"Routing disabled.";
        }
        updateControls();
    }

    void beginExit() {
        if (exitRequested_) return;
        exitRequested_ = true;
        requestStop();
        if (process_) {
            status_ = L"Stopping the audio bridge before exit…";
            updateControls();
        } else {
            DestroyWindow(window_);
        }
    }

    void requestStop() {
        if (!process_ || stopping_) return;
        if (stopEvent_ && !SetEvent(stopEvent_.get())) {
            latestDiagnostic_ = format_error(L"Could not signal a graceful bridge stop", GetLastError());
        }
        stopping_ = true;
        stopRequestedAt_ = std::chrono::steady_clock::now();
        status_ = L"Stopping audio routing…";
        updateControls();
    }

    std::wstring executablePath() const {
        std::wstring path(32768, L'\0');
        const DWORD count = GetModuleFileNameW(instance_, path.data(), static_cast<DWORD>(path.size()));
        if (count == 0 || count >= path.size())
            throw std::wstring(format_error(L"Could not find the panel executable path", GetLastError()));
        path.resize(count);
        const auto separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos) throw std::wstring(L"The panel executable has no directory path");
        path.resize(separator + 1);
        path += L"axe-usb78.exe";
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            throw std::wstring(L"Could not find axe-usb78.exe beside the control panel");
        return path;
    }

    void startChild() {
        if (!desiredEnabled_ || process_ || blocked_ || exitRequested_) return;
        status_ = L"Starting the Windows audio bridge…";
        latestDiagnostic_.clear();
        hasStats_ = false;
        updateControls();
        try {
            launchChildProcess();
        } catch (const std::wstring& error) {
            blocked_ = true;
            status_ = L"The audio bridge could not be started.";
            latestDiagnostic_ = error;
            updateControls();
        } catch (...) {
            blocked_ = true;
            status_ = L"The audio bridge could not be started due to an unexpected error.";
            updateControls();
        }
    }

    void launchChildProcess() {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE rawRead{}, rawWrite{};
        if (!CreatePipe(&rawRead, &rawWrite, &security, 0))
            throw std::wstring(format_error(L"Could not create the bridge status pipe", GetLastError()));
        UniqueHandle readPipe(rawRead);
        UniqueHandle writePipe(rawWrite);
        if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0))
            throw std::wstring(format_error(L"Could not protect the bridge status pipe", GetLastError()));

        UniqueHandle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!nullInput)
            throw std::wstring(format_error(L"Could not create the bridge input handle", GetLastError()));

        static std::atomic<std::uint64_t> eventSequence{0};
        const std::wstring eventName = L"Local\\AxeFxUsb78Stop-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(eventSequence.fetch_add(1, std::memory_order_relaxed));
        UniqueHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
        if (!stopEvent)
            throw std::wstring(format_error(L"Could not create the bridge stop event", GetLastError()));

        UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
        if (!job) throw std::wstring(format_error(L"Could not create the bridge job object", GetLastError()));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
        jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                     &jobLimits, sizeof(jobLimits))) {
            throw std::wstring(format_error(L"Could not configure bridge process cleanup", GetLastError()));
        }

        const std::wstring childPath = executablePath();
        std::wstring commandLine = quote_argument(childPath) + L" --run --stop-event " +
                                   quote_argument(eventName);
        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');
        const std::vector<HANDLE> inheritedHandles{nullInput.get(), writePipe.get()};
        AttributeList attributes(inheritedHandles);

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = nullInput.get();
        startup.StartupInfo.hStdOutput = writePipe.get();
        startup.StartupInfo.hStdError = writePipe.get();
        startup.lpAttributeList = attributes.get();
        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessW(childPath.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, nullptr, &startup.StartupInfo, &processInfo)) {
            throw std::wstring(format_error(L"Could not launch axe-usb78.exe", GetLastError()));
        }
        UniqueHandle process(processInfo.hProcess);
        UniqueHandle thread(processInfo.hThread);
        if (!AssignProcessToJobObject(job.get(), process.get())) {
            const DWORD error = GetLastError();
            TerminateProcess(process.get(), error);
            WaitForSingleObject(process.get(), 1000);
            throw std::wstring(format_error(L"Could not attach the bridge to its cleanup job", error));
        }
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            const DWORD error = GetLastError();
            TerminateJobObject(job.get(), error);
            WaitForSingleObject(process.get(), 1000);
            throw std::wstring(format_error(L"Could not start the bridge process", error));
        }

        writePipe.reset();
        nullInput.reset();
        process_ = std::move(process);
        job_ = std::move(job);
        stopEvent_ = std::move(stopEvent);
        pipeRead_ = std::move(readPipe);
        stopping_ = false;
        pipeEof_ = false;
        pendingLine_.clear();
        bridgeConflict_ = false;
        blocked_ = false;
        status_ = L"Starting the Windows audio bridge…";
        updateControls();
    }

    void handleOutputLine(std::string line) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) return;
        const std::wstring wide = utf8_to_wide(line);
        if (line.find("Routing ") == 0 && line.find(" -> Axe USB 7/8") != std::string::npos) {
            status_ = L"Routing is active through FAS ASIO USB 7/8.";
            long long periodFrames{};
            double queueMilliseconds{};
            if (parse_integer_after(line, "capture period ", periodFrames) &&
                parse_decimal_after(line, "queue target ", queueMilliseconds) &&
                periodFrames > 0 && queueMilliseconds > 0.0) {
                std::wostringstream info;
                info << L"WASAPI period: " << periodFrames << L" frames; queue target "
                     << std::fixed << std::setprecision(2) << queueMilliseconds << L" ms.";
                streamInfo_ = info.str();
            }
        } else if (line.find("Active ASIO buffer=") == 0) {
            long long bufferFrames{}, outputLatencyFrames{};
            if (parse_integer_after(line, "Active ASIO buffer=", bufferFrames) && bufferFrames > 0) {
                std::wostringstream info;
                info << L"ASIO: " << bufferFrames << L" frames ("
                     << std::fixed << std::setprecision(2)
                     << static_cast<double>(bufferFrames) / 48.0 << L" ms)";
                if (parse_integer_after(line, "driver output latency=", outputLatencyFrames) &&
                    outputLatencyFrames >= 0) {
                    info << L"; reported output latency " << outputLatencyFrames << L" frames / "
                         << static_cast<double>(outputLatencyFrames) / 48.0 << L" ms";
                }
                info << L'.';
                asioInfo_ = info.str();
            } else {
                asioInfo_ = L"FAS ASIO stream is active.";
            }
        } else if (line.find("Routing unavailable:") != std::string::npos) {
            latestDiagnostic_ = wide;
            bridgeConflict_ = line.find("Another Axe USB 7/8 bridge instance is running") != std::string::npos;
            status_ = bridgeConflict_
                ? L"Another bridge already owns the ASIO device. Stop it, then retry here."
                : L"The bridge could not open the audio route; see the reported reason.";
        } else if (line.find("captured=") != std::string::npos) {
            long long queued{}, silent{}, overflow{}, stale{}, interruptions{}, overloads{};
            if (parse_integer_after(line, "queued=", queued) && queued >= 0 &&
                parse_integer_after(line, "silent_frames=", silent) && silent >= 0 &&
                parse_integer_after(line, "overflow=", overflow) && overflow >= 0 &&
                parse_integer_after(line, "stale=", stale) && stale >= 0 &&
                parse_integer_after(line, "discontinuities=", interruptions) && interruptions >= 0 &&
                parse_integer_after(line, "asio_overloads=", overloads) && overloads >= 0) {
                std::wostringstream info;
                info << L"Buffered: " << std::fixed << std::setprecision(1)
                     << static_cast<double>(queued) / 48.0 << L" ms; interrupted: "
                     << silent << L" frames / " << interruptions << L" gaps; discarded: "
                     << (overflow + stale) << L" frames; ASIO overloads: " << overloads << L'.';
                statsLine_ = info.str();
                hasStats_ = true;
            } else {
                statsLine_ = L"Live audio statistics are unavailable.";
                hasStats_ = true;
            }
        } else if (line.find("Another Axe USB 7/8 bridge instance is running") != std::string::npos) {
            bridgeConflict_ = true;
            latestDiagnostic_ = wide;
            status_ = L"Another bridge already owns the ASIO device. Stop it, then retry here.";
        } else if (line.find("Routing stopped.") != std::string::npos) {
            if (!desiredEnabled_) status_ = L"Routing disabled.";
        } else if (line.find("failed") != std::string::npos || line.find("error") != std::string::npos) {
            latestDiagnostic_ = wide;
            status_ = L"The audio bridge reported an error; see details.";
        }
        updateControls();
    }

    void consumeBytes(const char* bytes, std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            const char character = bytes[index];
            if (character == '\n') {
                handleOutputLine(std::move(pendingLine_));
                pendingLine_.clear();
            } else if (pendingLine_.size() < 8192) {
                pendingLine_.push_back(character);
            } else if (character == '\r') {
                handleOutputLine(std::move(pendingLine_));
                pendingLine_.clear();
            }
        }
    }

    DWORD availablePipeBytes() {
        if (!pipeRead_ || pipeEof_) return 0;
        DWORD available{};
        if (!PeekNamedPipe(pipeRead_.get(), nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) pipeEof_ = true;
            else if (error != ERROR_NO_DATA) {
                latestDiagnostic_ = format_error(L"Could not poll bridge output", error);
                pipeEof_ = true;
            }
        }
        return available;
    }

    void drainPipe(DWORD byteBudget) {
        if (!pipeRead_ || pipeEof_) return;
        std::array<char, 4096> buffer{};
        DWORD consumed{};
        while (consumed < byteBudget) {
            const DWORD available = availablePipeBytes();
            if (available == 0) break;
            const DWORD requested = std::min<DWORD>(
                {available, static_cast<DWORD>(buffer.size()), byteBudget - consumed});
            DWORD read{};
            if (!ReadFile(pipeRead_.get(), buffer.data(), requested, &read, nullptr)) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) pipeEof_ = true;
                else latestDiagnostic_ = format_error(L"Could not read bridge output", error);
                break;
            }
            if (read == 0) {
                pipeEof_ = true;
                break;
            }
            consumeBytes(buffer.data(), read);
            consumed += read;
        }
    }

    void pollChild() {
        if (!process_) return;
        drainPipe(16 * 1024);
        if (stopping_ && std::chrono::steady_clock::now() - stopRequestedAt_ >
                         std::chrono::seconds(5) && job_) {
            TerminateJobObject(job_.get(), ERROR_CANCELLED);
            status_ = L"The bridge did not stop promptly; its child job was terminated safely.";
            stopping_ = false;
            updateControls();
        }
        if (WaitForSingleObject(process_.get(), 0) != WAIT_OBJECT_0) return;
        drainPipe(16 * 1024);
        const DWORD remaining = availablePipeBytes();
        if (remaining != 0) return;
        if (!pendingLine_.empty()) {
            handleOutputLine(std::move(pendingLine_));
            pendingLine_.clear();
        }
        DWORD exitCode = ERROR_GEN_FAILURE;
        GetExitCodeProcess(process_.get(), &exitCode);
        process_.reset();
        job_.reset();
        stopEvent_.reset();
        pipeRead_.reset();
        stopping_ = false;
        pipeEof_ = true;
        if (exitRequested_) {
            DestroyWindow(window_);
            return;
        }
        if (!desiredEnabled_) {
            status_ = L"Routing disabled.";
            blocked_ = false;
        } else if (!blocked_) {
            blocked_ = true;
            if (bridgeConflict_) {
                status_ = L"Another bridge is running. Stop it, then toggle routing off and on to retry.";
            } else {
                status_ = L"The audio bridge stopped (exit code " + std::to_wstring(exitCode) +
                          L"). Toggle routing off and on to retry.";
            }
        }
        updateControls();
        if (desiredEnabled_ && !blocked_) startChild();
    }

};

bool has_minimized_argument(int argc, wchar_t** argv, bool& showHelp, bool& shutdown) {
    bool minimized = false;
    for (int index = 1; index < argc; ++index) {
        if (std::wstring_view(argv[index]) == L"--minimized") minimized = true;
        else if (std::wstring_view(argv[index]) == L"--shutdown") shutdown = true;
        else if (std::wstring_view(argv[index]) == L"--help" || std::wstring_view(argv[index]) == L"-h") {
            showHelp = true;
        } else {
            throw std::wstring(L"Unknown option. Use --minimized, --shutdown, or --help.");
        }
    }
    return minimized;
}

bool shutdown_existing_panel() {
    HWND existing = FindWindowW(kWindowClass, kWindowTitle);
    if (!existing) return true;

    DWORD processId{};
    if (GetWindowThreadProcessId(existing, &processId) == 0 || processId == 0) {
        return !IsWindow(existing);
    }

    UniqueHandle process(OpenProcess(SYNCHRONIZE, FALSE, processId));
    if (!process) {
        if (!IsWindow(existing)) return true;
        throw std::wstring(format_error(L"Could not wait for the control panel to exit", GetLastError()));
    }

    DWORD_PTR result{};
    if (!SendMessageTimeoutW(existing, kShutdownMessage, 0, 0,
                             SMTO_ABORTIFHUNG, 5000, &result) &&
        WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) {
        return false;
    }
    return WaitForSingleObject(process.get(), 15000) == WAIT_OBJECT_0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    try {
        int argumentCount{};
        wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments) throw std::wstring(format_error(L"Could not read the command line", GetLastError()));
        bool minimized{};
        bool showHelp{};
        bool shutdown{};
        try {
            minimized = has_minimized_argument(argumentCount, arguments, showHelp, shutdown);
        } catch (...) {
            LocalFree(arguments);
            throw;
        }
        LocalFree(arguments);
        if (showHelp) {
            MessageBoxW(nullptr, L"Axe-Fx III USB 7/8 audio control panel\n\n"
                                 L"--minimized starts with the window hidden in the notification area.\n"
                                 L"--shutdown stops the running panel and its audio bridge.",
                        kWindowTitle, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (shutdown) return shutdown_existing_panel() ? 0 : 1;

        minimized = minimized || showCommand == SW_HIDE;
        HANDLE rawMutex = CreateMutexW(nullptr, TRUE, kPanelMutex);
        if (!rawMutex)
            throw std::wstring(format_error(L"Could not create the panel's single-instance lock", GetLastError()));
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(rawMutex);
            // Automatic launches must not bring an existing panel forward.
            if (minimized) return 0;
            HWND existing{};
            for (int attempt = 0; attempt < 40 && !existing; ++attempt) {
                existing = FindWindowW(kWindowClass, kWindowTitle);
                if (!existing) Sleep(50);
            }
            if (existing) PostMessageW(existing, kActivateMessage, 0, 0);
            return existing ? 0 : 1;
        }
        UniqueHandle mutex(rawMutex);

        TrayPanel panel(minimized);
        if (!panel.create(instance))
            throw std::wstring(format_error(L"Could not create the control panel window", GetLastError()));
        panel.finishInitialization();

        MSG message{};
        BOOL messageResult{};
        while ((messageResult = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(panel.window(), &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (messageResult == -1) return 1;
        return static_cast<int>(message.wParam);
    } catch (const std::wstring& error) {
        MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"The control panel stopped because of an unexpected error.",
                    kWindowTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
}
