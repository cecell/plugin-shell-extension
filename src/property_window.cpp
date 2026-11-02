#include "property_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <propsys.h>  // before propkey.h
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <mutex>

#include "debug_log.h"
#include "plugin_parser.h"

extern HINSTANCE g_hInst;

namespace pse {
namespace {

constexpr UINT WM_APP_SCANDONE = WM_APP + 1;
constexpr int IDC_TAB = 100;
constexpr int IDC_LIST = 101;
constexpr int IDM_COPY_VALUE = 1;
constexpr int IDM_COPY_ALL = 2;
constexpr wchar_t kDialogClass[] = L"PluginShellExtensionDialog";
constexpr wchar_t kListClass[] = L"PluginShellExtensionList";
constexpr wchar_t kNbsp = L' ';

// Layout of the Explorer Properties dialog's Details tab, in pixels at 96 DPI
// (measured from a 200% screenshot and halved).
constexpr int kClientW = 537, kClientH = 574;
constexpr int kTabX = 10, kTabY = 13, kTabRight = 9, kTabBottom = 44;
constexpr int kListX = 28, kListY = 52, kListRight = 85, kListBottom = 95;
constexpr int kButtonW = 99, kButtonH = 26, kButtonGap = 9, kButtonRight = 8, kButtonBottom = 10;
constexpr int kPropertyColumnW = 160;
constexpr int kHeaderMinH = 22;
constexpr int kTextPadX = 5, kTextPadY = 2;
constexpr int kGroupPadX = 7;

// DWMWA_USE_IMMERSIVE_DARK_MODE (dark title bar), Windows 10 20H1 and later.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;

// Colours for the parts this file paints itself. Common controls get the
// matching visual-style class instead (Explorer / DarkMode_Explorer, ...).
struct Palette {
    bool dark = false;
    COLORREF window;     // dialog background
    COLORREF page;       // tab page (painted by us only in dark mode)
    COLORREF tabBorder;  // tab page outline (dark mode)
    COLORREF listBack;
    COLORREF text;
    COLORREF groupText;
    COLORREF groupLine;
    COLORREF border;     // list outline
    // Row highlights, dark mode only (light mode uses the list visual style).
    COLORREF hot = 0, selected = 0, selectedHot = 0, selectedInactive = 0;
};

bool HighContrastOn() {
    HIGHCONTRASTW hc{sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

// Follows Settings > Personalization > Colors > "Choose your app mode".
// PSE_THEME=dark|light overrides it (for testing).
bool AppsUseDarkTheme() {
    wchar_t forced[16] = {};
    if (GetEnvironmentVariableW(L"PSE_THEME", forced, ARRAYSIZE(forced))) return _wcsicmp(forced, L"dark") == 0;
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// High contrast wins over dark mode and uses the user's system colours.
Palette CurrentPalette() {
    Palette p;
    if (HighContrastOn()) {
        p.window = p.page = GetSysColor(COLOR_3DFACE);
        p.tabBorder = p.border = GetSysColor(COLOR_WINDOWFRAME);
        p.listBack = GetSysColor(COLOR_WINDOW);
        p.text = GetSysColor(COLOR_WINDOWTEXT);
        p.groupText = GetSysColor(COLOR_HOTLIGHT);
        p.groupLine = GetSysColor(COLOR_GRAYTEXT);
    } else if (AppsUseDarkTheme()) {
        p.dark = true;
        p.window = RGB(32, 32, 32);
        p.page = RGB(43, 43, 43);
        p.tabBorder = RGB(64, 64, 64);
        p.listBack = RGB(25, 25, 25);
        p.text = RGB(255, 255, 255);
        p.groupText = RGB(153, 199, 255);
        p.groupLine = RGB(62, 72, 92);
        p.border = RGB(86, 86, 86);
        p.hot = RGB(45, 45, 45);
        p.selected = RGB(65, 65, 65);
        p.selectedHot = RGB(77, 77, 77);
        p.selectedInactive = RGB(55, 55, 55);
    } else {
        p.window = p.page = GetSysColor(COLOR_3DFACE);
        p.tabBorder = RGB(217, 217, 217);
        p.listBack = GetSysColor(COLOR_WINDOW);
        p.text = GetSysColor(COLOR_WINDOWTEXT);
        p.groupText = RGB(30, 57, 145);
        p.groupLine = RGB(198, 214, 238);
        p.border = RGB(130, 135, 144);
    }
    return p;
}

int Scale(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }
HMENU CtrlId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

struct Row {
    bool group = false;
    std::wstring name;
    std::wstring value;
    int top = 0;
    int height = 0;
    std::wstring tip;  // hover text for the row, if any
};

std::wstring StripBidiMarks(std::wstring s) {
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](wchar_t c) { return c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E); }),
            s.end());
    return s;
}

std::wstring Nbsp2Space(std::wstring s) {
    std::replace(s.begin(), s.end(), kNbsp, L' ');
    return s;
}

std::wstring GroupDigits(uint64_t n) {
    wchar_t sep[8] = L",";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, sep, ARRAYSIZE(sep));
    std::wstring digits = std::to_wstring(n);
    std::wstring out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) out.insert(0, sep);
        out.insert(out.begin(), *it);
        ++count;
    }
    return out;
}

// ---------------------------------------------------------------------------
// File properties, formatted exactly as the shell's Details tab does.

struct FileProps {
    std::wstring size, created, modified, attributes;
};

std::wstring FormatShellProperty(IPropertyStore* store, REFPROPERTYKEY key) {
    std::wstring out;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(store->GetValue(key, &pv)) && pv.vt != VT_EMPTY) {
        PWSTR text = nullptr;
        if (SUCCEEDED(PSFormatForDisplayAlloc(key, pv, PDFF_DEFAULT, &text)) && text) {
            out = StripBidiMarks(text);
            CoTaskMemFree(text);
        }
    }
    PropVariantClear(&pv);
    return out;
}

std::wstring FormatFileTime(const FILETIME& ft) {
    wchar_t buf[128] = {};
    DWORD flags = FDTF_SHORTDATE | FDTF_SHORTTIME | FDTF_NOAUTOREADINGORDER;
    SHFormatDateTimeW(&ft, &flags, buf, ARRAYSIZE(buf));
    return StripBidiMarks(buf);
}

