#include "folder_views.h"

#include <knownfolders.h>
#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "work_budget.h"

namespace pse {
namespace {

using Microsoft::WRL::ComPtr;
using nlohmann::json;

// Windows keeps the views of about 5,000 folders and forgets the oldest.
constexpr size_t kMaxWalkFolders = 4000;
constexpr DWORD kStepTimeoutMs = 8000;
constexpr UINT kScanFinished = WM_APP + 41;
constexpr UINT_PTR kWalkTimer = 1;
constexpr UINT kWalkTickMs = 15;
constexpr wchar_t kWalkClass[] = L"PluginShellExtensionFolderViewWalk";

std::mutex g_fileLock;

#define PSE_COLUMN L"Cecell.PluginShellExtension."

// Name, type, size and date come first in every preset, then every column
// that this format has. A column that isn't installed is left out when applied.
struct PresetDefinition {
    const wchar_t* name;
    std::vector<const wchar_t*> columns;
};

const std::vector<PresetDefinition>& PresetDefinitions() {
    static const std::vector<PresetDefinition> presets = {
        {L"Plugins",
         {PSE_COLUMN L"Flags", PSE_COLUMN L"Light", PSE_COLUMN L"ESLable", PSE_COLUMN L"Dirty", PSE_COLUMN L"ItmCount", PSE_COLUMN L"FormCount",
          PSE_COLUMN L"RecordCount", PSE_COLUMN L"MasterCount", PSE_COLUMN L"Masters", PSE_COLUMN L"DeletedCount", PSE_COLUMN L"DeletedNavmeshes", PSE_COLUMN L"HeaderVersion", PSE_COLUMN L"FormVersion"}},
        {L"Textures",
         {L"System.Image.Dimensions", L"System.Image.HorizontalSize", L"System.Image.VerticalSize", L"System.Image.BitDepth", PSE_COLUMN L"Mipmaps", L"System.Image.HorizontalResolution",
          L"System.Image.VerticalResolution", PSE_COLUMN L"TexFormat", PSE_COLUMN L"TexSrgb", PSE_COLUMN L"TexAlpha", PSE_COLUMN L"TexCubemap"}},
        {L"Meshes", {PSE_COLUMN L"NifGame", PSE_COLUMN L"NifVersion", PSE_COLUMN L"NifBlocks", PSE_COLUMN L"NifStrips", PSE_COLUMN L"NifShapes", PSE_COLUMN L"NifVertices", PSE_COLUMN L"NifTriangles", PSE_COLUMN L"NifTextures"}},
        {L"Audio",
         {L"System.Media.Duration", L"System.Audio.EncodingBitrate", PSE_COLUMN L"SampleRate", PSE_COLUMN L"Channels", L"System.Audio.SampleRate", L"System.Audio.ChannelCount", PSE_COLUMN L"AudioFormat",
          PSE_COLUMN L"FuzVersion", PSE_COLUMN L"HasLip", PSE_COLUMN L"LipVersion", PSE_COLUMN L"LipUnits"}},
        {L"Scripts", {PSE_COLUMN L"ScriptName", PSE_COLUMN L"ScriptCompiled", PSE_COLUMN L"ScriptExtends", PSE_COLUMN L"ScriptFunctions", PSE_COLUMN L"ScriptProperties", PSE_COLUMN L"ScriptStates", PSE_COLUMN L"ScriptFlags", PSE_COLUMN L"ScriptCompiledState"}},
        {L"Archives", {PSE_COLUMN L"BsaGame", PSE_COLUMN L"BsaVersion", PSE_COLUMN L"BsaFiles", PSE_COLUMN L"BsaFolders", PSE_COLUMN L"BsaContains", PSE_COLUMN L"BsaCompression", PSE_COLUMN L"BsaUnpacked", PSE_COLUMN L"BsaHasPlugin"}},
        {L"Animations", {PSE_COLUMN L"HkxContent", PSE_COLUMN L"HkxGame", L"System.Media.Duration", PSE_COLUMN L"HkxFrames", PSE_COLUMN L"HkxTracks",
          PSE_COLUMN L"HkxAnnotations", PSE_COLUMN L"HkxEvents", PSE_COLUMN L"HkxVariables", PSE_COLUMN L"HkxClips", PSE_COLUMN L"HkxAnimations"}},
    };
    return presets;
}

std::vector<FolderViewState> ShippedViews() {
    std::vector<FolderViewState> views;
    for (const PresetDefinition& preset : PresetDefinitions()) {
        FolderViewState state;
        state.name = preset.name;
        state.mode = FVM_DETAILS;
        state.iconSize = 16;
        state.shipped = true;
        for (const wchar_t* key : {L"System.ItemNameDisplay", L"System.ItemTypeText", L"System.Size", L"System.DateModified"}) state.columns.push_back({key, 0});
        for (const wchar_t* key : preset.columns) state.columns.push_back({key, 0});
        state.sort.push_back({L"System.ItemNameDisplay", 1});
        views.push_back(std::move(state));
    }
    return views;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring KeyText(const PROPERTYKEY& key) {
    wchar_t text[PKEYSTR_MAX] = {};
    return SUCCEEDED(PSStringFromPropertyKey(key, text, ARRAYSIZE(text))) ? text : L"";
}

// A saved layout holds "{fmtid} pid"; a built-in preset holds the property's name.
bool KeyFrom(const std::wstring& text, PROPERTYKEY& key) {
    if (!text.empty() && text[0] == L'{') return SUCCEEDED(PSPropertyKeyFromString(text.c_str(), &key));
    return SUCCEEDED(PSGetPropertyKeyFromName(text.c_str(), &key));
}

bool IsNullKey(const PROPERTYKEY& key) { return key.fmtid == GUID_NULL && key.pid == 0; }

json ToJson(const FolderViewState& s) {
    json columns = json::array(), sort = json::array();
    for (const auto& c : s.columns) columns.push_back({{"key", Utf8(c.key)}, {"width", c.width}});
    for (const auto& c : s.sort) sort.push_back({{"key", Utf8(c.key)}, {"direction", c.direction}});
    return {{"name", Utf8(s.name)}, {"mode", s.mode}, {"iconSize", s.iconSize}, {"columns", columns}, {"sort", sort}, {"group", Utf8(s.groupKey)}, {"groupAscending", s.groupAscending}};
}

bool FromJson(const json& j, FolderViewState& s) {
    try {
        s.name = Wide(j.at("name").get<std::string>());
        if (s.name.empty()) return false;
        s.mode = j.value("mode", 0);
        s.iconSize = j.value("iconSize", 0);
        s.groupKey = Wide(j.value("group", std::string()));
        s.groupAscending = j.value("groupAscending", true);
        for (const json& c : j.at("columns")) s.columns.push_back({Wide(c.at("key").get<std::string>()), c.value("width", 0u)});
        for (const json& c : j.at("sort")) s.sort.push_back({Wide(c.at("key").get<std::string>()), c.value("direction", 1)});
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

std::vector<FolderViewState> ReadViews(bool* damaged) {
    std::vector<FolderViewState> views;
    std::ifstream file(std::filesystem::path(FolderViewsFilePath()), std::ios::binary);
    if (!file) return views;
    const json root = json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("views") || !root["views"].is_array()) {
        if (damaged) *damaged = true;
        return views;
    }
    for (const json& item : root["views"]) {
        FolderViewState state;
        if (FromJson(item, state)) views.push_back(std::move(state));
    }
    std::sort(views.begin(), views.end(), [](const FolderViewState& a, const FolderViewState& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return views;
}

bool WriteViews(const std::vector<FolderViewState>& views) {
    json list = json::array();
    for (const FolderViewState& v : views) list.push_back(ToJson(v));
    const std::string text = json{{"views", list}}.dump(2);
    const std::wstring path = FolderViewsFilePath();
    const std::wstring temporary = path + L".tmp";
    {
        std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::wstring FolderOfView(IShellBrowser* browser, ComPtr<IShellView>& view) {
    view.Reset();
    if (FAILED(browser->QueryActiveShellView(&view)) || !view) return {};
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> folder;
    if (FAILED(view.As(&folderView)) || FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder)))) return {};
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(folder->GetCurFolder(&pidl))) return {};
    wchar_t path[MAX_PATH * 2] = {};
    const BOOL ok = SHGetPathFromIDListEx(pidl, path, ARRAYSIZE(path), GPFIDL_DEFAULT);
    CoTaskMemFree(pidl);
    return ok ? path : L"";
}

bool SameFolder(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

HMODULE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ThisModule), &module);
    return module;
}

// Stops at the first folder past the limit: that many are refused anyway.
std::vector<std::wstring> ListSubfolders(const std::wstring& root) {
    WorkSlot slot(WorkKind::Interactive);
    std::vector<std::wstring> found;
    std::deque<std::wstring> pending{root};
    while (!pending.empty() && found.size() <= kMaxWalkFolders) {
        const std::wstring folder = std::move(pending.front());
        pending.pop_front();
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            // Junctions can loop, and hidden folders aren't browsed.
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            std::wstring child = folder + L"\\" + data.cFileName;
            found.push_back(child);
            pending.push_back(std::move(child));
        } while (found.size() <= kMaxWalkFolders && FindNextFileW(find, &data));
        FindClose(find);
        slot.Pace();
    }
    return found;
}

class Walker {
public:
    Walker(IShellBrowser* browser, std::wstring root, FolderViewState state, FolderViewWalkCallbacks callbacks)
        : browser_(browser), root_(std::move(root)), from_(root_), state_(std::move(state)), callbacks_(std::move(callbacks)) {}

