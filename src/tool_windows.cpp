#include "tool_windows.h"
#include "papyrus_tools.h"
#include "plugin_tools.h"
#include "work_budget.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <strsafe.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "bsa_tools.h"
#include "dependents.h"
#include "property_window.h"

extern HINSTANCE g_hInst;

namespace pse {
namespace {

constexpr UINT WM_APP_HASH = WM_APP + 2;
constexpr int IDC_INPUT = 201, IDC_COMPARE = 202, IDC_CLOSE = 203, IDC_REPORT = 204;
constexpr UINT_PTR kTimerCopied = 1;
constexpr wchar_t kHashClass[] = L"PluginShellExtensionHashes";
constexpr wchar_t kReportClass[] = L"PluginShellExtensionReport";
constexpr wchar_t kNameClass[] = L"PluginShellExtensionFolderViewName";
constexpr wchar_t kDependentsClass[] = L"PluginShellExtensionDependents";
constexpr int IDC_LIST = 205, IDC_COPYLIST = 206;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;

int Scale(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }
HMENU CtrlId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

struct Theme {
    bool dark = false;
    COLORREF window, text, faint, border, hover, copied, matchBack, matchLine, badLine, inputBack;
};

Theme CurrentTheme() {
    Theme t;
    HIGHCONTRASTW hc{sizeof(hc)};
    const bool contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    wchar_t forced[16] = {};
    bool dark = light == 0;
    if (GetEnvironmentVariableW(L"PSE_THEME", forced, ARRAYSIZE(forced))) dark = _wcsicmp(forced, L"dark") == 0;
    if (contrast) {
        t.window = GetSysColor(COLOR_3DFACE);
        t.text = t.faint = GetSysColor(COLOR_WINDOWTEXT);
        t.border = GetSysColor(COLOR_WINDOWFRAME);
        t.hover = t.copied = t.matchBack = GetSysColor(COLOR_HIGHLIGHT);
        t.matchLine = GetSysColor(COLOR_HOTLIGHT);
        t.badLine = GetSysColor(COLOR_WINDOWTEXT);
        t.inputBack = GetSysColor(COLOR_WINDOW);
    } else if (dark) {
        t.dark = true;
        t.window = RGB(32, 32, 32);
        t.text = RGB(255, 255, 255);
        t.faint = RGB(170, 170, 170);
        t.border = RGB(86, 86, 86);
        t.hover = RGB(52, 52, 52);
        t.copied = RGB(36, 72, 118);
        t.matchBack = RGB(28, 86, 46);
        t.matchLine = RGB(76, 194, 112);
        t.badLine = RGB(232, 72, 72);
        t.inputBack = RGB(25, 25, 25);
    } else {
        t.window = GetSysColor(COLOR_3DFACE);
        t.text = GetSysColor(COLOR_WINDOWTEXT);
        t.faint = RGB(96, 96, 96);
        t.border = RGB(130, 135, 144);
        t.hover = RGB(229, 236, 247);
        t.copied = RGB(204, 228, 247);
        t.matchBack = RGB(198, 239, 206);
        t.matchLine = RGB(16, 124, 16);
        t.badLine = RGB(209, 17, 17);
        t.inputBack = GetSysColor(COLOR_WINDOW);
    }
    return t;
}

// Activates the DLL's own manifest (comctl32 v6) for the current thread.
class ActivationContext {
public:
    ActivationContext() {
        ACTCTXW act{sizeof(act)};
        act.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        act.hModule = g_hInst;
        act.lpResourceName = MAKEINTRESOURCEW(2);
        ctx_ = CreateActCtxW(&act);
        if (ctx_ != INVALID_HANDLE_VALUE && !ActivateActCtx(ctx_, &cookie_)) cookie_ = 0;
    }
    ~ActivationContext() {
        if (cookie_) DeactivateActCtx(0, cookie_);
        if (ctx_ != INVALID_HANDLE_VALUE) ReleaseActCtx(ctx_);
    }

private:
    HANDLE ctx_ = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie_ = 0;
};

HMODULE AddModuleRef() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&AddModuleRef), &module);
    return module;
}