std::wstring AttributeLetters(DWORD a) {
    std::wstring s;
    if (a & FILE_ATTRIBUTE_READONLY) s += L'R';
    if (a & FILE_ATTRIBUTE_HIDDEN) s += L'H';
    if (a & FILE_ATTRIBUTE_SYSTEM) s += L'S';
    if (a & FILE_ATTRIBUTE_DIRECTORY) s += L'D';
    if (a & FILE_ATTRIBUTE_ARCHIVE) s += L'A';
    if (a & FILE_ATTRIBUTE_TEMPORARY) s += L'T';
    if (a & FILE_ATTRIBUTE_SPARSE_FILE) s += L'P';
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) s += L'L';
    if (a & FILE_ATTRIBUTE_COMPRESSED) s += L'C';
    if (a & FILE_ATTRIBUTE_OFFLINE) s += L'O';
    if (a & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) s += L'I';
    if (a & FILE_ATTRIBUTE_ENCRYPTED) s += L'E';
    return s;
}

FileProps ReadFileProps(const std::wstring& path) {
    FileProps p;
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
        p.size = FormatShellProperty(store, PKEY_Size);
        p.created = FormatShellProperty(store, PKEY_DateCreated);
        p.modified = FormatShellProperty(store, PKEY_DateModified);
        p.attributes = FormatShellProperty(store, PKEY_FileAttributes);
        store->Release();
    }
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        if (p.size.empty()) {
            wchar_t buf[64] = {};
            ULONGLONG bytes = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            StrFormatByteSizeEx(bytes, SFBS_FLAGS_TRUNCATE_UNDISPLAYED_DECIMAL_DIGITS, buf, ARRAYSIZE(buf));
            p.size = buf;
        }
        if (p.created.empty()) p.created = FormatFileTime(fad.ftCreationTime);
        if (p.modified.empty()) p.modified = FormatFileTime(fad.ftLastWriteTime);
        if (p.attributes.empty()) p.attributes = AttributeLetters(fad.dwFileAttributes);
    }
    return p;
}

// ---------------------------------------------------------------------------
// PropertyList: the white Property/Value list. A ListView cannot give rows
// different heights, so this is a small custom control that draws the same
// way (Explorer list theme, group headings, header control) and wraps values.