    bool Start() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &Walker::StaticProc;
        wc.hInstance = ThisModule();
        wc.lpszClassName = kWalkClass;
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, kWalkClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        if (!hwnd_) return false;
        scan_ = std::thread([this] { Scan(); });
        return true;
    }

private:
    enum ScanResult : WPARAM { kCancelled, kWalk, kNothing, kTooMany };

public:
    ~Walker() {
        if (scan_.joinable()) scan_.join();
        UnregisterClassW(kWalkClass, ThisModule());
        if (callbacks_.done) callbacks_.done();
    }

private:
    void Scan() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto list = std::make_unique<std::vector<std::wstring>>(ListSubfolders(root_));
        ScanResult result = kWalk;
        if (list->empty()) result = kNothing;
        else if (list->size() > kMaxWalkFolders) result = kTooMany;
        else if (callbacks_.confirm && !callbacks_.confirm(list->size())) result = kCancelled;
        if (PostMessageW(hwnd_, kScanFinished, result, reinterpret_cast<LPARAM>(list.get()))) list.release();
        if (SUCCEEDED(co)) CoUninitialize();
    }

    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Walker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Walker*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && msg == kScanFinished) {
            self->ScanFinished(static_cast<ScanResult>(wp), reinterpret_cast<std::vector<std::wstring>*>(lp));
            return 0;
        }
        if (self && msg == WM_TIMER && wp == kWalkTimer) {
            self->Tick();
            return 0;
        }
        const LRESULT result = DefWindowProcW(hwnd, msg, wp, lp);
        if (self && msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete self;
        }
        return result;
    }

    void ScanFinished(ScanResult result, std::vector<std::wstring>* list) {
        std::unique_ptr<std::vector<std::wstring>> owned(list);
        if (result == kWalk) {
            folders_ = std::move(*owned);
            SetTimer(hwnd_, kWalkTimer, kWalkTickMs, nullptr);
            return;
        }
        if (result == kNothing) End(L"This folder has no subfolders.");
        else if (result == kTooMany) End(L"This folder has more than 4,000 subfolders. Windows only remembers about 5,000 folder views, so the layout wasn't applied to them.");
        else End(L"");
    }

    bool Browse(const std::wstring& path) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) return false;
        const HRESULT hr = browser_->BrowseObject(pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER | SBSP_WRITENOHISTORY);
        CoTaskMemFree(pidl);
        return SUCCEEDED(hr);
    }

    void Tick() {
        KillTimer(hwnd_, kWalkTimer);
        if (!waiting_) {
            if (next_ >= folders_.size()) {
                Browse(root_);
                End(failed_ ? std::to_wstring(failed_) + L" of " + std::to_wstring(folders_.size()) + L" subfolders couldn't be changed." : L"");
                return;
            }
            if (!Browse(folders_[next_])) {
                End(L"Stopped after " + std::to_wstring(next_) + L" of " + std::to_wstring(folders_.size()) + L" subfolders: this window couldn't open the next one.");
                return;
            }
            waiting_ = true;
            started_ = GetTickCount();
        } else {
            ComPtr<IShellView> view;
            const std::wstring current = FolderOfView(browser_.Get(), view);
            if (SameFolder(current, folders_[next_])) {
                if (FAILED(ApplyFolderView(view.Get(), state_))) ++failed_;
                from_ = folders_[next_];
                ++next_;
                waiting_ = false;
            } else if (!current.empty() && !SameFolder(current, from_)) {
                End(L"Stopped after " + std::to_wstring(next_) + L" of " + std::to_wstring(folders_.size()) + L" subfolders because this window went to another folder.");
                return;
            } else if (GetTickCount() - started_ > kStepTimeoutMs) {
                End(L"Stopped after " + std::to_wstring(next_) + L" of " + std::to_wstring(folders_.size()) + L" subfolders because the next one didn't open in time.");
                return;
            }
        }
        SetTimer(hwnd_, kWalkTimer, kWalkTickMs, nullptr);
    }

    void End(const std::wstring& message) {
        if (!message.empty() && callbacks_.report) callbacks_.report(message);
        DestroyWindow(hwnd_);
    }

    ComPtr<IShellBrowser> browser_;
    std::wstring root_, from_;
    FolderViewState state_;
    FolderViewWalkCallbacks callbacks_;
    std::vector<std::wstring> folders_;
    std::thread scan_;
    HWND hwnd_ = nullptr;
    size_t next_ = 0, failed_ = 0;
    bool waiting_ = false;
    DWORD started_ = 0;
};

}  // namespace