HFONT MessageFont(UINT dpi) {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

void FillBox(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void DrawOutline(HDC dc, RECT r, COLORREF c, int width) {
    for (int i = 0; i < width; ++i) {
        HBRUSH b = CreateSolidBrush(c);
        FrameRect(dc, &r, b);
        DeleteObject(b);
        InflateRect(&r, -1, -1);
    }
}

int RunLoop(HWND hwnd) {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

void CenterOnCursorMonitor(HWND hwnd, int width, int height) {
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(hwnd, nullptr, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2, mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - height) / 2,
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---- The hash dialog ----

constexpr int kHashW = 640, kHashH = 300, kPad = 14, kRowsY = 40, kRowH = 30, kLabelW = 76, kInputH = 26, kCompareW = 92, kButtonH = 26;

struct HashJob {
    std::atomic<bool> cancel{false};
    std::mutex lock;
    FileHashes result;
    bool done = false;
    int percent = 0;
};

struct HashThreadArgs {
    std::wstring path;
    HWND hwnd;
    std::shared_ptr<HashJob> job;
    HMODULE module;
};

DWORD WINAPI HashThread(void* param) {
    std::unique_ptr<HashThreadArgs> a(static_cast<HashThreadArgs*>(param));
    int shown = -1;
    FileHashes r = ComputeHashes(a->path, &a->job->cancel, [&](uint64_t done, uint64_t total) {
        const int percent = total ? static_cast<int>(done * 100 / total) : 100;
        if (percent != shown) {
            shown = percent;
            {
                std::lock_guard<std::mutex> g(a->job->lock);
                a->job->percent = percent;
            }
            PostMessageW(a->hwnd, WM_APP_HASH, 0, 0);
        }
        return !a->job->cancel.load();
    });
    {
        std::lock_guard<std::mutex> g(a->job->lock);
        a->job->result = std::move(r);
        a->job->done = true;
    }
    PostMessageW(a->hwnd, WM_APP_HASH, 1, 0);
    HMODULE module = a->module;
    a.reset();
    FreeLibraryAndExitThread(module, 0);
}

class HashWindow {
public:
    HashWindow(std::wstring path, std::wstring png, std::wstring script) : path_(std::move(path)), png_(std::move(png)), script_(std::move(script)) {}

    static void Register() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &HashWindow::StaticProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kHashClass;
        RegisterClassExW(&wc);
    }

    HWND Create() {
        const std::wstring title = std::wstring(PathFindFileNameW(path_.c_str())) + L" Hashes";
        return CreateWindowExW(WS_EX_CONTROLPARENT, kHashClass, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, g_hInst, this);
    }

private:
    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<HashWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<HashWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Proc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    int S(int v) const { return Scale(v, dpi_); }
    RECT RowRect(int i) const { return {S(kPad), S(kRowsY + kRowH * i), Width() - S(kPad), S(kRowsY + kRowH * (i + 1))}; }
    int Width() const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        return rc.right;
    }
    int InputY() const { return S(kRowsY + kRowH * 3 + 34); }
    RECT InputOutline() const { return {S(kPad) - 2, InputY() - 2, Width() - S(kPad) - S(kCompareW) - S(8) + 2, InputY() + S(kInputH) + 2}; }
    const std::wstring& Value(int i) const { return i == 0 ? hashes_.sha256 : i == 1 ? hashes_.crc32 : hashes_.md5; }
    static const wchar_t* Label(int i) { return i == 0 ? L"SHA-256" : i == 1 ? L"CRC-32" : L"MD5"; }

    int RowAt(int x, int y) const {
        if (!hashes_.ok) return -1;
        for (int i = 0; i < 3; ++i) {
            RECT r = RowRect(i);
            r.left += S(kLabelW);
            POINT p{x, y};
            if (PtInRect(&r, p)) return i;
        }
        return -1;
    }

    void Copy(int row) {
        const std::wstring& text = Value(row);
        if (text.empty() || !OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            memcpy(GlobalLock(g), text.c_str(), bytes);
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        }
        CloseClipboard();
        copied_ = row;
        SetTimer(hwnd_, kTimerCopied, 1400, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Compare() {
        if (!hashes_.ok) return;
        wchar_t text[512] = {};
        GetWindowTextW(input_, text, ARRAYSIZE(text));
        std::wstring value = text;
        const size_t a = value.find_first_not_of(L" \t\r\n"), b = value.find_last_not_of(L" \t\r\n");
        value = a == std::wstring::npos ? L"" : value.substr(a, b - a + 1);
        matched_ = -1;
        mismatch_ = false;
        if (value.empty()) {
            status_ = L"Paste a hash first.";
        } else {
            for (int i = 0; i < 3 && matched_ < 0; ++i)
                if (_wcsicmp(value.c_str(), Value(i).c_str()) == 0) matched_ = i;
            mismatch_ = matched_ < 0;
            status_ = mismatch_ ? L"No match." : std::wstring(L"Matches ") + Label(matched_) + L".";
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Paint(HDC dc) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        FillBox(dc, rc, theme_.window);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, font_);
        SetTextColor(dc, theme_.faint);
        RECT note{S(kPad), S(12), rc.right - S(kPad), S(34)};
        DrawTextW(dc, L"Click a hash value to copy", -1, &note, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        for (int i = 0; i < 3; ++i) {
            RECT row = RowRect(i);
            if (matched_ == i) FillBox(dc, row, theme_.matchBack);
            else if (copied_ == i) FillBox(dc, row, theme_.copied);
            else if (hot_ == i) FillBox(dc, row, theme_.hover);
            SelectObject(dc, font_);
            SetTextColor(dc, theme_.text);
            RECT label = row;
            label.left += S(6);
            label.right = row.left + S(kLabelW);
            DrawTextW(dc, Label(i), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            RECT value = row;
            value.left += S(kLabelW);
            std::wstring text;
            if (hashes_.ok) {
                text = Value(i);
                SelectObject(dc, mono_);
            } else if (!hashes_.error.empty()) {
                text = i == 0 ? hashes_.error : L"";
            } else {
                text = L"Calculating… " + std::to_wstring(percent_) + L"%";
                SetTextColor(dc, theme_.faint);
            }
            DrawTextW(dc, text.c_str(), -1, &value, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (copied_ == i) {
                SelectObject(dc, font_);
                SetTextColor(dc, theme_.text);
                RECT tag = row;
                tag.right -= S(8);
                DrawTextW(dc, L"Copied", -1, &tag, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
            }
        }
        SelectObject(dc, font_);
        SetTextColor(dc, theme_.faint);
        RECT prompt{S(kPad), InputY() - S(24), rc.right - S(kPad), InputY() - S(3)};
        DrawTextW(dc, L"Paste a hash to compare:", -1, &prompt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        const COLORREF line = mismatch_ ? theme_.badLine : matched_ >= 0 ? theme_.matchLine : theme_.border;
        DrawOutline(dc, InputOutline(), line, mismatch_ || matched_ >= 0 ? 2 : 1);
        SetTextColor(dc, mismatch_ ? theme_.badLine : matched_ >= 0 ? theme_.matchLine : theme_.faint);
        RECT status{S(kPad), InputY() + S(kInputH) + S(6), rc.right - S(kPad) - S(110), InputY() + S(kInputH) + S(30)};
        DrawTextW(dc, status_.c_str(), -1, &status, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }

    void Layout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int compareX = rc.right - S(kPad) - S(kCompareW);
        MoveWindow(input_, S(kPad) + S(6), InputY() + S(5), compareX - S(8) - S(kPad) - S(12), S(kInputH) - S(8), TRUE);
        MoveWindow(compare_, compareX, InputY(), S(kCompareW), S(kInputH), TRUE);
        MoveWindow(close_, rc.right - S(kPad) - S(kCompareW), rc.bottom - S(kPad) - S(kButtonH), S(kCompareW), S(kButtonH), TRUE);
    }

    void ApplyDpi(UINT dpi) {
        dpi_ = dpi;
        HFONT oldFont = font_, oldMono = mono_;
        font_ = MessageFont(dpi_);
        mono_ = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        for (HWND h : {input_, compare_, close_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(input_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_), TRUE);
        if (oldFont) DeleteObject(oldFont);
        if (oldMono) DeleteObject(oldMono);
    }

    void ApplyTheme() {
        theme_ = CurrentTheme();
        if (inputBrush_) DeleteObject(inputBrush_);
        inputBrush_ = CreateSolidBrush(theme_.inputBack);
        BOOL dark = theme_.dark;
        DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
        for (HWND h : {input_, compare_, close_}) SetWindowTheme(h, theme_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    void StartHashing() {
        auto* args = new HashThreadArgs{path_, hwnd_, job_, AddModuleRef()};
        if (HANDLE t = CreateThread(nullptr, 0, &HashThread, args, 0, nullptr)) {
            CloseHandle(t);
            return;
        }
        if (args->module) FreeLibrary(args->module);
        delete args;
        hashes_.error = L"Could not start hashing.";
    }

    void PlayScriptAndSave() {
        std::wstring s = script_, log;
        for (size_t p = 0; p <= s.size();) {
            size_t bar = s.find(L'|', p);
            if (bar == std::wstring::npos) bar = s.size();
            const std::wstring step = s.substr(p, bar - p);
            p = bar + 1;
            if (step.rfind(L"copy:", 0) == 0) {
                Copy(_wtoi(step.c_str() + 5));
            } else if (step.rfind(L"compare:", 0) == 0) {
                std::wstring value = step.substr(8);
                if (value == L"sha") value = hashes_.sha256;
                else if (value == L"crc") value = hashes_.crc32;
                else if (value == L"MD5") { value = hashes_.md5; CharUpperBuffW(value.data(), static_cast<DWORD>(value.size())); }
                SetWindowTextW(input_, value.c_str());
                Compare();
            }
            log += step + L"\t" + status_ + L"\tmatched=" + std::to_wstring(matched_) + L"\tmismatch=" + std::to_wstring(mismatch_) + L"\tcopied=" + std::to_wstring(copied_) + L"\r\n";
        }
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        SaveWindowPng(hwnd_, png_, nullptr, nullptr);
        std::wstring text = L"sha256\t" + hashes_.sha256 + L"\r\ncrc32\t" + hashes_.crc32 + L"\r\nmd5\t" + hashes_.md5 + L"\r\nerror\t" + hashes_.error + L"\r\n" + log;
        if (OpenClipboard(hwnd_)) {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                text += L"clipboard\t";
                text += static_cast<const wchar_t*>(GlobalLock(data));
                GlobalUnlock(data);
                text += L"\r\n";
            }
            CloseClipboard();
        }
        HANDLE f = CreateFileW((png_ + L".txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(f, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &wrote, nullptr);
            CloseHandle(f);
        }
        DestroyWindow(hwnd_);
    }

    void OnHashUpdate(bool finished) {
        bool done = false;
        {
            std::lock_guard<std::mutex> g(job_->lock);
            percent_ = job_->percent;
            if (finished && job_->done) {
                hashes_ = job_->result;
                done = true;
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (done) {
            EnableWindow(compare_, hashes_.ok);
            EnableWindow(input_, hashes_.ok);
            if (!png_.empty()) PlayScriptAndSave();
        }
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                dpi_ = GetDpiForWindow(hwnd_);
                input_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, CtrlId(IDC_INPUT), g_hInst, nullptr);
                compare_ = CreateWindowExW(0, L"BUTTON", L"Compare", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, hwnd_, CtrlId(IDC_COMPARE), g_hInst, nullptr);
                close_ = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd_, CtrlId(IDC_CLOSE), g_hInst, nullptr);
                ApplyDpi(dpi_);
                RECT r{0, 0, S(kHashW), S(kHashH)};
                AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)), dpi_);
                CenterOnCursorMonitor(hwnd_, r.right - r.left, r.bottom - r.top);
                Layout();
                ApplyTheme();
                EnableWindow(compare_, FALSE);
                EnableWindow(input_, FALSE);
                StartHashing();
                return 0;
            }
            case WM_APP_HASH:
                OnHashUpdate(wp != 0);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                RECT rc;
                GetClientRect(hwnd_, &rc);
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                HGDIOBJ oldBmp = SelectObject(mem, bmp);
                HGDIOBJ oldFont = SelectObject(mem, font_);
                Paint(mem);
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, oldFont);
                SelectObject(mem, oldBmp);
                DeleteObject(bmp);
                DeleteDC(mem);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_PRINTCLIENT:
                Paint(reinterpret_cast<HDC>(wp));
                return 0;
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORSTATIC:
                SetTextColor(reinterpret_cast<HDC>(wp), theme_.text);
                SetBkColor(reinterpret_cast<HDC>(wp), theme_.inputBack);
                return reinterpret_cast<LRESULT>(inputBrush_);
            case WM_MOUSEMOVE: {
                const int hot = RowAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (hot != hot_) {
                    hot_ = hot;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    if (hot_ >= 0) {
                        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
                        TrackMouseEvent(&t);
                    }
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                hot_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT && hot_ >= 0) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }
                break;
            case WM_LBUTTONUP: {
                const int row = RowAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (row >= 0) Copy(row);
                return 0;
            }
            case WM_TIMER:
                if (wp == kTimerCopied) {
                    KillTimer(hwnd_, kTimerCopied);
                    copied_ = -1;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            case WM_COMMAND:
                if (LOWORD(wp) == IDC_COMPARE || LOWORD(wp) == IDOK) Compare();
                else if (LOWORD(wp) == IDC_CLOSE || LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd_);
                else if (LOWORD(wp) == IDC_INPUT && HIWORD(wp) == EN_CHANGE && (mismatch_ || matched_ >= 0)) {
                    mismatch_ = false;  // the old result no longer describes what's typed
                    matched_ = -1;
                    status_.clear();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            case WM_DPICHANGED: {
                ApplyDpi(HIWORD(wp));
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                Layout();
                return 0;
            }
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED:
                ApplyTheme();
                break;
            case WM_DESTROY:
                job_->cancel = true;
                PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY:
                if (font_) DeleteObject(font_);
                if (mono_) DeleteObject(mono_);
                if (inputBrush_) DeleteObject(inputBrush_);
                font_ = mono_ = nullptr;
                inputBrush_ = nullptr;
                SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    std::wstring path_, png_, script_, status_;
    HWND hwnd_ = nullptr, input_ = nullptr, compare_ = nullptr, close_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr, mono_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    Theme theme_ = CurrentTheme();
    FileHashes hashes_;
    int percent_ = 0, hot_ = -1, copied_ = -1, matched_ = -1;
    bool mismatch_ = false;
    std::shared_ptr<HashJob> job_ = std::make_shared<HashJob>();
};

std::mutex g_classLock;
int g_hashUsers = 0, g_reportUsers = 0;

struct DpiScope {
    DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope() {
        if (old) SetThreadDpiAwarenessContext(old);
    }
};

struct HashWindowArgs {
    std::wstring path;
    HMODULE module;
};

DWORD WINAPI HashWindowThread(void* param) {
    std::unique_ptr<HashWindowArgs> a(static_cast<HashWindowArgs*>(param));
    RunHashWindow(a->path, L"", L"");
    HMODULE module = a->module;
    a.reset();
    FreeLibraryAndExitThread(module, 0);
}

// ---- The test report window ----

class ReportWindow {
public:
    ReportWindow(std::wstring title, int status, std::wstring text, std::wstring png) : title_(std::move(title)), status_(status), text_(std::move(text)), png_(std::move(png)) {}

    static void Register() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &ReportWindow::StaticProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kReportClass;
        RegisterClassExW(&wc);
    }

    HWND Create() { return CreateWindowExW(WS_EX_CONTROLPARENT, kReportClass, title_.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, g_hInst, this); }

private:
    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<ReportWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<ReportWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Proc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    int S(int v) const { return Scale(v, dpi_); }

    void Layout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        MoveWindow(edit_, S(kPad), S(48), rc.right - S(2 * kPad), rc.bottom - S(48) - S(kPad) - S(kButtonH) - S(10), TRUE);
        MoveWindow(close_, rc.right - S(kPad) - S(kCompareW), rc.bottom - S(kPad) - S(kButtonH), S(kCompareW), S(kButtonH), TRUE);
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                dpi_ = GetDpiForWindow(hwnd_);
                edit_ = CreateWindowExW(0, L"EDIT", text_.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_,
                                        CtrlId(IDC_REPORT), g_hInst, nullptr);
                SendMessageW(edit_, EM_SETLIMITTEXT, 0, 0);
                SetWindowTextW(edit_, text_.c_str());
                close_ = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, hwnd_, CtrlId(IDC_CLOSE), g_hInst, nullptr);
                font_ = MessageFont(dpi_);
                bold_ = nullptr;
                LOGFONTW lf{};
                GetObjectW(font_, sizeof(lf), &lf);
                lf.lfWeight = FW_SEMIBOLD;
                lf.lfHeight = MulDiv(lf.lfHeight, 4, 3);
                bold_ = CreateFontIndirectW(&lf);
                mono_ = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
                SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_), TRUE);
                SendMessageW(close_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                theme_ = CurrentTheme();
                inputBrush_ = CreateSolidBrush(theme_.inputBack);
                BOOL dark = theme_.dark;
                DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
                for (HWND h : {edit_, close_}) SetWindowTheme(h, theme_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                CenterOnCursorMonitor(hwnd_, S(780), S(560));
                Layout();
                if (!png_.empty()) PostMessageW(hwnd_, WM_APP_HASH, 0, 0);
                return 0;
            }
            case WM_APP_HASH:
                RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                SaveWindowPng(hwnd_, png_, nullptr, nullptr);
                DestroyWindow(hwnd_);
                return 0;
            case WM_SIZE:
                Layout();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                PaintHeading(dc);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_PRINTCLIENT:
                PaintHeading(reinterpret_cast<HDC>(wp));
                return 0;
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORSTATIC:
                SetTextColor(reinterpret_cast<HDC>(wp), theme_.text);
                SetBkColor(reinterpret_cast<HDC>(wp), theme_.inputBack);
                return reinterpret_cast<LRESULT>(inputBrush_);
            case WM_COMMAND:
                if (LOWORD(wp) == IDC_CLOSE || LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY:
                for (HFONT f : {font_, bold_, mono_})
                    if (f) DeleteObject(f);
                if (inputBrush_) DeleteObject(inputBrush_);
                SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void PaintHeading(HDC dc) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        FillBox(dc, rc, theme_.window);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, bold_);
        SetTextColor(dc, status_ == 0 ? theme_.matchLine : status_ == 1 ? RGB(202, 138, 4) : theme_.badLine);
        RECT r{S(kPad), S(10), rc.right - S(kPad), S(42)};
        const wchar_t* text = status_ == 0 ? L"Passed" : status_ == 1 ? L"Passed with warnings" : status_ == 2 ? L"Failed" : L"Couldn't be opened";
        DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    std::wstring title_, text_, png_;
    int status_;
    HWND hwnd_ = nullptr, edit_ = nullptr, close_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr, bold_ = nullptr, mono_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    Theme theme_ = CurrentTheme();
};

// ---- The dependents list ----

class DependentsWindow {
public:
    DependentsWindow(std::wstring path, std::wstring png) : path_(std::move(path)), png_(std::move(png)) {
        name_ = PathFindFileNameW(path_.c_str());
        list_ = DependentsOf(path_);
    }

    static void Register() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &DependentsWindow::StaticProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kDependentsClass;
        RegisterClassExW(&wc);
    }

    HWND Create() {
        const std::wstring title = name_ + L" dependents";
        return CreateWindowExW(WS_EX_CONTROLPARENT, kDependentsClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, g_hInst, this);
    }

    // One plugin name per line.
    std::wstring ListText() const {
        std::wstring text;
        for (const Dependent& d : list_.plugins) text += d.name + L"\r\n";
        return text;
    }

private:
    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<DependentsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<DependentsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Proc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    // The column header draws its text black even in dark mode: colour it here.
    static LRESULT CALLBACK ListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        if (msg == WM_NOTIFY) {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(h)) {
                auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
                if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                    SetTextColor(cd->hdc, reinterpret_cast<DependentsWindow*>(ref)->theme_.text);
                    return CDRF_DODEFAULT;
                }
            }
        }
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, &DependentsWindow::ListProc, 1);
        return DefSubclassProc(h, msg, wp, lp);
    }

    int S(int v) const { return Scale(v, dpi_); }
    int Top() const { return S(list_.gameData ? 44 : 64); }

    void Layout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int bottom = rc.bottom - S(kPad) - S(kButtonH);
        MoveWindow(listView_, S(kPad), Top(), rc.right - S(2 * kPad), bottom - S(10) - Top(), TRUE);
        MoveWindow(close_, rc.right - S(kPad) - S(kCompareW), bottom, S(kCompareW), S(kButtonH), TRUE);
        MoveWindow(copy_, rc.right - S(kPad) - S(2 * kCompareW) - S(8), bottom, S(kCompareW), S(kButtonH), TRUE);
        RECT lr;
        GetClientRect(listView_, &lr);
        ListView_SetColumnWidth(listView_, 0, (std::max)(S(100), static_cast<int>(lr.right) - S(64)));
    }

    void Fill() {
        // A list view's first column can't be centred, so Active is added second
        // and shown first.
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = LVCFMT_LEFT;
        col.cx = S(400);
        col.pszText = const_cast<wchar_t*>(L"Dependents");
        ListView_InsertColumn(listView_, 0, &col);
        col.fmt = LVCFMT_CENTER;
        col.cx = S(64);
        col.pszText = const_cast<wchar_t*>(L"Active");
        ListView_InsertColumn(listView_, 1, &col);
        int order[] = {1, 0};
        ListView_SetColumnOrderArray(listView_, 2, order);
        for (size_t i = 0; i < list_.plugins.size(); ++i) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<wchar_t*>(list_.plugins[i].name.c_str());
            ListView_InsertItem(listView_, &item);
            ListView_SetItemText(listView_, static_cast<int>(i), 1, const_cast<wchar_t*>(list_.plugins[i].active ? L"A" : L"-"));
        }
    }

    void CopyList() {
        const std::wstring text = ListText();
        if (!OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            memcpy(GlobalLock(mem), text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        }
        CloseClipboard();
        SetWindowTextW(copy_, L"Copied");
        SetTimer(hwnd_, kTimerCopied, 1500, nullptr);
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                dpi_ = GetDpiForWindow(hwnd_);
                theme_ = CurrentTheme();
                listView_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, 0, 0, 10, 10, hwnd_,
                                            CtrlId(IDC_LIST), g_hInst, nullptr);
                ListView_SetExtendedListViewStyle(listView_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                copy_ = CreateWindowExW(0, L"BUTTON", L"Copy list", WS_CHILD | WS_VISIBLE | WS_TABSTOP | (list_.plugins.empty() ? WS_DISABLED : 0), 0, 0, 10, 10, hwnd_, CtrlId(IDC_COPYLIST), g_hInst, nullptr);
                close_ = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, hwnd_, CtrlId(IDC_CLOSE), g_hInst, nullptr);
                font_ = MessageFont(dpi_);
                LOGFONTW lf{};
                GetObjectW(font_, sizeof(lf), &lf);
                lf.lfWeight = FW_SEMIBOLD;
                lf.lfHeight = MulDiv(lf.lfHeight, 4, 3);
                bold_ = CreateFontIndirectW(&lf);
                for (HWND h : {listView_, copy_, close_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                BOOL dark = theme_.dark;
                DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
                for (HWND h : {listView_, copy_, close_}) SetWindowTheme(h, theme_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                if (HWND header = ListView_GetHeader(listView_)) SetWindowTheme(header, theme_.dark ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
                SetWindowSubclass(listView_, &DependentsWindow::ListProc, 1, reinterpret_cast<DWORD_PTR>(this));
                ListView_SetBkColor(listView_, theme_.inputBack);
                ListView_SetTextBkColor(listView_, theme_.inputBack);
                ListView_SetTextColor(listView_, theme_.text);
                Fill();
                CenterOnCursorMonitor(hwnd_, S(560), S(520));
                Layout();
                if (!png_.empty()) PostMessageW(hwnd_, WM_APP_HASH, 0, 0);
                return 0;
            }
            case WM_APP_HASH:
                RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                SaveWindowPng(hwnd_, png_, nullptr, nullptr);
                DestroyWindow(hwnd_);
                return 0;
            case WM_TIMER:
                if (wp == kTimerCopied) {
                    KillTimer(hwnd_, kTimerCopied);
                    SetWindowTextW(copy_, L"Copy list");
                }
                return 0;
            case WM_SIZE:
                Layout();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                mm->ptMinTrackSize = {S(360), S(260)};
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                PaintHeading(dc);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_PRINTCLIENT:
                PaintHeading(reinterpret_cast<HDC>(wp));
                return 0;
            case WM_COMMAND:
                if (LOWORD(wp) == IDC_COPYLIST) CopyList();
                else if (LOWORD(wp) == IDC_CLOSE || LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY:
                for (HFONT f : {font_, bold_})
                    if (f) DeleteObject(f);
                SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void PaintHeading(HDC dc) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        FillBox(dc, rc, theme_.window);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, bold_);
        SetTextColor(dc, theme_.text);
        const size_t n = list_.plugins.size();
        const std::wstring head = !list_.ok ? L"The folder couldn't be read"
                                  : n == 0  ? L"No plugin in this folder has " + name_ + L" as a master"
                                            : std::to_wstring(n) + (n == 1 ? L" plugin has " : L" plugins have ") + name_ + L" as a master";
        RECT r{S(kPad), S(8), rc.right - S(kPad), S(40)};
        DrawTextW(dc, head.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!list_.gameData) {
            SelectObject(dc, font_);
            SetTextColor(dc, theme_.faint);
            RECT r2{S(kPad), S(38), rc.right - S(kPad), S(58)};
            DrawTextW(dc, L"Not the game's Data folder, so plugins.txt doesn't apply: none is shown as active.", -1, &r2, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        SelectObject(dc, old);
    }

    std::wstring path_, png_, name_;
    DependentList list_;
    HWND hwnd_ = nullptr, listView_ = nullptr, copy_ = nullptr, close_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr, bold_ = nullptr;
    Theme theme_ = CurrentTheme();
};

int g_dependentsUsers = 0;

struct DependentsArgs {
    std::wstring path;
    HMODULE module;
};

DWORD WINAPI DependentsThread(void* param) {
    std::unique_ptr<DependentsArgs> a(static_cast<DependentsArgs*>(param));
    RunDependentsWindow(a->path, L"");
    HMODULE module = a->module;
    a.reset();
    FreeLibraryAndExitThread(module, 0);
}

// ---- Extract and test, on their own threads ----

class ProgressBox {
public:
    explicit ProgressBox(const wchar_t* title) {
        if (FAILED(CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog_)))) return;
        dialog_->SetTitle(title);
        dialog_->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME, nullptr);
    }
    ~ProgressBox() {
        if (dialog_) {
            dialog_->StopProgressDialog();
            dialog_->Release();
        }
    }
    void Line(const std::wstring& text) {
        if (dialog_) dialog_->SetLine(1, text.c_str(), TRUE, nullptr);
    }
    bool Update(uint64_t done, uint64_t total) {
        if (!dialog_) return true;
        dialog_->SetProgress64(done, total);
        return !dialog_->HasUserCancelled();
    }

private:
    IProgressDialog* dialog_ = nullptr;
};

std::wstring FileNameOf(const std::wstring& path) { return PathFindFileNameW(path.c_str()); }

std::wstring FolderOf(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }

std::wstring StemOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    const size_t dot = name.rfind(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

struct ActionArgs {
    std::vector<std::wstring> paths;
    bool intoOwnFolder = false;
    HMODULE module = nullptr;
    std::wstring target;  // extraction only; empty means the archive's own folder
};

DWORD WINAPI ExtractThread(void* param) {
    std::unique_ptr<ActionArgs> a(static_cast<ActionArgs*>(param));
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        ActivationContext act;
        DpiScope dpi;
        for (const auto& path : a->paths) {
            std::wstring base = a->target.empty() ? FolderOf(path) : a->target;
            // A drive root's path already ends in a backslash.
            if (!base.empty() && base.back() == L'\\') base.pop_back();
            const std::wstring destination = a->intoOwnFolder ? base + L"\\" + StemOf(path) : base;
            ExistingFiles existing = ExistingFiles::Replace;
            const uint64_t already = CountExisting(path, destination);
            if (already) {
                const std::wstring ask = std::to_wstring(already) + L" of the files in " + FileNameOf(path) + L" already exist in\r\n" + destination +
                                         L"\r\n\r\nReplace them? Choose No to keep the existing files and extract only the rest.";
                const int choice = MessageBoxW(nullptr, ask.c_str(), L"BSA manager", MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
                if (choice == IDCANCEL) continue;
                if (choice == IDNO) existing = ExistingFiles::Skip;
            }
            ExtractResult r;
            {
                ProgressBox box(L"Extracting");
                box.Line(FileNameOf(path));
                r = ExtractBsa(path, destination, existing, [&](uint64_t done, uint64_t total) { return box.Update(done, total); });
            }
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, destination.c_str(), nullptr);
            if (r.cancelled) continue;
            if (!r.opened || r.failed || r.skippedNames) {
                std::wstring text = !r.opened ? r.firstError : std::to_wstring(r.written) + L" files were extracted from " + FileNameOf(path) + L".";
                if (r.failed) text += L"\r\n\r\n" + std::to_wstring(r.failed) + L" couldn't be extracted. The first was:\r\n" + r.firstError;
                if (r.skippedNames) text += L"\r\n\r\n" + std::to_wstring(r.skippedNames) + L" were left out because their names aren't safe to write.";
                MessageBoxW(nullptr, text.c_str(), L"BSA manager", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    HMODULE module = a->module;
    a.reset();
    FreeLibraryAndExitThread(module, 0);
}

DWORD WINAPI TestThread(void* param) {
    std::unique_ptr<ActionArgs> a(static_cast<ActionArgs*>(param));
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        ActivationContext act;
        DpiScope dpi;
        for (const auto& path : a->paths) {
            BsaTestReport report;
            {
                ProgressBox box(L"Testing");
                box.Line(FileNameOf(path));
                report = TestBsa(path, [&](uint64_t done, uint64_t total) { return box.Update(done, total); });
            }
            if (!report.cancelled) RunReportWindow(FileNameOf(path) + L" test", report.status, report.text, L"");
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    HMODULE module = a->module;
    a.reset();
    FreeLibraryAndExitThread(module, 0);
}

// ---- The name box for Save current view as ----

class NameWindow {
public:
    explicit NameWindow(std::wstring png) : png_(std::move(png)) {}

    static void Register() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &NameWindow::StaticProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kNameClass;
        RegisterClassExW(&wc);
    }

    HWND Create() { return CreateWindowExW(WS_EX_CONTROLPARENT, kNameClass, L"Save current view", WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, g_hInst, this); }
    const std::wstring& Name() const { return name_; }

private:
    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<NameWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<NameWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Proc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    int S(int v) const { return Scale(v, dpi_); }

    void Paint(HDC dc) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        HBRUSH back = CreateSolidBrush(theme_.window);
        FillRect(dc, &rc, back);
        DeleteObject(back);
        HGDIOBJ old = SelectObject(dc, font_);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, theme_.text);
        RECT text{S(kPad), S(kPad), rc.right - S(kPad), S(kPad) + S(22)};
        DrawTextW(dc, L"Name for this view (a preset's name replaces that preset):", -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    void UpdateSave() {
        wchar_t text[64] = {};
        GetWindowTextW(edit_, text, ARRAYSIZE(text));
        bool any = false;
        for (const wchar_t* p = text; *p; ++p) any = any || !iswspace(*p);
        EnableWindow(save_, any);
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                dpi_ = GetDpiForWindow(hwnd_);
                theme_ = CurrentTheme();
                font_ = MessageFont(dpi_);
                inputBrush_ = CreateSolidBrush(theme_.inputBack);
                edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, S(kPad), S(kPad + 26), S(400), S(kInputH), hwnd_, CtrlId(IDC_INPUT), g_hInst, nullptr);
                SendMessageW(edit_, EM_SETLIMITTEXT, 60, 0);
                save_ = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED | BS_DEFPUSHBUTTON, S(kPad + 400 - 2 * kCompareW - 8), S(kPad + 26 + kInputH + 14), S(kCompareW), S(kButtonH), hwnd_, CtrlId(IDOK), g_hInst, nullptr);
                cancel_ = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(kPad + 400 - kCompareW), S(kPad + 26 + kInputH + 14), S(kCompareW), S(kButtonH), hwnd_, CtrlId(IDCANCEL), g_hInst, nullptr);
                for (HWND h : {edit_, save_, cancel_}) {
                    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                    SetWindowTheme(h, theme_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                }
                BOOL dark = theme_.dark;
                DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
                RECT r{0, 0, S(428), S(kPad + 26 + kInputH + 14 + kButtonH + kPad)};
                AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_CONTROLPARENT, dpi_);
                CenterOnCursorMonitor(hwnd_, r.right - r.left, r.bottom - r.top);
                SetFocus(edit_);
                if (!png_.empty()) PostMessageW(hwnd_, WM_APP_HASH, 0, 0);
                return 0;
            }
            case WM_APP_HASH:
                RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                SaveWindowPng(hwnd_, png_, nullptr, nullptr);
                DestroyWindow(hwnd_);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                Paint(dc);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_PRINTCLIENT:
                Paint(reinterpret_cast<HDC>(wp));
                return 0;
            case WM_CTLCOLOREDIT:
                SetTextColor(reinterpret_cast<HDC>(wp), theme_.text);
                SetBkColor(reinterpret_cast<HDC>(wp), theme_.inputBack);
                return reinterpret_cast<LRESULT>(inputBrush_);
            case WM_COMMAND:
                if (LOWORD(wp) == IDC_INPUT && HIWORD(wp) == EN_CHANGE) {
                    UpdateSave();
                } else if (LOWORD(wp) == IDOK && IsWindowEnabled(save_)) {
                    wchar_t text[64] = {};
                    GetWindowTextW(edit_, text, ARRAYSIZE(text));
                    name_ = text;
                    while (!name_.empty() && iswspace(name_.back())) name_.pop_back();
                    size_t start = 0;
                    while (start < name_.size() && iswspace(name_[start])) ++start;
                    name_.erase(0, start);
                    DestroyWindow(hwnd_);
                } else if (LOWORD(wp) == IDCANCEL) {
                    DestroyWindow(hwnd_);
                }
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY:
                if (font_) DeleteObject(font_);
                if (inputBrush_) DeleteObject(inputBrush_);
                SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    HWND hwnd_ = nullptr, edit_ = nullptr, save_ = nullptr, cancel_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    UINT dpi_ = 96;
    Theme theme_ = CurrentTheme();
    std::wstring png_, name_;
};

int g_nameUsers = 0;

// Runs a job that talks to the user on its own thread.
struct UserJob {
    std::function<void()> work;
    HMODULE module = nullptr;
};

DWORD WINAPI UserJobThread(void* param) {
    std::unique_ptr<UserJob> job(static_cast<UserJob*>(param));
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        ActivationContext act;
        DpiScope dpi;
        try {
            job->work();
        } catch (...) {
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    HMODULE module = job->module;
    job.reset();
    FreeLibraryAndExitThread(module, 0);
}

void StartUserJob(std::function<void()> work) {
    auto* job = new UserJob{std::move(work), AddModuleRef()};
    if (HANDLE t = CreateThread(nullptr, 0, &UserJobThread, job, 0, nullptr)) {
        CloseHandle(t);
        return;
    }
    if (job->module) FreeLibrary(job->module);
    delete job;
}

// Decompiling and compiling recurse with the depth of the script, and they
// run inside Explorer, so they get far more stack than any real script needs.
DWORD WINAPI PapyrusThread(void* param) {
    std::unique_ptr<ActionArgs> a(static_cast<ActionArgs*>(param));
    const bool compile = a->intoOwnFolder;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        ActivationContext act;
        DpiScope dpi;
        const wchar_t* const title = compile ? L"Compile" : L"Decompile";
        std::vector<std::pair<std::wstring, std::wstring>> work;
        size_t existing = 0;
        for (const auto& path : a->paths) {
            std::wstring target = compile ? CompiledScriptPath(path) : DecompiledScriptPath(path);
            if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) ++existing;
            work.emplace_back(path, std::move(target));
        }
        bool replace = true, go = true;
        if (existing) {
            std::wstring ask = existing == 1 && work.size() == 1 ? work[0].second + L" already exists. Replace it?"
                                                                  : std::to_wstring(existing) + L" of the output files already exist. Replace them?\r\n\r\nNo keeps them and does the rest.";
            const int choice = MessageBoxW(nullptr, ask.c_str(), title, (work.size() == 1 ? MB_YESNO : MB_YESNOCANCEL) | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (choice == IDCANCEL || (work.size() == 1 && choice == IDNO)) go = false;
            if (choice == IDNO) replace = false;
        }
        if (go) {
            int worst = 0;
            bool show = compile;
            std::wstring report;
            {
                PapyrusWorkspace workspace;
                ProgressBox box(compile ? L"Compiling" : L"Decompiling");
                for (size_t i = 0; i < work.size(); ++i) {
                    const auto& [source, target] = work[i];
                    if (!replace && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
                    box.Line(FileNameOf(source));
                    if (!box.Update(i, work.size())) break;
                    WorkSlot slot(WorkKind::Interactive);
                    const PapyrusResult r = compile ? workspace.Compile(source, target) : workspace.Decompile(source, target);
                    slot.Pace();
                    worst = (std::max)(worst, r.status);
                    report += r.text;
                    if (r.status) show = true;
                    const std::wstring folder = target.substr(0, target.find_last_of(L'\\'));
                    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
                }
            }
            if (show && !report.empty()) {
                const std::wstring name = work.size() == 1 ? FileNameOf(work[0].first) + L" " : std::to_wstring(work.size()) + L" scripts ";
                RunReportWindow(name + (compile ? L"compile" : L"decompile"), worst, report, L"");
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    HMODULE module = a->module;
    a.reset();
    FreeLibraryAndExitThread(module, 0);
}

void Start(LPTHREAD_START_ROUTINE routine, ActionArgs* args, SIZE_T stack = 0) {
    args->module = AddModuleRef();
    if (HANDLE t = CreateThread(nullptr, stack, routine, args, stack ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, nullptr)) {
        CloseHandle(t);
        return;
    }
    if (args->module) FreeLibrary(args->module);
    delete args;
}

}  // namespace

int RunHashWindow(const std::wstring& path, const std::wstring& png, const std::wstring& script) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = 1;
    {
        ActivationContext act;
        DpiScope dpi;
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        {
            std::lock_guard<std::mutex> g(g_classLock);
            if (g_hashUsers++ == 0) HashWindow::Register();
        }
        {
            HashWindow window(path, png, script);
            if (HWND hwnd = window.Create()) {
                ShowWindow(hwnd, SW_SHOWNORMAL);
                SetForegroundWindow(hwnd);
                result = RunLoop(hwnd);
            }
        }
        std::lock_guard<std::mutex> g(g_classLock);
        if (--g_hashUsers == 0) UnregisterClassW(kHashClass, g_hInst);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return result;
}

int RunReportWindow(const std::wstring& title, int status, const std::wstring& text, const std::wstring& png) {
    int result = 1;
    {
        std::lock_guard<std::mutex> g(g_classLock);
        if (g_reportUsers++ == 0) ReportWindow::Register();
    }
    {
        ReportWindow window(title, status, text, png);
        if (HWND hwnd = window.Create()) {
            ShowWindow(hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd);
            result = RunLoop(hwnd);
        }
    }
    std::lock_guard<std::mutex> g(g_classLock);
    if (--g_reportUsers == 0) UnregisterClassW(kReportClass, g_hInst);
    return result;
}

void ExtractBsasAsync(const std::vector<std::wstring>& paths, bool intoOwnFolder, const std::wstring& target) {
    auto* args = new ActionArgs{paths, intoOwnFolder};
    args->target = target;
    Start(&ExtractThread, args);
}

void TestBsasAsync(const std::vector<std::wstring>& paths) { Start(&TestThread, new ActionArgs{paths, false}); }

bool RunFolderViewNameWindow(std::wstring& name, const std::wstring& png) {
    {
        std::lock_guard<std::mutex> g(g_classLock);
        if (g_nameUsers++ == 0) NameWindow::Register();
    }
    {
        NameWindow window(png);
        if (HWND hwnd = window.Create()) {
            ShowWindow(hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd);
            RunLoop(hwnd);
            name = window.Name();
        }
    }
    std::lock_guard<std::mutex> g(g_classLock);
    if (--g_nameUsers == 0) UnregisterClassW(kNameClass, g_hInst);
    return !name.empty();
}

void SaveFolderViewAsync(FolderViewState state) {
    StartUserJob([state = std::move(state)]() mutable {
        for (;;) {
            std::wstring name;
            if (!RunFolderViewNameWindow(name, L"")) return;
            bool exists = false;
            for (const FolderViewState& known : LoadFolderViews()) exists = exists || _wcsicmp(known.name.c_str(), name.c_str()) == 0;
            if (exists) {
                const std::wstring ask = L"A view named \"" + name + L"\" already exists. Replace it?";
                if (MessageBoxW(nullptr, ask.c_str(), L"Folder views", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) continue;
            }
            state.name = name;
            if (!StoreFolderView(state)) MessageBoxW(nullptr, L"The view couldn't be saved.", L"Folder views", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            return;
        }
    });
}

void RemoveFolderViewAsync(const std::wstring& name) {
    StartUserJob([name] {
        const std::wstring ask = L"Remove the saved view \"" + name + L"\"?";
        if (MessageBoxW(nullptr, ask.c_str(), L"Folder views", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES) RemoveFolderView(name);
    });
}

void UpgradeHeadersAsync(const std::vector<std::wstring>& paths) {
    StartUserJob([paths] {
        std::wstring report;
        size_t upgraded = 0, unchanged = 0, failed = 0;
        bool formTooOld = false;
        for (const std::wstring& path : paths) {
            const HeaderUpgradeResult r = UpgradePluginHeader(path);
            report += r.text + L"\r\n";
            formTooOld = formTooOld || r.formTooOld;
            if (r.status == 0) {
                ++upgraded;
                SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
            } else if (r.status == 3) {
                ++failed;
            } else {
                ++unchanged;
            }
        }
        // A short list reads better in a message than in the scrolling report.
        if (paths.size() <= 8) {
            MessageBoxW(nullptr, report.c_str(), L"Upgrade header", MB_OK | MB_SETFOREGROUND | (failed || formTooOld ? MB_ICONWARNING : MB_ICONINFORMATION));
            return;
        }
        const std::wstring summary = std::to_wstring(upgraded) + L" upgraded, " + std::to_wstring(unchanged) + L" left unchanged, " + std::to_wstring(failed) + L" failed.\r\n\r\n";
        RunReportWindow(L"Upgrade header", failed ? 2 : unchanged ? 1 : 0, summary + report, L"");
    });
}

void ShowMessageAsync(const std::wstring& title, const std::wstring& text) {
    StartUserJob([title, text] { MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND); });
}

void PapyrusAsync(const std::vector<std::wstring>& paths, bool compile) {
    Start(&PapyrusThread, new ActionArgs{paths, compile}, 64u * 1024 * 1024);
}

int RunDependentsWindow(const std::wstring& path, const std::wstring& png) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = 1;
    {
        ActivationContext act;
        DpiScope dpi;
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&icc);
        {
            std::lock_guard<std::mutex> g(g_classLock);
            if (g_dependentsUsers++ == 0) DependentsWindow::Register();
        }
        {
            DependentsWindow window(path, png);
            if (HWND hwnd = window.Create()) {
                ShowWindow(hwnd, png.empty() ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
                if (png.empty()) SetForegroundWindow(hwnd);
                result = RunLoop(hwnd);
            }
            if (!png.empty()) {
                // Tests read the list as text too.
                wprintf(L"%s", window.ListText().c_str());
            }
        }
        std::lock_guard<std::mutex> g(g_classLock);
        if (--g_dependentsUsers == 0) UnregisterClassW(kDependentsClass, g_hInst);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return result;
}

void ShowDependentsWindowAsync(const std::wstring& path) {
    auto* args = new DependentsArgs{path, AddModuleRef()};
    if (HANDLE t = CreateThread(nullptr, 0, &DependentsThread, args, 0, nullptr)) {
        CloseHandle(t);
        return;
    }
    if (args->module) FreeLibrary(args->module);
    delete args;
}

void ShowHashWindowAsync(const std::wstring& path) {
    auto* args = new HashWindowArgs{path, AddModuleRef()};
    if (HANDLE t = CreateThread(nullptr, 0, &HashWindowThread, args, 0, nullptr)) {
        CloseHandle(t);
        return;
    }
    if (args->module) FreeLibrary(args->module);
    delete args;
}

}  // namespace pse