class PropertyList {
public:
    static void Register() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &PropertyList::StaticProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kListClass;
        RegisterClassExW(&wc);
    }

    static PropertyList* From(HWND hwnd) {
        return reinterpret_cast<PropertyList*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    void SetRows(std::vector<Row> rows) {
        int oldSel = sel_;
        rows_ = std::move(rows);
        sel_ = (oldSel >= 0 && oldSel < static_cast<int>(rows_.size()) && !rows_[oldSel].group) ? oldSel : -1;
        hot_ = -1;
        MeasureRows();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SetDpi(UINT dpi, HFONT font, HFONT groupFont) {
        dpi_ = dpi;
        font_ = font;
        groupFont_ = groupFont;
        OpenTheme();
        SendMessageW(header_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        col0Pref_ = Scale(kPropertyColumnW, dpi_);
        Layout();
    }

    void SetPalette(const Palette& palette) {
        palette_ = palette;
        // Explorer's own dark list, scrollbar and header styles.
        SetWindowTheme(hwnd_, palette_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SetWindowTheme(header_, palette_.dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
        SetWindowTheme(tip_, palette_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        OpenTheme();
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    // Snapshot testing: show a selected and a hovered row.
    void TestHighlight(int selected, int hot) {
        if (selected >= 0 && selected < static_cast<int>(rows_.size())) sel_ = selected;
        if (hot >= 0 && hot < static_cast<int>(rows_.size())) hot_ = hot;
        focused_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }

private:
    void OpenTheme() {
        if (theme_) CloseThemeData(theme_);
        theme_ = OpenThemeDataForDpi(hwnd_, palette_.dark ? L"DarkMode_Explorer::ListView" : L"Explorer::ListView",
                                     dpi_);
    }

    Palette palette_ = CurrentPalette();
    HWND hwnd_ = nullptr;
    HWND header_ = nullptr;
    HWND tip_ = nullptr;
    std::wstring tipText_;
    HTHEME theme_ = nullptr;
    HFONT font_ = nullptr;
    HFONT groupFont_ = nullptr;
    UINT dpi_ = 96;
    std::vector<Row> rows_;
    int col0Pref_ = kPropertyColumnW;  // width the user asked for
    int col0_ = kPropertyColumnW;      // width that fits the current client area
    int headerH_ = kHeaderMinH;
    int contentH_ = 0;
    int scrollY_ = 0;
    int hot_ = -1;
    int sel_ = -1;
    bool focused_ = false;
    bool trackingLeave_ = false;
    bool settingColumns_ = false;

    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        PropertyList* self = From(hwnd);
        if (msg == WM_NCCREATE) {
            self = new PropertyList();
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        LRESULT r = self->Proc(msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete self;
        }
        return r;
    }

    int ClientWidth() const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        return rc.right;
    }
    int ClientHeight() const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        return rc.bottom;
    }
    int ValueColumnWidth() const { return std::max(ClientWidth() - col0_, Scale(40, dpi_)); }

    void Layout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        WINDOWPOS wp{};
        HDLAYOUT hdl{&rc, &wp};
        Header_Layout(header_, &hdl);
        headerH_ = std::max(wp.cy, Scale(kHeaderMinH, dpi_));
        SetWindowPos(header_, nullptr, 0, 0, rc.right, headerH_, SWP_NOZORDER | SWP_NOACTIVATE);
        col0_ = std::clamp(col0Pref_, Scale(40, dpi_), std::max(Scale(40, dpi_), ClientWidth() - Scale(40, dpi_)));
        SetColumnWidths();
        MeasureRows();
    }

    void SetColumnWidths() {
        settingColumns_ = true;
        HDITEMW item{};
        item.mask = HDI_WIDTH;
        item.cxy = col0_;
        Header_SetItem(header_, 0, &item);
        item.cxy = ValueColumnWidth();
        Header_SetItem(header_, 1, &item);
        settingColumns_ = false;
    }

    int TextHeight(HDC dc, const std::wstring& text, int width) const {
        RECT r{0, 0, std::max(width, 1), 0};
        DrawTextW(dc, text.empty() ? L" " : text.c_str(), -1, &r,
                  DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_EXPANDTABS);
        return r.bottom;
    }

    void MeasureRows() {
        HDC dc = GetDC(hwnd_);
        int padX = Scale(kTextPadX, dpi_);
        int padY = Scale(kTextPadY, dpi_);
        int y = 0;
        bool first = true;
        for (Row& row : rows_) {
            row.top = y;
            if (row.group) {
                HGDIOBJ old = SelectObject(dc, groupFont_);
                TEXTMETRICW tm;
                GetTextMetricsW(dc, &tm);
                SelectObject(dc, old);
                row.height = tm.tmHeight + Scale(first ? 6 : 12, dpi_);
            } else {
                HGDIOBJ old = SelectObject(dc, font_);
                int nameH = TextHeight(dc, row.name, col0_ - 2 * padX);
                int valueH = TextHeight(dc, row.value, ValueColumnWidth() - 2 * padX);
                SelectObject(dc, old);
                row.height = std::max(nameH, valueH) + 2 * padY;
            }
            first = false;
            y += row.height;
        }
        ReleaseDC(hwnd_, dc);
        contentH_ = y + Scale(4, dpi_);
        UpdateScrollBar();
    }

    int PageHeight() const { return std::max(ClientHeight() - headerH_, 1); }

    void UpdateScrollBar() {
        int maxScroll = std::max(contentH_ - PageHeight(), 0);
        scrollY_ = std::clamp(scrollY_, 0, maxScroll);
        SCROLLINFO si{sizeof(si)};
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
        si.nMin = 0;
        si.nMax = std::max(contentH_ - 1, 0);
        si.nPage = static_cast<UINT>(PageHeight());
        si.nPos = scrollY_;
        SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    }

    void ScrollTo(int y) {
        int maxScroll = std::max(contentH_ - PageHeight(), 0);
        y = std::clamp(y, 0, maxScroll);
        if (y == scrollY_) return;
        scrollY_ = y;
        SetScrollPos(hwnd_, SB_VERT, scrollY_, TRUE);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    int HitTest(int clientY) const {
        if (clientY < headerH_) return -1;
        int y = clientY - headerH_ + scrollY_;
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (y >= rows_[i].top && y < rows_[i].top + rows_[i].height) return static_cast<int>(i);
        }
        return -1;
    }

    void EnsureVisible(int i) {
        if (i < 0) return;
        const Row& r = rows_[i];
        if (r.top < scrollY_) ScrollTo(r.top);
        else if (r.top + r.height > scrollY_ + PageHeight()) ScrollTo(r.top + r.height - PageHeight());
    }

    void Select(int i) {
        if (i >= 0 && rows_[i].group) i = -1;
        if (i == sel_) return;
        sel_ = i;
        EnsureVisible(i);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void MoveSelection(int delta) {
        int n = static_cast<int>(rows_.size());
        if (!n) return;
        int i = sel_;
        if (i < 0) i = delta > 0 ? -1 : n;
        for (int step = 0; step < std::abs(delta); ++step) {
            int j = i;
            do {
                j += delta > 0 ? 1 : -1;
            } while (j >= 0 && j < n && rows_[j].group);
            if (j < 0 || j >= n) break;
            i = j;
        }
        if (i >= 0 && i < n) Select(i);
    }

    void CopyToClipboard(bool all) {
        std::wstring text;
        if (all) {
            for (const Row& r : rows_) {
                if (r.group) {
                    if (!text.empty()) text += L"\r\n";
                    text += L"[" + r.name + L"]\r\n";
                } else {
                    text += r.name + L": " + r.value + L"\r\n";
                }
            }
        } else if (sel_ >= 0) {
            text = rows_[sel_].value;
        }
        text = Nbsp2Space(text);
        if (text.empty() || !OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            memcpy(GlobalLock(mem), text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        }
        CloseClipboard();
    }

    void ShowContextMenu(POINT screen) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (sel_ >= 0 ? 0 : MF_GRAYED), IDM_COPY_VALUE, L"&Copy value\tCtrl+C");
        AppendMenuW(menu, MF_STRING, IDM_COPY_ALL, L"Copy &all");
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (cmd == IDM_COPY_VALUE) CopyToClipboard(false);
        if (cmd == IDM_COPY_ALL) CopyToClipboard(true);
    }

    void Paint(HDC target) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bmp = CreateCompatibleBitmap(target, std::max<LONG>(rc.right, 1), std::max<LONG>(rc.bottom, 1));
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        HBRUSH back = CreateSolidBrush(palette_.listBack);
        FillRect(dc, &rc, back);
        DeleteObject(back);
        SetBkMode(dc, TRANSPARENT);

        int padX = Scale(kTextPadX, dpi_);
        int padY = Scale(kTextPadY, dpi_);
        UINT textFlags = DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_EXPANDTABS;
        for (size_t i = 0; i < rows_.size(); ++i) {
            const Row& row = rows_[i];
            int top = headerH_ + row.top - scrollY_;
            if (top + row.height <= headerH_) continue;
            if (top >= rc.bottom) break;
            RECT rr{0, top, rc.right, top + row.height};
            if (row.group) {
                HGDIOBJ old = SelectObject(dc, groupFont_);
                TEXTMETRICW tm;
                GetTextMetricsW(dc, &tm);
                RECT tr{Scale(kGroupPadX, dpi_), rr.bottom - tm.tmHeight - Scale(3, dpi_), rc.right, rr.bottom};
                SetTextColor(dc, palette_.groupText);
                DrawTextW(dc, row.name.c_str(), -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_LEFT | DT_TOP);
                SIZE ext{};
                GetTextExtentPoint32W(dc, row.name.c_str(), static_cast<int>(row.name.size()), &ext);
                SelectObject(dc, old);
                int lineY = tr.top + tm.tmHeight / 2 + 1;
                RECT line{tr.left + ext.cx + Scale(4, dpi_), lineY, rc.right - Scale(kGroupPadX, dpi_), lineY + 1};
                HBRUSH lb = CreateSolidBrush(palette_.groupLine);
                FillRect(dc, &line, lb);
                DeleteObject(lb);
                continue;
            }
            bool selected = static_cast<int>(i) == sel_;
            bool hot = static_cast<int>(i) == hot_;
            if (selected || hot) {
                int state = selected ? (focused_ ? (hot ? LISS_HOTSELECTED : LISS_SELECTED) : LISS_SELECTEDNOTFOCUS)
                                     : LISS_HOT;
                RECT sr = rr;
                InflateRect(&sr, -Scale(1, dpi_), 0);
                if (palette_.dark) {
                    // The dark list style still draws light-blue highlights, so
                    // dark mode uses Explorer's grey hover/selection fills.
                    COLORREF fill = state == LISS_HOT              ? palette_.hot
                                    : state == LISS_SELECTEDNOTFOCUS ? palette_.selectedInactive
                                    : state == LISS_HOTSELECTED      ? palette_.selectedHot
                                                                     : palette_.selected;
                    HBRUSH b = CreateSolidBrush(fill);
                    FillRect(dc, &sr, b);
                    DeleteObject(b);
                } else if (theme_) {
                    DrawThemeBackground(theme_, dc, LVP_LISTITEM, state, &sr, nullptr);
                } else {
                    FillRect(dc, &sr, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
                }
            }
            SetTextColor(dc, (selected && !theme_) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : palette_.text);
            HGDIOBJ old = SelectObject(dc, font_);
            RECT nr{padX, top + padY, col0_ - padX, rr.bottom};
            DrawTextW(dc, row.name.c_str(), -1, &nr, textFlags);
            RECT vr{col0_ + padX, top + padY, col0_ + ValueColumnWidth() - padX, rr.bottom};
            DrawTextW(dc, row.value.c_str(), -1, &vr, textFlags);
            SelectObject(dc, old);
        }

        BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                // One tool over the whole list; the text comes from the hovered
                // row (TTN_GETDISPINFO), and rows without a tip show none.
                tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                       0, 0, 0, 0, hwnd_, nullptr, g_hInst, nullptr);
                TTTOOLINFOW ti{sizeof(ti)};
                ti.uFlags = TTF_SUBCLASS;
                ti.hwnd = hwnd_;
                ti.uId = 1;
                ti.lpszText = LPSTR_TEXTCALLBACKW;
                ti.rect = {0, 0, 32767, 32767};
                SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
                header_ = CreateWindowExW(0, WC_HEADERW, nullptr, WS_CHILD | WS_VISIBLE | HDS_HORZ | HDS_FULLDRAG,
                                          0, 0, 0, 0, hwnd_, nullptr, g_hInst, nullptr);
                HDITEMW item{};
                item.mask = HDI_TEXT | HDI_WIDTH | HDI_FORMAT;
                item.fmt = HDF_LEFT | HDF_STRING;
                item.cxy = col0_;
                item.pszText = const_cast<wchar_t*>(L"Property");
                Header_InsertItem(header_, 0, &item);
                item.pszText = const_cast<wchar_t*>(L"Value");
                Header_InsertItem(header_, 1, &item);
                SetPalette(palette_);
                return 0;
            }
            case WM_DESTROY:
                if (theme_) CloseThemeData(theme_);
                theme_ = nullptr;
                return 0;
            case WM_THEMECHANGED:
                if (theme_) CloseThemeData(theme_);
                theme_ = nullptr;
                OpenTheme();
                InvalidateRect(hwnd_, nullptr, FALSE);
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
                Paint(dc);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_PRINTCLIENT:
                Paint(reinterpret_cast<HDC>(wp));
                return 0;
            case WM_NCPAINT: {
                LRESULT r = DefWindowProcW(hwnd_, msg, wp, lp);
                HDC dc = GetWindowDC(hwnd_);
                RECT wr;
                GetWindowRect(hwnd_, &wr);
                OffsetRect(&wr, -wr.left, -wr.top);
                HBRUSH b = CreateSolidBrush(palette_.border);
                FrameRect(dc, &wr, b);
                DeleteObject(b);
                ReleaseDC(hwnd_, dc);
                return r;
            }
            case WM_VSCROLL: {
                SCROLLINFO si{sizeof(si)};
                si.fMask = SIF_ALL;
                GetScrollInfo(hwnd_, SB_VERT, &si);
                int line = Scale(20, dpi_);
                int y = scrollY_;
                switch (LOWORD(wp)) {
                    case SB_LINEUP: y -= line; break;
                    case SB_LINEDOWN: y += line; break;
                    case SB_PAGEUP: y -= PageHeight(); break;
                    case SB_PAGEDOWN: y += PageHeight(); break;
                    case SB_TOP: y = 0; break;
                    case SB_BOTTOM: y = contentH_; break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION: y = si.nTrackPos; break;
                }
                ScrollTo(y);
                return 0;
            }
            case WM_MOUSEWHEEL: {
                UINT lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                if (lines == WHEEL_PAGESCROLL) lines = static_cast<UINT>(PageHeight() / Scale(20, dpi_));
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                ScrollTo(scrollY_ - MulDiv(delta, static_cast<int>(lines) * Scale(20, dpi_), WHEEL_DELTA));
                return 0;
            }
            case WM_MOUSEMOVE: {
                if (!trackingLeave_) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                    trackingLeave_ = TrackMouseEvent(&tme) != FALSE;
                }
                int h = HitTest(GET_Y_LPARAM(lp));
                if (h >= 0 && rows_[h].group) h = -1;
                if (h != hot_) {
                    hot_ = h;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    SendMessageW(tip_, TTM_POP, 0, 0);  // re-query the text for the new row
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                trackingLeave_ = false;
                if (hot_ != -1) {
                    hot_ = -1;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
                SetFocus(hwnd_);
                Select(HitTest(GET_Y_LPARAM(lp)));
                return 0;
            case WM_CONTEXTMENU: {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc;
                    GetWindowRect(hwnd_, &rc);
                    pt = {rc.left + Scale(20, dpi_), rc.top + headerH_ + Scale(10, dpi_)};
                    if (sel_ >= 0) pt.y = rc.top + headerH_ + rows_[sel_].top - scrollY_ + Scale(10, dpi_);
                }
                ShowContextMenu(pt);
                return 0;
            }
            case WM_KEYDOWN: {
                bool ctrl = GetKeyState(VK_CONTROL) < 0;
                switch (wp) {
                    case VK_UP: MoveSelection(-1); return 0;
                    case VK_DOWN: MoveSelection(1); return 0;
                    case VK_HOME: sel_ = -1; MoveSelection(1); return 0;
                    case VK_END: sel_ = -1; MoveSelection(-1); return 0;
                    case VK_PRIOR: MoveSelection(-5); return 0;
                    case VK_NEXT: MoveSelection(5); return 0;
                    case 'C':
                    case VK_INSERT:
                        if (ctrl) CopyToClipboard(false);
                        return 0;
                }
                break;
            }
            case WM_GETDLGCODE:
                return DLGC_WANTARROWS | DLGC_WANTCHARS;
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                focused_ = msg == WM_SETFOCUS;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_NOTIFY: {
                if (reinterpret_cast<NMHDR*>(lp)->hwndFrom == tip_) {
                    if (reinterpret_cast<NMHDR*>(lp)->code == TTN_GETDISPINFOW) {
                        auto* di = reinterpret_cast<NMTTDISPINFOW*>(lp);
                        tipText_ = (hot_ >= 0 && hot_ < static_cast<int>(rows_.size())) ? rows_[hot_].tip : L"";
                        di->lpszText = const_cast<wchar_t*>(tipText_.c_str());
                    }
                    return 0;
                }
                auto* nh = reinterpret_cast<NMHEADERW*>(lp);
                if (nh->hdr.hwndFrom != header_) break;
                switch (nh->hdr.code) {
                    case NM_CUSTOMDRAW: {
                        // The dark header style keeps dark text; recolour it.
                        auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
                        if (!palette_.dark) return CDRF_DODEFAULT;
                        if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                        if (cd->dwDrawStage == CDDS_ITEMPREPAINT) SetTextColor(cd->hdc, palette_.text);
                        return CDRF_DODEFAULT;
                    }
                    case HDN_BEGINTRACKW:
                    case HDN_DIVIDERDBLCLICKW:
                        return nh->iItem != 0;  // only the Property column is sizable
                    case HDN_ITEMCHANGEDW:
                        if (!settingColumns_ && nh->iItem == 0 && nh->pitem && (nh->pitem->mask & HDI_WIDTH)) {
                            col0Pref_ = nh->pitem->cxy;
                            Layout();
                            InvalidateRect(hwnd_, nullptr, FALSE);
                        }
                        return 0;
                }
                break;
            }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
};

// ---------------------------------------------------------------------------
// Background GRUP scan. The result lands in a shared slot so nothing leaks if
// the window closes before the scan finishes.

struct ScanSlot {
    std::mutex lock;
    bool done = false;
    GroupScan result;
};

struct ScanJob {
    HWND hwnd;
    std::wstring path;
    PluginHeader header;
    std::shared_ptr<ScanSlot> slot;
    HMODULE module;
};

DWORD WINAPI ScanThread(void* param) {
    std::unique_ptr<ScanJob> job(static_cast<ScanJob*>(param));
    GroupScan scan = ScanGroups(job->path, job->header);
    {
        std::lock_guard<std::mutex> g(job->slot->lock);
        job->slot->result = std::move(scan);
        job->slot->done = true;
    }
    PostMessageW(job->hwnd, WM_APP_SCANDONE, 0, 0);
    HMODULE module = job->module;
    job.reset();
    FreeLibraryAndExitThread(module, 0);
}

HMODULE AddModuleRef() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&AddModuleRef), &module);
    return module;
}