std::wstring FolderViewsFilePath() {
    wchar_t forced[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"PSE_FOLDER_VIEWS_FILE", forced, ARRAYSIZE(forced))) return forced;
    PWSTR base = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) folder = std::wstring(base) + L"\\Plugin Shell Extension";
    CoTaskMemFree(base);
    if (!folder.empty()) CreateDirectoryW(folder.c_str(), nullptr);
    return folder + L"\\folder_views.json";
}

std::vector<FolderViewState> LoadFolderViews() {
    std::lock_guard<std::mutex> lock(g_fileLock);
    std::vector<FolderViewState> views = ReadViews(nullptr);
    for (FolderViewState& preset : ShippedViews()) {
        const bool replaced = std::any_of(views.begin(), views.end(), [&](const FolderViewState& v) { return _wcsicmp(v.name.c_str(), preset.name.c_str()) == 0; });
        if (!replaced) views.push_back(std::move(preset));
    }
    std::sort(views.begin(), views.end(), [](const FolderViewState& a, const FolderViewState& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return views;
}

bool StoreFolderView(const FolderViewState& state) {
    std::lock_guard<std::mutex> lock(g_fileLock);
    bool damaged = false;
    std::vector<FolderViewState> views = ReadViews(&damaged);
    if (damaged) {
        // Kept for the user rather than overwritten.
        const std::wstring path = FolderViewsFilePath();
        MoveFileExW(path.c_str(), (path + L".damaged").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    views.erase(std::remove_if(views.begin(), views.end(), [&](const FolderViewState& v) { return _wcsicmp(v.name.c_str(), state.name.c_str()) == 0; }), views.end());
    views.push_back(state);
    return WriteViews(views);
}

bool RemoveFolderView(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(g_fileLock);
    std::vector<FolderViewState> views = ReadViews(nullptr);
    const size_t before = views.size();
    views.erase(std::remove_if(views.begin(), views.end(), [&](const FolderViewState& v) { return _wcsicmp(v.name.c_str(), name.c_str()) == 0; }), views.end());
    return views.size() != before && WriteViews(views);
}

HRESULT CaptureFolderView(IShellView* view, FolderViewState& state) {
    if (!view) return E_POINTER;
    ComPtr<IFolderView2> folderView;
    ComPtr<IColumnManager> columns;
    HRESULT hr = view->QueryInterface(IID_PPV_ARGS(&folderView));
    if (SUCCEEDED(hr)) hr = view->QueryInterface(IID_PPV_ARGS(&columns));
    if (FAILED(hr)) return hr;

    FOLDERVIEWMODE mode = FVM_AUTO;
    int size = 0;
    hr = folderView->GetViewModeAndIconSize(&mode, &size);
    if (FAILED(hr)) return hr;
    state.mode = mode;
    state.iconSize = size;

    UINT count = 0;
    hr = columns->GetColumnCount(CM_ENUM_VISIBLE, &count);
    if (FAILED(hr)) return hr;
    std::vector<PROPERTYKEY> keys(count);
    if (count) hr = columns->GetColumns(CM_ENUM_VISIBLE, keys.data(), count);
    if (FAILED(hr)) return hr;
    state.columns.clear();
    for (const PROPERTYKEY& key : keys) {
        CM_COLUMNINFO info{};
        info.cbSize = sizeof(info);
        info.dwMask = CM_MASK_WIDTH;
        const UINT width = SUCCEEDED(columns->GetColumnInfo(key, &info)) ? info.uWidth : 0;
        state.columns.push_back({KeyText(key), width});
    }

    int sortCount = 0;
    state.sort.clear();
    if (SUCCEEDED(folderView->GetSortColumnCount(&sortCount)) && sortCount > 0) {
        std::vector<SORTCOLUMN> sort(sortCount);
        if (SUCCEEDED(folderView->GetSortColumns(sort.data(), sortCount))) {
            for (const SORTCOLUMN& c : sort) state.sort.push_back({KeyText(c.propkey), c.direction});
        }
    }

    PROPERTYKEY group{};
    BOOL ascending = TRUE;
    state.groupKey.clear();
    state.groupAscending = true;
    if (SUCCEEDED(folderView->GetGroupBy(&group, &ascending)) && !IsNullKey(group)) {
        state.groupKey = KeyText(group);
        state.groupAscending = ascending != FALSE;
    }
    return S_OK;
}

HRESULT ApplyFolderView(IShellView* view, const FolderViewState& state) {
    if (!view) return E_POINTER;
    ComPtr<IFolderView2> folderView;
    ComPtr<IColumnManager> columns;
    HRESULT hr = view->QueryInterface(IID_PPV_ARGS(&folderView));
    if (SUCCEEDED(hr)) hr = view->QueryInterface(IID_PPV_ARGS(&columns));
    if (FAILED(hr)) return hr;

    hr = folderView->SetViewModeAndIconSize(static_cast<FOLDERVIEWMODE>(state.mode), state.iconSize);
    if (FAILED(hr)) return hr;

    std::vector<PROPERTYKEY> keys;
    std::vector<UINT> widths;
    for (const FolderViewColumn& c : state.columns) {
        PROPERTYKEY key;
        if (KeyFrom(c.key, key)) {
            keys.push_back(key);
            widths.push_back(c.width);
        }
    }
    if (!keys.empty()) {
        hr = columns->SetColumns(keys.data(), static_cast<UINT>(keys.size()));
        if (FAILED(hr)) return hr;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (!widths[i]) continue;
            CM_COLUMNINFO info{};
            info.cbSize = sizeof(info);
            info.dwMask = CM_MASK_WIDTH;
            info.uWidth = widths[i];
            columns->SetColumnInfo(keys[i], &info);
        }
    }

    std::vector<SORTCOLUMN> sort;
    for (const FolderViewSort& c : state.sort) {
        SORTCOLUMN column{};
        if (KeyFrom(c.key, column.propkey)) {
            column.direction = static_cast<SORTDIRECTION>(c.direction);
            sort.push_back(column);
        }
    }
    if (!sort.empty()) folderView->SetSortColumns(sort.data(), static_cast<int>(sort.size()));

    PROPERTYKEY group{};
    if (state.groupKey.empty() || !KeyFrom(state.groupKey, group)) group = PROPERTYKEY{};
    folderView->SetGroupBy(group, state.groupAscending);

    // Saved now, so the layout survives even if the window never navigates away.
    view->SaveViewState();
    return S_OK;
}

bool StartFolderViewWalk(IShellBrowser* browser, const std::wstring& root, const FolderViewState& state, FolderViewWalkCallbacks callbacks) {
    if (!browser) return false;
    auto walker = std::make_unique<Walker>(browser, root, state, std::move(callbacks));
    if (!walker->Start()) return false;
    walker.release();  // deletes itself when its window closes
    return true;
}

}  // namespace pse