// ---------------------------------------------------------------------------
// The dialog window.

class PropertyDialog {
public:
    static void Register() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &PropertyDialog::StaticProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // WM_ERASEBKGND paints the palette colour
        wc.lpszClassName = kDialogClass;
        RegisterClassExW(&wc);
    }

    PropertyDialog(std::wstring path, std::wstring snapshot) : path_(std::move(path)), snapshot_(std::move(snapshot)) {}

    HWND Create(int cascadeIndex) {
        cascade_ = cascadeIndex;
        std::wstring name = PathFindFileNameW(path_.c_str());
        std::wstring title = name + L" Properties";
        POINT cursor;
        GetCursorPos(&cursor);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
        // Created on the target monitor first so the DPI is known; sized in WM_CREATE.
        return CreateWindowExW(WS_EX_CONTROLPARENT, kDialogClass, title.c_str(),
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
                               mi.rcWork.left, mi.rcWork.top, 100, 100, nullptr, nullptr, g_hInst, this);
    }

private:
    std::wstring path_;
    std::wstring snapshot_;
    int cascade_ = 0;
    HWND hwnd_ = nullptr;
    HWND tab_ = nullptr, list_ = nullptr, ok_ = nullptr, cancel_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr, groupFont_ = nullptr;
    HICON iconSmall_ = nullptr, iconBig_ = nullptr;
    PluginHeader header_;
    FileProps fileProps_;
    std::shared_ptr<ScanSlot> slot_ = std::make_shared<ScanSlot>();
    bool scanDone_ = false;
    GroupScan scan_;
    Palette palette_ = CurrentPalette();
    HBRUSH windowBrush_ = nullptr;

    // Re-reads light/dark/high contrast and restyles everything; called at
    // creation and whenever Windows reports a colour or theme change.
    void ApplyPalette() {
        palette_ = CurrentPalette();
        if (windowBrush_) DeleteObject(windowBrush_);
        windowBrush_ = CreateSolidBrush(palette_.window);
        BOOL dark = palette_.dark;
        DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
        for (HWND b : {ok_, cancel_}) SetWindowTheme(b, palette_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        PropertyList::From(list_)->SetPalette(palette_);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    // The tab control has no dark visual style, so in dark mode it is painted
    // here: the "Details" tab and the page outline. Light mode and high
    // contrast keep the control's own drawing.
    static LRESULT CALLBACK TabProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<PropertyDialog*>(ref);
        if (self->palette_.dark && (msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_PRINTCLIENT)) {
            if (msg == WM_ERASEBKGND) return 1;
            PAINTSTRUCT ps{};
            HDC dc = msg == WM_PAINT ? BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wp);
            self->PaintDarkTab(hwnd, dc);
            if (msg == WM_PAINT) EndPaint(hwnd, &ps);
            return 0;
        }
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, &PropertyDialog::TabProc, 1);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    void PaintDarkTab(HWND tab, HDC dc) {
        RECT rc;
        GetClientRect(tab, &rc);
        auto fill = [dc](const RECT& r, COLORREF c) {
            HBRUSH b = CreateSolidBrush(c);
            FillRect(dc, &r, b);
            DeleteObject(b);
        };
        fill(rc, palette_.window);
        RECT item{};
        TabCtrl_GetItemRect(tab, 0, &item);
        RECT page = rc;
        page.top = item.bottom;
        fill(page, palette_.tabBorder);
        InflateRect(&page, -1, -1);
        fill(page, palette_.page);
        // The selected tab joins the page: outline on three sides only.
        RECT tabRect{item.left, item.top, item.right, item.bottom + 1};
        fill(tabRect, palette_.tabBorder);
        RECT inner{tabRect.left + 1, tabRect.top + 1, tabRect.right - 1, tabRect.bottom};
        fill(inner, palette_.page);
        wchar_t text[64] = {};
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = text;
        ti.cchTextMax = ARRAYSIZE(text);
        TabCtrl_GetItem(tab, 0, &ti);
        HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(SendMessageW(tab, WM_GETFONT, 0, 0));
        HGDIOBJ old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, palette_.text);
        DrawTextW(dc, text, -1, &inner, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<PropertyDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<PropertyDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->Proc(msg, wp, lp);
    }

    SIZE DefaultWindowSize(UINT dpi) const {
        RECT r{0, 0, Scale(kClientW, dpi), Scale(kClientH, dpi)};
        AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)), dpi);
        return {r.right - r.left, r.bottom - r.top};
    }

    void ApplyDpi(UINT dpi) {
        dpi_ = dpi;
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
        HFONT oldFont = font_, oldGroup = groupFont_;
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW glf = ncm.lfMessageFont;
        glf.lfHeight = MulDiv(glf.lfHeight, 7, 6);
        groupFont_ = CreateFontIndirectW(&glf);
        for (HWND h : {tab_, ok_, cancel_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        PropertyList::From(list_)->SetDpi(dpi_, font_, groupFont_);
        if (oldFont) DeleteObject(oldFont);
        if (oldGroup) DeleteObject(oldGroup);
    }

    void LayoutChildren() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        auto s = [this](int v) { return Scale(v, dpi_); };
        int cw = rc.right, ch = rc.bottom;
        HDWP dwp = BeginDeferWindowPos(5);
        dwp = DeferWindowPos(dwp, tab_, nullptr, s(kTabX), s(kTabY), cw - s(kTabX) - s(kTabRight),
                             ch - s(kTabY) - s(kTabBottom), SWP_NOZORDER | SWP_NOACTIVATE);
        dwp = DeferWindowPos(dwp, list_, nullptr, s(kListX), s(kListY), cw - s(kListX) - s(kListRight),
                             ch - s(kListY) - s(kListBottom), SWP_NOZORDER | SWP_NOACTIVATE);
        int by = ch - s(kButtonBottom) - s(kButtonH);
        int bx = cw - s(kButtonRight) - s(kButtonW);
        for (HWND b : {cancel_, ok_}) {
            dwp = DeferWindowPos(dwp, b, nullptr, bx, by, s(kButtonW), s(kButtonH), SWP_NOZORDER | SWP_NOACTIVATE);
            bx -= s(kButtonW) + s(kButtonGap);
        }
        EndDeferWindowPos(dwp);
    }

    std::vector<Row> BuildRows() const {
        std::vector<Row> rows;
        auto group = [&rows](const wchar_t* name) { rows.push_back({true, name, L""}); };
        auto row = [&rows](const wchar_t* name, std::wstring value) { rows.push_back({false, name, std::move(value)}); };

        group(L"Plugin");
        if (!header_.ok) {
            row(L"Error", header_.error);
        } else {
            row(L"Header version", header_.hasHedr ? VersionText(header_.version) : L"");
            row(L"Form version", std::to_wstring(header_.formVersion));
            row(L"Records", header_.hasHedr ? GroupDigits(static_cast<uint32_t>(header_.numRecords)) : L"");
            row(L"Flags", FlagsText(header_.flags));
            row(L"Light", header_.light ? L"True" : L"False");
            // A plugin that already loads as light needs no check; otherwise
            // it needs the full record walk, so it fills in with the GRUP scan.
            if (header_.light || scanDone_) {
                EslStatus esl = ClassifyEsl(header_, scan_);
                row(L"ESLable", EslStatusText(esl, false));
                if (EslNeedsNewerGame(header_, scan_, esl)) rows.back().tip = kNeedsNewerGameText;
            } else {
                row(L"ESLable", L"Checking…");
            }
            row(L"Author - CNAM", header_.author);
            row(L"Description - SNAM", header_.description);
            std::wstring masters;
            for (size_t i = 0; i < header_.masters.size(); ++i) {
                if (!masters.empty()) masters += L"\r\n";
                masters += L"[" + std::to_wstring(i) + L"] " + header_.masters[i];
            }
            row(L"Masters", masters);

            // "TYPE (n), TYPE (n), ..." with a no-break space so a type never
            // wraps away from its count.
            auto countList = [](const std::vector<GroupCount>& counts) {
                std::wstring s;
                for (const auto& c : counts) {
                    if (!s.empty()) s += L", ";
                    s += c.label + kNbsp + L"(" + std::to_wstring(c.records) + L")";
                }
                return s;
            };
            group(L"Contents");
            std::wstring types = L"Counting…";
            std::wstring deleted = L"Counting…";
            if (scanDone_) {
                types = countList(scan_.groups);
                if (!scan_.error.empty()) types += (types.empty() ? L"" : L"\r\n") + scan_.error;
                deleted = scan_.deleted.empty() ? (scan_.ok ? L"None" : L"") : countList(scan_.deleted);
            }
            row(L"Record types", types);
            row(L"Deleted forms", deleted);
            row(L"Deleted Navmeshes", scanDone_ ? (scan_.ok || scan_.deletedNavm ? std::to_wstring(scan_.deletedNavm) : L"")
                                       : L"Counting…");
            if (!header_.error.empty()) row(L"Warning", header_.error);
        }

        group(L"File");
        row(L"Size", fileProps_.size);
        row(L"Created", fileProps_.created);
        row(L"Modified", fileProps_.modified);
        row(L"Attributes", fileProps_.attributes);
        return rows;
    }

    // Completes through WM_APP_SCANDONE in every case, so snapshot mode always
    // finishes.
    void StartScan() {
        if (header_.ok) {
            auto* job = new ScanJob{hwnd_, path_, header_, slot_, AddModuleRef()};
            if (HANDLE t = CreateThread(nullptr, 0, &ScanThread, job, 0, nullptr)) {
                CloseHandle(t);
                return;
            }
            if (job->module) FreeLibrary(job->module);
            delete job;
        }
        {
            std::lock_guard<std::mutex> g(slot_->lock);
            if (header_.ok) slot_->result.error = L"Could not start the GRUP scan.";
            slot_->done = true;
        }
        PostMessageW(hwnd_, WM_APP_SCANDONE, 0, 0);
    }

    void Snapshot() {
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
        RECT wr, visible;
        GetWindowRect(hwnd_, &wr);
        // Crop the invisible resize borders, as a screenshot of the window would.
        if (FAILED(DwmGetWindowAttribute(hwnd_, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))))
            visible = wr;
        int fullW = wr.right - wr.left, fullH = wr.bottom - wr.top;
        int w = visible.right - visible.left, h = visible.bottom - visible.top;
        int cropX = visible.left - wr.left, cropY = visible.top - wr.top;
        // The frame bounds are always physical pixels; a DPI-unaware window's
        // rect is not, so fall back to the whole window when they disagree.
        if (cropX < 0 || cropY < 0 || cropX + w > fullW || cropY + h > fullH) {
            cropX = cropY = 0;
            w = fullW;
            h = fullH;
        }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), fullW, -fullH, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        HGDIOBJ old = SelectObject(dc, dib);
        PrintWindow(hwnd_, dc, PW_RENDERFULLCONTENT);
        SelectObject(dc, old);
        DeleteDC(dc);

        IWICImagingFactory* factory = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(snapshot_.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) {
            // WriteSource converts to whatever pixel format the PNG encoder picked.
            IWICBitmap* bitmap = nullptr;
            if (SUCCEEDED(factory->CreateBitmapFromMemory(static_cast<UINT>(fullW), static_cast<UINT>(fullH),
                                                          GUID_WICPixelFormat32bppBGR, static_cast<UINT>(fullW) * 4,
                                                          static_cast<UINT>(fullW * fullH * 4),
                                                          static_cast<BYTE*>(bits), &bitmap))) {
                WICRect crop{cropX, cropY, w, h};
                if (SUCCEEDED(frame->WriteSource(bitmap, &crop)) && SUCCEEDED(frame->Commit())) encoder->Commit();
                bitmap->Release();
            }
        }
        // Metrics next to the PNG, for checking the layout.
        RECT cr;
        GetClientRect(hwnd_, &cr);
        wchar_t info[256];
        swprintf_s(info, L"dpi=%u window=%dx%d client=%dx%d", dpi_, w, h, static_cast<int>(cr.right),
                   static_cast<int>(cr.bottom));
        HANDLE f = CreateFileW((snapshot_ + L".txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, info, static_cast<DWORD>(wcslen(info) * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(f);
        }
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        DeleteObject(dib);
    }

    void OnScanDone() {
        {
            std::lock_guard<std::mutex> g(slot_->lock);
            if (!slot_->done) return;
            scan_ = std::move(slot_->result);
        }
        scanDone_ = true;
        PropertyList::From(list_)->SetRows(BuildRows());
        if (!snapshot_.empty()) {
            // PSE_TEST_HIGHLIGHT=1: capture with row 2 selected and
            // row 4 hovered, to check the highlight colours.
            if (GetEnvironmentVariableW(L"PSE_TEST_HIGHLIGHT", nullptr, 0) > 0)
                PropertyList::From(list_)->TestHighlight(2, 4);
            Snapshot();
            DestroyWindow(hwnd_);
        }
    }

    void PlaceWindow() {
        SIZE size = DefaultWindowSize(dpi_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        RECT& work = mi.rcWork;
        int offset = Scale(26, dpi_) * (cascade_ % 10);
        int x = work.left + (work.right - work.left - size.cx) / 2 + offset;
        int y = work.top + (work.bottom - work.top - size.cy) / 2 + offset;
        x = std::max<int>(work.left, std::min<int>(x, work.right - size.cx));
        y = std::max<int>(work.top, std::min<int>(y, work.bottom - size.cy));
        SetWindowPos(hwnd_, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                dpi_ = GetDpiForWindow(hwnd_);
                SHFILEINFOW sfi{};
                if (SHGetFileInfoW(path_.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON))
                    iconSmall_ = sfi.hIcon;
                if (SHGetFileInfoW(path_.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON))
                    iconBig_ = sfi.hIcon;
                if (iconSmall_) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall_));
                if (iconBig_) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig_));

                tab_ = CreateWindowExW(0, WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                                       0, 0, 0, 0, hwnd_, CtrlId(IDC_TAB), g_hInst, nullptr);
                TCITEMW ti{};
                ti.mask = TCIF_TEXT;
                ti.pszText = const_cast<wchar_t*>(L"Details");
                TabCtrl_InsertItem(tab_, 0, &ti);
                list_ = CreateWindowExW(0, kListClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                                                    WS_BORDER | WS_CLIPCHILDREN,
                                        0, 0, 0, 0, hwnd_, CtrlId(IDC_LIST), g_hInst, nullptr);
                ok_ = CreateWindowExW(0, WC_BUTTONW, L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0,
                                      0, 0, hwnd_, CtrlId(IDOK), g_hInst, nullptr);
                cancel_ = CreateWindowExW(0, WC_BUTTONW, L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                          0, 0, 0, 0, hwnd_, CtrlId(IDCANCEL), g_hInst, nullptr);
                // The tab control is drawn first, under the list.
                SetWindowPos(tab_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SetWindowSubclass(tab_, &PropertyDialog::TabProc, 1, reinterpret_cast<DWORD_PTR>(this));

                ApplyPalette();
                ApplyDpi(dpi_);
                PlaceWindow();
                LayoutChildren();

                header_ = ReadPluginHeader(path_);
                fileProps_ = ReadFileProps(path_);
                StartScan();
                PropertyList::From(list_)->SetRows(BuildRows());
                SetFocus(list_);
                return 0;
            }
            case WM_SIZE:
                LayoutChildren();
                return 0;
            case WM_GETMINMAXINFO: {
                if (!list_) break;
                SIZE size = DefaultWindowSize(dpi_);
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                mmi->ptMinTrackSize = {size.cx, size.cy};
                return 0;
            }
            case WM_DPICHANGED: {
                ApplyDpi(HIWORD(wp));
                auto* r = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                LayoutChildren();
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
                    DestroyWindow(hwnd_);
                    return 0;
                }
                break;
            case WM_APP_SCANDONE:
                OnScanDone();
                return 0;
            case WM_ERASEBKGND: {
                RECT rc;
                GetClientRect(hwnd_, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, windowBrush_);
                return 1;
            }
            case WM_CTLCOLORBTN:  // the corners around themed push buttons
                return reinterpret_cast<LRESULT>(windowBrush_);
            case WM_SETTINGCHANGE:
                // "ImmersiveColorSet" = app mode switched between light and dark.
                if (lp && (lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0)) ApplyPalette();
                if (wp == SPI_SETHIGHCONTRAST) ApplyPalette();
                if (wp == SPI_SETNONCLIENTMETRICS) ApplyDpi(dpi_);  // system font changed
                break;
            case WM_THEMECHANGED:
            case WM_SYSCOLORCHANGE:
                ApplyPalette();
                break;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY:
                if (windowBrush_) DeleteObject(windowBrush_);
                windowBrush_ = nullptr;
                if (font_) DeleteObject(font_);
                if (groupFont_) DeleteObject(groupFont_);
                if (iconSmall_) DestroyIcon(iconSmall_);
                if (iconBig_) DestroyIcon(iconBig_);
                font_ = groupFont_ = nullptr;
                iconSmall_ = iconBig_ = nullptr;
                SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
};

std::mutex g_classLock;
int g_classUsers = 0;

struct WindowThreadArgs {
    std::wstring path;
    int index;
    HMODULE module;
};

DWORD WINAPI WindowThread(void* param) {
    std::unique_ptr<WindowThreadArgs> args(static_cast<WindowThreadArgs*>(param));
    RunPropertyWindow(args->path, std::wstring(), args->index);
    HMODULE module = args->module;
    args.reset();
    FreeLibraryAndExitThread(module, 0);
}

// Activates the DLL's own manifest (comctl32 v6) for the current thread.
class ActivationContext {
public:
    ActivationContext() {
        ACTCTXW act{sizeof(act)};
        act.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        act.hModule = g_hInst;
        act.lpResourceName = MAKEINTRESOURCEW(2);  // ISOLATIONAWARE_MANIFEST_RESOURCE_ID
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

}  // namespace

int RunPropertyWindow(const std::wstring& path, const std::wstring& snapshotPng, int cascadeIndex) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = 1;
    {
        ActivationContext actctx;
        // Per-monitor DPI aware: laid out for each monitor's scale and re-laid
        // out when moved between monitors. PSE_DPI_UNAWARE renders the
        // 100% layout instead (for testing).
        bool unaware = GetEnvironmentVariableW(L"PSE_DPI_UNAWARE", nullptr, 0) > 0;
        DPI_AWARENESS_CONTEXT oldDpi = SetThreadDpiAwarenessContext(
            unaware ? DPI_AWARENESS_CONTEXT_UNAWARE : DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        {
            std::lock_guard<std::mutex> g(g_classLock);
            if (g_classUsers++ == 0) {
                PropertyList::Register();
                PropertyDialog::Register();
            }
        }

        auto dialog = std::make_unique<PropertyDialog>(path, snapshotPng);
        HWND hwnd = dialog->Create(cascadeIndex);
        DebugLog(L"RunPropertyWindow %s hwnd=%p err=%lu", path.c_str(), hwnd, hwnd ? 0ul : GetLastError());
        if (hwnd) {
            ShowWindow(hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd);
            ULONGLONG start = GetTickCount64();
            MSG msg;
            BOOL got;
            while ((got = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
                if (!IsDialogMessageW(hwnd, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            DebugLog(L"RunPropertyWindow loop ended got=%d after %llu ms", got, GetTickCount64() - start);
            result = 0;
        }
        dialog.reset();
        {
            // The last window out unregisters, so a reloaded DLL never meets a
            // class whose window procedure points into the old image.
            std::lock_guard<std::mutex> g(g_classLock);
            if (--g_classUsers == 0) {
                UnregisterClassW(kDialogClass, g_hInst);
                UnregisterClassW(kListClass, g_hInst);
            }
        }
        if (oldDpi) SetThreadDpiAwarenessContext(oldDpi);
    }
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return result;
}

void ShowPropertyWindowsAsync(const std::vector<std::wstring>& paths) {
    int index = 0;
    for (const auto& path : paths) {
        auto* args = new WindowThreadArgs{path, index++, AddModuleRef()};
        HANDLE t = CreateThread(nullptr, 0, &WindowThread, args, 0, nullptr);
        DebugLog(L"ShowPropertyWindowsAsync %s module=%p thread=%p", path.c_str(), args->module, t);
        if (t) {
            CloseHandle(t);
        } else {
            if (args->module) FreeLibrary(args->module);
            delete args;
        }
    }
}

}  // namespace pse
