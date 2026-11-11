// Tests the .bsa shell folder without touching .bsa's real file association.
//
//   bsabrowsetest --direct <dll> <archive> [...]   walks each archive and reads a sample
//   bsabrowsetest --registration                   installs and removes it in a throwaway key
//   bsabrowsetest --open <dll> <archive> <folder> <file>   opens one file, as a double-click does
//   bsabrowsetest --explorer <dll> <archive> <empty folder>
//       Registers a made-up extension for the current user, opens the archive in
//       Windows' real folder view (the same one Explorer uses), goes into a folder
//       the way a double-click does, and drops files into the given folder through
//       Windows' real drop target. Everything it registered is removed at the end.
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <atomic>
#include <thread>

#include "bsa_tools.h"
#include "papyrus_tools.h"
#include "asset_facts.h"
#include <propvarutil.h>
#include "plugin_tools.h"
#include "folder_views.h"
#include <exdisp.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <initguid.h>
#include <thumbcache.h>
#include <propkey.h>
#include <filesystem>
#include <fstream>
#include "registration.h"
#include "work_budget.h"

namespace {

int g_failures = 0;

void Check(bool ok, const wchar_t* what) {
    wprintf(L"%s %s\n", ok ? L"ok  " : L"FAIL", what);
    g_failures += !ok;
}

std::wstring Name(IShellFolder* folder, PCUITEMID_CHILD item, SHGDNF flags = SHGDN_INFOLDER) {
    STRRET s{};
    wchar_t buf[1024] = {};
    if (SUCCEEDED(folder->GetDisplayNameOf(item, flags, &s))) StrRetToBufW(&s, item, buf, ARRAYSIZE(buf));
    return buf;
}

struct Walk {
    size_t folders = 0, files = 0, read = 0, readFailed = 0;
    unsigned long long bytes = 0;
};

void WalkFolder(IShellFolder* folder, Walk* w, size_t readEvery) {
    IEnumIDList* e = nullptr;
    if (FAILED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &e)) || !e) return;
    LPITEMIDLIST item = nullptr;
    ULONG got = 0;
    while (e->Next(1, &item, &got) == S_OK && got) {
        SFGAOF attrs = SFGAO_FOLDER | SFGAO_STREAM | SFGAO_CANCOPY;
        PCUITEMID_CHILD one = item;
        folder->GetAttributesOf(1, &one, &attrs);
        if (attrs & SFGAO_FOLDER) {
            ++w->folders;
            IShellFolder* child = nullptr;
            if (SUCCEEDED(folder->BindToObject(item, nullptr, IID_PPV_ARGS(&child)))) {
                WalkFolder(child, w, readEvery);
                child->Release();
            }
        } else {
            ++w->files;
            if (readEvery && w->files % readEvery == 0) {
                IStream* s = nullptr;
                if (SUCCEEDED(folder->BindToObject(item, nullptr, IID_PPV_ARGS(&s))) && s) {
                    STATSTG st{};
                    s->Stat(&st, STATFLAG_NONAME);
                    w->bytes += st.cbSize.QuadPart;
                    ++w->read;
                    s->Release();
                } else {
                    ++w->readFailed;
                }
            }
        }
        CoTaskMemFree(item);
    }
    e->Release();
}

int Direct(int argc, wchar_t** argv) {
    HMODULE dll = LoadLibraryW(argv[2]);
    auto getClassObject = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    IClassFactory* factory = nullptr;
    if (!getClassObject || FAILED(getClassObject(pse::kClsidBsaFolder, IID_PPV_ARGS(&factory)))) {
        fwprintf(stderr, L"cannot load the folder from the DLL\n");
        return 2;
    }
    for (int i = 3; i < argc; ++i) {
        LPITEMIDLIST pidl = nullptr;
        IPersistFolder* persist = nullptr;
        IShellFolder* folder = nullptr;
        Walk w;
        HRESULT init = E_FAIL;
        if (SUCCEEDED(SHParseDisplayName(argv[i], nullptr, &pidl, 0, nullptr)) && SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&persist)))) {
            init = persist->Initialize(pidl);
            if (SUCCEEDED(init) && SUCCEEDED(persist->QueryInterface(IID_PPV_ARGS(&folder)))) WalkFolder(folder, &w, 25);
        }
        wprintf(L"%s|%x|%zu|%zu|%zu|%zu|%llu\n", argv[i], static_cast<unsigned>(init), w.folders, w.files, w.read, w.readFailed, w.bytes);
        if (folder) folder->Release();
        if (persist) persist->Release();
        if (pidl) CoTaskMemFree(pidl);
    }
    factory->Release();
    return 0;
}

// --winner <dll> <archive> <folder inside it>: the Winner column and menu, through the DLL.
int WinnerTest(const wchar_t* dllPath, const wchar_t* archive, const wchar_t* inside) {
    HMODULE dll = LoadLibraryW(dllPath);
    auto getClassObject = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    IClassFactory* factory = nullptr;
    if (!getClassObject || FAILED(getClassObject(pse::kClsidBsaFolder, IID_PPV_ARGS(&factory)))) return 2;
    LPITEMIDLIST pidl = nullptr;
    IPersistFolder* persist = nullptr;
    IShellFolder2* root = nullptr;
    if (FAILED(SHParseDisplayName(archive, nullptr, &pidl, 0, nullptr)) || FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&persist))) ||
        FAILED(persist->Initialize(pidl)) || FAILED(persist->QueryInterface(IID_PPV_ARGS(&root))))
        return 3;
    IShellFolder2* folder = root;
    PIDLIST_RELATIVE sub = nullptr;
    if (*inside) {
        if (FAILED(root->ParseDisplayName(nullptr, nullptr, const_cast<LPWSTR>(inside), nullptr, &sub, nullptr)) ||
            FAILED(root->BindToObject(sub, nullptr, IID_PPV_ARGS(&folder))))
            return 4;
    }
    UINT columns = 0;
    SHCOLSTATEF state = 0;
    while (SUCCEEDED(folder->GetDefaultColumnState(columns, &state))) ++columns;
    SHELLDETAILS head{};
    std::wstring title;
    if (columns > 3 && SUCCEEDED(folder->GetDetailsOf(nullptr, 3, &head))) {
        wchar_t b[64];
        StrRetToBufW(&head.str, nullptr, b, 64);
        title = b;
    }
    wprintf(L"columns %u%s%s\n", columns, title.empty() ? L"" : L", 4th: ", title.c_str());
    IEnumIDList* e = nullptr;
    folder->EnumObjects(nullptr, SHCONTF_NONFOLDERS, &e);
    PITEMID_CHILD item = nullptr;
    int yes = 0, no = 0, blank = 0;
    std::vector<PITEMID_CHILD> noItems;
    while (e && e->Next(1, &item, nullptr) == S_OK) {
        std::wstring text;
        SHELLDETAILS d{};
        if (columns > 3 && SUCCEEDED(folder->GetDetailsOf(item, 3, &d))) {
            wchar_t b[64];
            StrRetToBufW(&d.str, item, b, 64);
            text = b;
        }
        VARIANT v;
        VariantInit(&v);
        SHCOLUMNID id{};
        std::wstring ex;
        if (columns > 3 && SUCCEEDED(folder->MapColumnToSCID(3, &id)) && SUCCEEDED(folder->GetDetailsEx(item, &id, &v)) && v.vt == VT_BSTR) ex = v.bstrVal;
        VariantClear(&v);
        if (text != ex) wprintf(L"MISMATCH GetDetailsOf '%s' vs GetDetailsEx '%s'\n", text.c_str(), ex.c_str());
        if (text == L"Yes") ++yes;
        else if (text == L"No") {
            ++no;
            if (noItems.size() < 3) {
                noItems.push_back(item);
                continue;
            }
        } else ++blank;
        CoTaskMemFree(item);
    }
    if (e) e->Release();
    wprintf(L"Yes %d, No %d, blank %d\n", yes, no, blank);
    // The menu on the first "No" file (or on any file when none is No).
    if (noItems.empty()) {
        folder->EnumObjects(nullptr, SHCONTF_NONFOLDERS, &e);
        if (e && e->Next(1, &item, nullptr) == S_OK) noItems.push_back(item);
        if (e) e->Release();
    }
    for (PITEMID_CHILD it : noItems) {
        IContextMenu* menu = nullptr;
        PCUITEMID_CHILD arr[] = {it};
        if (FAILED(folder->GetUIObjectOf(nullptr, 1, arr, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&menu)))) continue;
        HMENU m = CreatePopupMenu();
        menu->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL);
        std::wstring items;
        for (int i = 0; i < GetMenuItemCount(m); ++i) {
            wchar_t t[256] = {};
            GetMenuStringW(m, i, t, 256, MF_BYPOSITION);
            items += std::wstring(i ? L" | " : L"") + t;
            if (HMENU s = GetSubMenu(m, i)) {
                items += L" > [";
                for (int j = 0; j < GetMenuItemCount(s); ++j) {
                    wchar_t u[256] = {};
                    GetMenuStringW(s, j, u, 256, MF_BYPOSITION);
                    items += std::wstring(j ? L", " : L"") + u + ((GetMenuState(s, j, MF_BYPOSITION) & MF_GRAYED) ? L" (greyed)" : L"");
                }
                items += L"]";
            }
        }
        wprintf(L"%s: %s\n", Name(folder, it).c_str(), items.c_str());
        CMINVOKECOMMANDINFO ici{sizeof(ici)};
        ici.lpVerb = MAKEINTRESOURCEA(2);  // Winner > Copy path
        if (SUCCEEDED(menu->InvokeCommand(&ici)) && OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                wprintf(L"  Copy path: %s\n", static_cast<const wchar_t*>(GlobalLock(h)));
                GlobalUnlock(h);
            }
            CloseClipboard();
        }
        DestroyMenu(m);
        menu->Release();
        CoTaskMemFree(it);
    }
    if (folder != root) folder->Release();
    if (sub) CoTaskMemFree(sub);
    root->Release();
    persist->Release();
    CoTaskMemFree(pidl);
    factory->Release();
    return 0;
}

int Budget() {
    using namespace pse;
    Check(PaceDelayMs(10, 1, 4) == 0, L"an idle machine isn't slowed");
    unsigned last = 0;
    bool rises = true;
    for (int cpu = 50; cpu <= 100; cpu += 5) {
        const unsigned d = PaceDelayMs(cpu, 1, 4);
        rises = rises && d >= last;
        last = d;
    }
    Check(rises && last >= 150, L"it slows more as the CPU gets busier");
    Check(PaceDelayMs(30, 4, 4) > PaceDelayMs(30, 1, 4), L"it slows more with more workers");
    Check(PaceDelayMs(100, 64, 1) <= 400, L"a rest is never longer than the cap");
    const int limit = WorkerLimit();
    wprintf(L"     limit %d, active now %d\n", limit, ActiveWorkers());
    Check(limit >= 1 && limit <= 64, L"the limit is sensible");
    const int before = ActiveWorkers();
    std::atomic<int> running{0}, peak{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < limit + 6; ++i)
        threads.emplace_back([&] {
            WorkSlot slot(WorkKind::Interactive);
            const int now = ++running;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {
            }
            Sleep(120);
            --running;
        });
    for (auto& t : threads) t.join();
    wprintf(L"     peak running %d of limit %d\n", peak.load(), limit);
    Check(peak.load() <= limit, L"never more workers than the limit");
    Check(peak.load() >= 1, L"work did run");
    Check(ActiveWorkers() == before, L"every slot was given back");
    {
        std::atomic<bool> cancel{true};
        std::vector<std::unique_ptr<WorkSlot>> full;
        while (static_cast<int>(full.size()) < limit - before) full.push_back(std::make_unique<WorkSlot>(WorkKind::Interactive));
        WorkSlot blocked(WorkKind::Interactive, &cancel);
        Check(!blocked.Held(), L"a full budget can be waited on and cancelled");
    }
    Check(ActiveWorkers() == before, L"cancelling leaves nothing held");
    return g_failures ? 1 : 0;
}

int Facts(const wchar_t* archive) {
    pse::BsaFacts f = pse::ReadBsaFacts(archive);
    wprintf(L"ok=%d version=%u files=%u folders=%u compressed=%llu unpacked=%llu archive=%llu skipped=%llu largest=%s game=%s type=%s\n", f.ok, f.version, f.files, f.folders, f.compressedFiles, f.unpackedBytes, f.archiveBytes,
            f.skippedNames, f.largestName.c_str(), pse::BsaGameName(f.version), pse::BsaCompressionName(f));
    for (const auto& k : f.kinds) wprintf(L"%s\t%llu\n", k.label.c_str(), k.count);
    return f.ok ? 0 : 1;
}

int Extract(const wchar_t* archive, const wchar_t* dest, const wchar_t* mode) {
    const uint64_t existing = pse::CountExisting(archive, dest);
    const ULONGLONG start = GetTickCount64();
    pse::ExtractResult r = pse::ExtractBsa(archive, dest, !wcscmp(mode, L"skip") ? pse::ExistingFiles::Skip : pse::ExistingFiles::Replace, {});
    wprintf(L"existing=%llu opened=%d written=%llu skippedExisting=%llu skippedNames=%llu failed=%llu ms=%llu error=%s\n", existing, r.opened, r.written, r.skippedExisting, r.skippedNames, r.failed, GetTickCount64() - start, r.firstError.c_str());
    return r.opened && !r.failed ? 0 : 1;
}

int TestArchive(const wchar_t* archive) {
    const ULONGLONG start = GetTickCount64();
    pse::BsaTestReport r = pse::TestBsa(archive, {});
    wprintf(L"status=%d ms=%llu\n%s\n", r.status, GetTickCount64() - start, r.text.c_str());
    return 0;
}

int Hash(const wchar_t* file) {
    pse::FileHashes h = pse::ComputeHashes(file, nullptr, {});
    wprintf(L"ok=%d sha256=%s crc32=%s md5=%s error=%s\n", h.ok, h.sha256.c_str(), h.crc32.c_str(), h.md5.c_str(), h.error.c_str());
    return h.ok ? 0 : 1;
}

int HashWindow(const wchar_t* dllPath, const wchar_t* file, const wchar_t* png, const wchar_t* script) {
    HMODULE dll = LoadLibraryW(dllPath);
    auto run = dll ? reinterpret_cast<int(__stdcall*)(const wchar_t*, const wchar_t*, const wchar_t*)>(GetProcAddress(dll, "PseHashSnapshot")) : nullptr;
    return run ? run(file, png, script) : 2;
}

int ReportWindow(const wchar_t* dllPath, const wchar_t* archive, const wchar_t* png) {
    HMODULE dll = LoadLibraryW(dllPath);
    auto run = dll ? reinterpret_cast<int(__stdcall*)(const wchar_t*, int, const wchar_t*, const wchar_t*)>(GetProcAddress(dll, "PseReportSnapshot")) : nullptr;
    if (!run) return 2;
    pse::BsaTestReport r = pse::TestBsa(archive, {});
    return run(L"Test result", r.status, r.text.c_str(), png);
}

// Runs each line of a list file: "d|c<TAB>input<TAB>output". Prints one status line per script.
int PapyrusListOnThisThread(const wchar_t* listPath, const wchar_t* data) {
    std::wifstream list{std::filesystem::path(listPath)};
    pse::PapyrusWorkspace workspace(data);
    std::wstring line;
    int problems = 0;
    while (std::getline(list, line)) {
        const size_t a = line.find(L'\t'), b = line.rfind(L'\t');
        if (a == std::wstring::npos || a == b) continue;
        const std::wstring in = line.substr(a + 1, b - a - 1), out = line.substr(b + 1);
        const pse::PapyrusResult r = line[0] == L'c' ? workspace.Compile(in, out) : workspace.Decompile(in, out);
        if (r.status) ++problems;
        std::wstring text = r.text;
        for (wchar_t& c : text) if (c == L'\r' || c == L'\n') c = L' ';
        wprintf(L"%d %ls\n", r.status, text.c_str());
    }
    return problems ? 1 : 0;
}

// Drives a real Explorer window from outside its process.
namespace views {
using Microsoft::WRL::ComPtr;
int g_fail = 0;
void Expect(bool ok, const wchar_t* what) {
    wprintf(L"%ls %ls\n", ok ? L"ok  " : L"FAIL", what);
    if (!ok) ++g_fail;
}
void Pump(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
}
ComPtr<IWebBrowser2> FindWindowFor(const std::wstring& folder) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return nullptr;
    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count; ++i) {
        VARIANT index{};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        if (windows->Item(index, &item) != S_OK || !item) continue;
        ComPtr<IServiceProvider> sp;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        ComPtr<IFolderView> fv;
        ComPtr<IPersistFolder2> pf;
        if (FAILED(item.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&fv)) || FAILED(fv->GetFolder(IID_PPV_ARGS(&pf)))) continue;
        PIDLIST_ABSOLUTE pidl = nullptr;
        wchar_t path[1024] = {};
        if (SUCCEEDED(pf->GetCurFolder(&pidl))) { SHGetPathFromIDListW(pidl, path); CoTaskMemFree(pidl); }
        if (!_wcsicmp(path, folder.c_str())) { ComPtr<IWebBrowser2> web; item.As(&web); return web; }
    }
    return nullptr;
}
ComPtr<IWebBrowser2> Open(const std::wstring& folder) {
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + folder + L"\"").c_str(), nullptr, SW_SHOWNOACTIVATE);
    for (int i = 0; i < 200; ++i) {
        Pump(50);
        if (auto w = FindWindowFor(folder)) return w;
    }
    return nullptr;
}
bool ViewOf(IWebBrowser2* web, ComPtr<IShellBrowser>& browser, ComPtr<IShellView>& view) {
    ComPtr<IServiceProvider> sp;
    return SUCCEEDED(web->QueryInterface(IID_PPV_ARGS(&sp))) && SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) && SUCCEEDED(browser->QueryActiveShellView(&view));
}
std::wstring Key(const PROPERTYKEY& k) { wchar_t t[PKEYSTR_MAX] = {}; PSStringFromPropertyKey(k, t, ARRAYSIZE(t)); return t; }
void Print(const pse::FolderViewState& s) {
    wprintf(L"  mode=%d icon=%d group=[%ls] columns=%zu:", s.mode, s.iconSize, s.groupKey.c_str(), s.columns.size());
    for (auto& c : s.columns) wprintf(L" %u", c.width);
    wprintf(L" sort=%zu", s.sort.size());
    for (auto& c : s.sort) wprintf(L" %d", c.direction);
    wprintf(L"\n");
}
bool Same(const pse::FolderViewState& a, const pse::FolderViewState& b) {
    if (a.mode != b.mode || a.groupKey != b.groupKey || a.columns.size() != b.columns.size() || a.sort.size() != b.sort.size()) return false;
    if (a.mode != FVM_DETAILS && a.iconSize != b.iconSize) return false;
    for (size_t i = 0; i < a.columns.size(); ++i) if (a.columns[i].key != b.columns[i].key || a.columns[i].width != b.columns[i].width) return false;
    for (size_t i = 0; i < a.sort.size(); ++i) if (a.sort[i].key != b.sort[i].key || a.sort[i].direction != b.sort[i].direction) return false;
    return true;
}
std::vector<pse::FolderViewState> Saved() {
    std::vector<pse::FolderViewState> out;
    for (auto& v : pse::LoadFolderViews()) if (!v.shipped) out.push_back(v);
    return out;
}
int Run(const std::wstring& root) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto web = Open(root);
    Expect(web != nullptr, L"a real Explorer window opened on the test folder");
    if (!web) return 1;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    Expect(ViewOf(web.Get(), browser, view), L"its view can be reached");
    pse::FolderViewState original;
    HRESULT hr = pse::CaptureFolderView(view.Get(), original);
    wprintf(L"capture hr=0x%08lx\n", static_cast<unsigned long>(hr));
    Expect(SUCCEEDED(hr), L"capture works");
    Print(original);

    pse::FolderViewState a;
    a.name = L"A";
    a.mode = FVM_DETAILS;
    a.iconSize = 16;
    a.columns = {{Key(PKEY_ItemNameDisplay), 300}, {Key(PKEY_Size), 420}, {Key(PKEY_ItemTypeText), 360}, {Key(PKEY_DateModified), 240}};
    a.sort = {{Key(PKEY_Size), -1}};
    a.groupKey = Key(PKEY_ItemTypeText);
    a.groupAscending = true;
    hr = pse::ApplyFolderView(view.Get(), a);
    wprintf(L"apply hr=0x%08lx\n", static_cast<unsigned long>(hr));
    Expect(SUCCEEDED(hr), L"apply A works");
    Pump(600);
    pse::FolderViewState got;
    ViewOf(web.Get(), browser, view);
    pse::CaptureFolderView(view.Get(), got);
    Print(got);
    Expect(Same(a, got), L"mode, columns, order, widths, sort and group all read back as applied");

    for (UINT w : {60u, 100u, 130u, 190u, 201u, 250u}) {
        pse::FolderViewState p = a;
        p.columns[1].width = w;
        pse::ApplyFolderView(view.Get(), p);
        Pump(250);
        pse::FolderViewState r;
        pse::CaptureFolderView(view.Get(), r);
        wprintf(L"probe size column %u -> %u\n", w, r.columns[1].width);
    }
    pse::ApplyFolderView(view.Get(), a);
    Pump(250);
    pse::FolderViewState b = a;
    b.name = L"B";
    b.mode = FVM_ICON;
    b.iconSize = 256;
    b.groupKey.clear();
    b.sort = {{Key(PKEY_ItemNameDisplay), 1}};
    pse::ApplyFolderView(view.Get(), b);
    Pump(600);
    pse::CaptureFolderView(view.Get(), got);
    Print(got);
    Expect(got.mode == FVM_ICON && got.iconSize == 256 && got.groupKey.empty(), L"extra large icons and no grouping read back");

    pse::ApplyFolderView(view.Get(), a);
    Pump(400);
    web->Quit();
    web.Reset(); browser.Reset(); view.Reset();
    Pump(1500);
    web = Open(root);
    Expect(web != nullptr, L"the folder reopened");
    if (web) {
        ViewOf(web.Get(), browser, view);
        pse::FolderViewState back;
        pse::CaptureFolderView(view.Get(), back);
        Print(back);
        Expect(Same(a, back), L"Windows remembered the layout after the window closed");

        // The subfolder walk, in the same window.
        bool finished = false;
        std::wstring message;
        pse::FolderViewWalkCallbacks cb;
        size_t asked = 0;
        cb.confirm = [&](size_t n) { asked = n; return true; };
        cb.report = [&](const std::wstring& m) { message = m; };
        cb.done = [&] { finished = true; };
        pse::FolderViewState c = a;
        c.columns[1].width = 410;
        const ULONGLONG t0 = GetTickCount64();
        Expect(pse::StartFolderViewWalk(browser.Get(), root, c, cb), L"the walk starts");
        while (!finished && GetTickCount64() - t0 < 120000) Pump(20);
        wprintf(L"walk: asked=%zu finished=%d ms=%llu message=[%ls]\n", asked, finished, GetTickCount64() - t0, message.c_str());
        Expect(finished && message.empty(), L"the walk finishes cleanly");
        Expect(asked == 4, L"it found all 4 subfolders and skipped the hidden one");
        Pump(800);
        Expect(FindWindowFor(root) != nullptr, L"the window came back to where it started");
        web->Quit();
        web.Reset(); browser.Reset(); view.Reset();
        Pump(1200);
        for (const wchar_t* sub : {L"\\one", L"\\two", L"\\two\\deep", L"\\three"}) {
            auto w = Open(root + sub);
            Expect(w != nullptr, sub);
            if (!w) continue;
            ComPtr<IShellBrowser> b2;
            ComPtr<IShellView> v2;
            ViewOf(w.Get(), b2, v2);
            pse::FolderViewState s;
            pse::CaptureFolderView(v2.Get(), s);
            Print(s);
            Expect(Same(c, s), L"the subfolder remembered the layout");
            w->Quit();
            Pump(700);
        }
    }

    // The built-in presets, on a real window.
    {
        auto presets = pse::LoadFolderViews();
        Expect(presets.size() == 7, L"seven presets ship");
        auto win = Open(root);
        Expect(win != nullptr, L"a window opened for the presets");
        if (win) {
            ComPtr<IShellBrowser> pb;
            ComPtr<IShellView> pv;
            ViewOf(win.Get(), pb, pv);
            for (const auto& p : presets) {
                HRESULT phr = pse::ApplyFolderView(pv.Get(), p);
                Pump(350);
                ViewOf(win.Get(), pb, pv);
                pse::FolderViewState r;
                pse::CaptureFolderView(pv.Get(), r);
                size_t installed = 0;  // a new column exists only after the schema is registered
                for (const auto& c : p.columns) { PROPERTYKEY k; if (SUCCEEDED(PSGetPropertyKeyFromName(c.key.c_str(), &k))) ++installed; }
                wprintf(L"preset %ls hr=0x%08lx listed %zu, installed %zu, got %zu\n", p.name.c_str(), static_cast<unsigned long>(phr), p.columns.size(), installed, r.columns.size());
                Expect(p.shipped && r.mode == FVM_DETAILS && r.groupKey.empty() && r.columns.size() == installed && r.sort.size() == 1 && r.sort[0].direction == 1, L"details, not grouped, every column, sorted by name");
            }
            pse::FolderViewState custom = presets[0];
            custom.name = L"plugins";
            custom.shipped = false;
            custom.columns.resize(2);
            Expect(pse::StoreFolderView(custom), L"saving under a preset's name works");
            auto after = pse::LoadFolderViews();
            size_t matches = 0, cols = 0;
            bool shippedStill = false;
            for (auto& v : after) if (!_wcsicmp(v.name.c_str(), L"plugins")) { ++matches; cols = v.columns.size(); shippedStill = v.shipped; }
            Expect(after.size() == 7 && matches == 1 && cols == 2 && !shippedStill, L"that replaces the preset, with no duplicate");
            Expect(pse::RemoveFolderView(L"plugins"), L"it can be removed");
            after = pse::LoadFolderViews();
            matches = 0; cols = 0; shippedStill = false;
            for (auto& v : after) if (!_wcsicmp(v.name.c_str(), L"plugins")) { ++matches; cols = v.columns.size(); shippedStill = v.shipped; }
            Expect(after.size() == 7 && matches == 1 && cols == 17 && shippedStill, L"removing it brings the preset back");
            win->Quit();
            Pump(800);
        }
    }

    // Storage.
    pse::FolderViewState stored = a;
    stored.name = L"Mods & \u00e9";
    Expect(pse::StoreFolderView(stored), L"a view saves");
    stored.columns[0].width = 301;
    Expect(pse::StoreFolderView(stored), L"saving the same name replaces it");
    auto list = Saved();
    Expect(list.size() == 1 && list[0].name == stored.name && Same(stored, list[0]), L"it loads back identically, with an accented name");
    Expect(pse::RemoveFolderView(stored.name) && Saved().empty(), L"it can be removed");
    {
        std::ofstream f(std::filesystem::path(pse::FolderViewsFilePath()), std::ios::binary);
        f << "{ not json";
    }
    Expect(Saved().empty(), L"a damaged file loads as empty");
    Expect(pse::StoreFolderView(stored) && Saved().size() == 1, L"saving after damage works");
    Expect(GetFileAttributesW((pse::FolderViewsFilePath() + L".damaged").c_str()) != INVALID_FILE_ATTRIBUTES, L"the damaged file is kept aside");
    wprintf(L"failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
}  // namespace views

// The right-drag handler as Explorer drives it: dropped files, target folder, menu, click.
int BsaDrop(const wchar_t* dllPath, const wchar_t* source, const wchar_t* target, int command) {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE dll = LoadLibraryW(dllPath);
    auto get = dll ? reinterpret_cast<HRESULT(__stdcall*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    if (!get) return 2;
    ComPtr<IClassFactory> factory;
    ComPtr<IShellExtInit> init;
    ComPtr<IShellItem> item;
    ComPtr<IDataObject> data;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(get(pse::kClsidBsaDrop, IID_PPV_ARGS(&factory))) || FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&init)))) return 2;
    if (FAILED(SHCreateItemFromParsingName(source, nullptr, IID_PPV_ARGS(&item))) || FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) return 2;
    if (FAILED(SHParseDisplayName(target, nullptr, &pidl, 0, nullptr))) return 2;
    const HRESULT hr = init->Initialize(pidl, data.Get(), nullptr);
    CoTaskMemFree(pidl);
    wprintf(L"initialize=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 3;
    ComPtr<IContextMenu> menu;
    init.As(&menu);
    HMENU popup = CreatePopupMenu();
    wprintf(L"defaultonly adds %d\n", HRESULT_CODE(menu->QueryContextMenu(popup, 0, 100, 200, CMF_DEFAULTONLY)));
    const HRESULT added = menu->QueryContextMenu(popup, 0, 100, 200, CMF_NORMAL);
    wchar_t text[260] = {};
    GetMenuStringW(popup, 0, text, ARRAYSIZE(text), MF_BYPOSITION);
    HMENU sub = GetSubMenu(popup, 0);
    wprintf(L"added=%d top=[%ls] items=%d", HRESULT_CODE(added), text, sub ? GetMenuItemCount(sub) : -1);
    for (int i = 0; sub && i < GetMenuItemCount(sub); ++i) {
        GetMenuStringW(sub, i, text, ARRAYSIZE(text), MF_BYPOSITION);
        wprintf(L" [%ls]", text);
    }
    wprintf(L"\n");
    if (command < 0) return 0;
    CMINVOKECOMMANDINFO info{sizeof(info)};
    info.lpVerb = MAKEINTRESOURCEA(command);
    info.nShow = SW_SHOWNORMAL;
    wprintf(L"invoke=0x%08lx\n", static_cast<unsigned long>(menu->InvokeCommand(&info)));
    const std::wstring destination = command == 1 ? std::wstring(target) + L"\\" + std::filesystem::path(source).stem().wstring() : target;
    uint64_t last = 0;
    int same = 0;
    for (int i = 0; i < 1200 && same < 15; ++i) {
        Sleep(100);
        const uint64_t now = pse::CountExisting(source, destination);
        same = now && now == last ? same + 1 : 0;
        last = now;
    }
    wprintf(L"extracted=%llu into [%ls]\n", static_cast<unsigned long long>(last), destination.c_str());
    return 0;
}

int HeaderUpgrade(const wchar_t* path) {
    const pse::HeaderUpgradeResult r = pse::UpgradePluginHeader(path);
    wprintf(L"%d %ls\n", r.status, r.text.c_str());
    return r.status;
}

// The plugin menu as Explorer drives it; clicks Upgrade header and closes its message.
int PluginMenu(const wchar_t* dllPath, const wchar_t* plugin) {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE dll = LoadLibraryW(dllPath);
    auto get = dll ? reinterpret_cast<HRESULT(__stdcall*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    ComPtr<IClassFactory> factory;
    ComPtr<IShellExtInit> init;
    ComPtr<IShellItem> item;
    ComPtr<IDataObject> data;
    if (!get || FAILED(get(pse::kClsidContextMenu, IID_PPV_ARGS(&factory))) || FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&init)))) return 2;
    if (FAILED(SHCreateItemFromParsingName(plugin, nullptr, IID_PPV_ARGS(&item))) || FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) return 2;
    if (FAILED(init->Initialize(nullptr, data.Get(), nullptr))) return 3;
    ComPtr<IContextMenu> menu;
    init.As(&menu);
    HMENU popup = CreatePopupMenu();
    const HRESULT added = menu->QueryContextMenu(popup, 0, 100, 200, CMF_NORMAL);
    wchar_t text[260] = {};
    GetMenuStringW(popup, 0, text, ARRAYSIZE(text), MF_BYPOSITION);
    HMENU sub = GetSubMenu(popup, 0);
    wprintf(L"added=%d top=[%ls]", HRESULT_CODE(added), text);
    for (int i = 0; sub && i < GetMenuItemCount(sub); ++i) {
        GetMenuStringW(sub, i, text, ARRAYSIZE(text), MF_BYPOSITION);
        wprintf(L" [%ls id=%u]", text, GetMenuItemID(sub, i));
    }
    wprintf(L"\n");
    CMINVOKECOMMANDINFO info{sizeof(info)};
    info.lpVerb = MAKEINTRESOURCEA(1);
    info.nShow = SW_SHOWNORMAL;
    wprintf(L"invoke=0x%08lx\n", static_cast<unsigned long>(menu->InvokeCommand(&info)));
    for (int i = 0; i < 100; ++i) {
        Sleep(100);
        if (HWND box = FindWindowW(L"#32770", L"Upgrade header")) {
            HWND label = FindWindowExW(box, nullptr, L"Static", nullptr);
            while (label && !GetWindowTextLengthW(label)) label = FindWindowExW(box, label, L"Static", nullptr);
            if (label) GetWindowTextW(label, text, ARRAYSIZE(text));
            wprintf(L"message=[%ls]\n", text);
            PostMessageW(box, WM_CLOSE, 0, 0);
            Sleep(400);
            return 0;
        }
    }
    wprintf(L"no message\n");
    return 1;
}

// One line per file: "path<TAB>pid=value|..." for the quick and the full facts.
void PrintFacts(const std::vector<pse::AssetFact>& facts) {
    for (const auto& f : facts) {
        if (f.type == pse::AssetFact::Type::Text) wprintf(L"%lu=%ls|", f.pid, f.text.c_str());
        else wprintf(L"%lu=%llu|", f.pid, static_cast<unsigned long long>(f.number));
    }
}
int AssetFacts(const wchar_t* listPath) {
    std::wifstream list{std::filesystem::path(listPath)};
    std::wstring line;
    _setmode(_fileno(stdout), _O_U8TEXT);
    while (std::getline(list, line)) {
        if (line.empty()) continue;
        wprintf(L"%ls\tQ:", line.c_str());
        PrintFacts(pse::ReadQuickAssetFacts(line));
        wprintf(L"\tF:");
        const auto full = pse::ReadFullAssetFacts(line);
        PrintFacts(full);
        std::vector<pse::AssetFact> back;
        wprintf(L"\tRT:%d\n", pse::DecodeAssetFacts(pse::EncodeAssetFacts(full), &back) && back.size() == full.size());
    }
    return 0;
}

// The real COM handler, as Explorer would use it: every property it reports.
int FileColumns(const wchar_t* dllPath, const wchar_t* file) {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(dllPath);
    auto get = dll ? reinterpret_cast<HRESULT(__stdcall*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    ComPtr<IClassFactory> factory;
    ComPtr<IInitializeWithFile> init;
    if (!get || FAILED(get(pse::kClsidFilePropertyHandler, IID_PPV_ARGS(&factory))) || FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&init)))) return 2;
    if (FAILED(init->Initialize(file, STGM_READ))) return 3;
    ComPtr<IPropertyStore> store;
    init.As(&store);
    DWORD count = 0;
    store->GetCount(&count);
    for (int pass = 0; pass < 2; ++pass) {
        wprintf(L"pass %d, %lu properties\n", pass, count);
        for (DWORD i = 0; i < count; ++i) {
            PROPERTYKEY key;
            store->GetAt(i, &key);
            PROPVARIANT value;
            PropVariantInit(&value);
            store->GetValue(key, &value);
            PWSTR text = nullptr;
            PropVariantToStringAlloc(value, &text);
            wprintf(L"  pid %lu vt=%d [%ls]\n", key.pid, value.vt, text ? text : L"");
            CoTaskMemFree(text);
            PropVariantClear(&value);
        }
        if (GetEnvironmentVariableW(L"PSE_ASYNC_TEST", nullptr, 0) == 0) break;
        Sleep(2500);
        init.Reset();
        store.Reset();
        if (FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&init))) || FAILED(init->Initialize(file, STGM_READ))) return 3;
        init.As(&store);
    }
    return 0;
}

// Asks Windows for each picture's thumbnail the way a view does, through a shell item.
int BsaThumbs(const wchar_t* dllPath, const wchar_t* archive, const wchar_t* folderName, int limit, const wchar_t* bmp) {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(dllPath);
    auto get = dll ? reinterpret_cast<HRESULT(__stdcall*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    ComPtr<IClassFactory> factory;
    ComPtr<IPersistFolder> persist;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!get || FAILED(get(pse::kClsidBsaFolder, IID_PPV_ARGS(&factory))) || FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&persist)))) return 2;
    if (FAILED(SHParseDisplayName(archive, nullptr, &pidl, 0, nullptr)) || FAILED(persist->Initialize(pidl))) return 2;
    ComPtr<IShellFolder> root, folder;
    persist.As(&root);
    folder = root;
    std::wstring path = folderName;
    size_t at = 0;
    while (at < path.size()) {
        size_t end = path.find(L'\\', at);
        if (end == std::wstring::npos) end = path.size();
        std::wstring part = path.substr(at, end - at);
        at = end + 1;
        PIDLIST_RELATIVE child = nullptr;
        ComPtr<IShellFolder> next;
        if (FAILED(folder->ParseDisplayName(nullptr, nullptr, part.data(), nullptr, &child, nullptr)) || FAILED(folder->BindToObject(child, nullptr, IID_PPV_ARGS(&next)))) return 3;
        CoTaskMemFree(child);
        folder = next;
    }
    ComPtr<IEnumIDList> list;
    if (folder->EnumObjects(nullptr, SHCONTF_NONFOLDERS, &list) != S_OK) return 3;
    PITEMID_CHILD item = nullptr;
    int tried = 0, made = 0, refused = 0;
    HRESULT lastFail = S_OK;
    bool saved = false;
    while (tried < limit && list->Next(1, &item, nullptr) == S_OK) {
        ComPtr<IThumbnailProvider> direct;
        PCUITEMID_CHILD one = item;
        HRESULT hr = folder->GetUIObjectOf(nullptr, 1, &one, IID_IThumbnailProvider, nullptr, reinterpret_cast<void**>(direct.GetAddressOf()));
        if (FAILED(hr)) { ++refused; CoTaskMemFree(item); continue; }
        ++tried;
        HBITMAP bitmap = nullptr;
        WTS_ALPHATYPE alpha;
        hr = direct->GetThumbnail(256, &bitmap, &alpha);
        if (SUCCEEDED(hr) && bitmap) {
            ++made;
            BITMAP b{};
            GetObjectW(bitmap, sizeof(b), &b);
            if (!saved) {
                wprintf(L"first thumbnail %ldx%ld bits=%d\n", b.bmWidth, b.bmHeight, b.bmBitsPixel);
                HDC dc = GetDC(nullptr);
                BITMAPINFOHEADER h{sizeof(h)};
                h.biWidth = b.bmWidth; h.biHeight = -b.bmHeight; h.biPlanes = 1; h.biBitCount = 32;
                std::vector<uint8_t> px(4 * b.bmWidth * b.bmHeight);
                GetDIBits(dc, bitmap, 0, b.bmHeight, px.data(), reinterpret_cast<BITMAPINFO*>(&h), DIB_RGB_COLORS);
                ReleaseDC(nullptr, dc);
                h.biHeight = b.bmHeight;
                std::ofstream f{std::filesystem::path(bmp), std::ios::binary};
                BITMAPFILEHEADER fh{0x4D42, static_cast<DWORD>(54 + px.size()), 0, 0, 54};
                f.write(reinterpret_cast<char*>(&fh), sizeof(fh));
                f.write(reinterpret_cast<char*>(&h), sizeof(h));
                for (LONG y = b.bmHeight - 1; y >= 0; --y) f.write(reinterpret_cast<char*>(px.data()) + 4 * b.bmWidth * y, 4 * b.bmWidth);
                saved = true;
            }
            DeleteObject(bitmap);
        } else {
            lastFail = hr;
        }
        CoTaskMemFree(item);
    }
    wprintf(L"tried=%d thumbnails=%d failed=%d not-pictures=%d lastFail=0x%08lx\n", tried, made, tried - made, refused, static_cast<unsigned long>(lastFail));
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    int left = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((std::wstring(temp) + L"WHAM thumbnails\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do { if (fd.cFileName[0] != L'.') ++left; } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    wprintf(L"temporary files left behind=%d\n", left);
    return 0;
}

int NameWindowShot(const wchar_t* dllPath, const wchar_t* png) {
    HMODULE dll = LoadLibraryW(dllPath);
    auto run = dll ? reinterpret_cast<int(__stdcall*)(const wchar_t*)>(GetProcAddress(dll, "PseFolderViewNameSnapshot")) : nullptr;
    return run ? run(png) : 2;
}

// The same 64 MB of stack the menu command runs with.
int PapyrusList(const wchar_t* listPath, const wchar_t* data) {
    struct Args { const wchar_t* list; const wchar_t* data; } args{listPath, data};
    HANDLE t = CreateThread(nullptr, 64u * 1024 * 1024, [](void* p) -> DWORD {
        auto* a = static_cast<Args*>(p);
        return static_cast<DWORD>(PapyrusListOnThisThread(a->list, a->data));
    }, &args, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return 2;
    WaitForSingleObject(t, INFINITE);
    DWORD code = 2;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    return static_cast<int>(code);
}

int PropsWindow(const wchar_t* dllPath, const wchar_t* archive, const wchar_t* png) {
    HMODULE dll = LoadLibraryW(dllPath);
    auto run = dll ? reinterpret_cast<void(CALLBACK*)(HWND, HINSTANCE, LPWSTR, int)>(GetProcAddress(dll, "SnapshotW")) : nullptr;
    if (!run) return 2;
    std::wstring line = L"\"" + std::wstring(archive) + L"\" \"" + png + L"\"";
    run(nullptr, nullptr, line.data(), 0);
    return 0;
}

int OpenInside(const wchar_t* dllPath, const wchar_t* archive, const wchar_t* folderName, const wchar_t* fileName) {
    HMODULE dll = LoadLibraryW(dllPath);
    auto getClassObject = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    IClassFactory* factory = nullptr;
    if (!getClassObject || FAILED(getClassObject(pse::kClsidBsaFolder, IID_PPV_ARGS(&factory)))) return 2;
    LPITEMIDLIST pidl = nullptr;
    IPersistFolder* persist = nullptr;
    IShellFolder* root = nullptr;
    if (FAILED(SHParseDisplayName(archive, nullptr, &pidl, 0, nullptr)) || FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&persist))) ||
        FAILED(persist->Initialize(pidl)) || FAILED(persist->QueryInterface(IID_PPV_ARGS(&root))))
        return 2;
    wchar_t name[MAX_PATH];
    wcscpy_s(name, folderName);
    LPITEMIDLIST folderItem = nullptr;
    IShellFolder* folder = nullptr;
    if (FAILED(root->ParseDisplayName(nullptr, nullptr, name, nullptr, &folderItem, nullptr)) || FAILED(root->BindToObject(folderItem, nullptr, IID_PPV_ARGS(&folder)))) return 2;
    wcscpy_s(name, fileName);
    LPITEMIDLIST fileItem = nullptr;
    if (FAILED(folder->ParseDisplayName(nullptr, nullptr, name, nullptr, &fileItem, nullptr))) return 2;
    PCUITEMID_CHILD one = fileItem;
    IContextMenu* menu = nullptr;
    if (FAILED(folder->GetUIObjectOf(nullptr, 1, &one, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&menu)))) return 2;
    CMINVOKECOMMANDINFO info{sizeof(info)};
    info.lpVerb = "open";
    info.nShow = SW_HIDE;
    const HRESULT hr = menu->InvokeCommand(&info);
    wprintf(L"open: %08x\n", static_cast<unsigned>(hr));
    menu->Release();
    return FAILED(hr) ? 1 : 0;
}

std::wstring Read(const std::wstring& key, const wchar_t* value) {
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? buf : L"";
}

bool Exists(const std::wstring& key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

int Registration() {
    const std::wstring base = L"Software\\PluginShellExtensionBsaTest\\";
    const std::wstring classes = base + L"Classes\\", ext = L".bsa";
    const std::wstring extKey = classes + ext, prog = classes + pse::kBsaProgId, clsid = classes + L"CLSID\\" + pse::kBsaFolderClsidString;
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    const std::wstring old = L"bsa_auto_file";
    RegSetKeyValueW(HKEY_CURRENT_USER, extKey.c_str(), nullptr, REG_SZ, old.c_str(), static_cast<DWORD>((old.size() + 1) * sizeof(wchar_t)));
    RegSetKeyValueW(HKEY_CURRENT_USER, (extKey + L"\\OpenWithProgids").c_str(), pse::kBsaProgId, REG_NONE, nullptr, 0);
    Check(pse::InstallBsaBrowser(HKEY_CURRENT_USER, classes, ext, L"C:\\x.dll"), L"install succeeds");
    Check(!Exists(extKey + L"\\OpenWithProgids"), L"no OpenWithProgids entry is left to trigger the program prompt");
    Check(Exists(prog + L"\\DefaultIcon"), L"the type has the zip icon");
    Check(Read(prog + L"\\shell\\open", L"FriendlyAppName") == L"WHAM", L"the open verb has our name");
    Check(Read(prog + L"\\Application", L"ApplicationName") == L"WHAM", L"the type has our application name");
    Check(Read(extKey, nullptr) == pse::kBsaProgId, L"the extension points at our type");
    Check(Read(prog, pse::kPreviousProgIdValue) == old, L"the previous type is recorded");
    Check(Read(prog + L"\\CLSID", nullptr) == pse::kBsaFolderClsidString, L"the type names the folder");
    Check(Read(clsid + L"\\InprocServer32", nullptr) == L"C:\\x.dll", L"the DLL path is written");
    Check(Read(clsid + L"\\InprocServer32", L"ThreadingModel") == L"Apartment", L"Apartment threading");
    Check(Exists(classes + L"SystemFileAssociations\\.bsa\\shell\\BrowseContents\\command"), L"the Browse contents verb exists");
    Check(Read(classes + L"SystemFileAssociations\\.bsa\\shellex\\ContextMenuHandlers\\PluginShellExtensionBsaManager", nullptr) == pse::kBsaManagerClsidString, L"the BSA manager menu is on .bsa");
    Check(!Exists(classes + L"SystemFileAssociations\\.ba2"), L"nothing is registered for .ba2");
    Check(Exists(clsid + L"\\Implemented Categories\\{00021490-0000-0000-C000-000000000046}"), L"registered as a browsable folder");
    Check(pse::InstallBsaBrowser(HKEY_CURRENT_USER, classes, ext, L"C:\\y.dll"), L"reinstall succeeds");
    Check(Read(prog, pse::kPreviousProgIdValue) == old, L"a reinstall keeps the recorded type");
    pse::RemoveBsaBrowser(HKEY_CURRENT_USER, classes, ext);
    Check(Read(extKey, nullptr) == old, L"uninstall puts the previous type back");
    Check(!Exists(prog) && !Exists(clsid), L"uninstall removes our keys");
    Check(!Exists(classes + L"SystemFileAssociations\\.bsa\\shell\\BrowseContents"), L"uninstall removes the verb");
    Check(!Exists(classes + L"SystemFileAssociations\\.bsa\\shellex\\ContextMenuHandlers\\PluginShellExtensionBsaManager"), L"uninstall removes the BSA manager menu");
    Check(!Exists(classes + L"CLSID\\" + pse::kBsaManagerClsidString), L"uninstall removes the BSA manager class");

    pse::InstallBsaBrowser(HKEY_CURRENT_USER, classes, ext, L"C:\\x.dll");
    for (const wchar_t* t : {L"Directory\\shellex", L"Directory\\Background\\shellex", L"Drive\\shellex"}) Check(Read(classes + t + L"\\DragDropHandlers\\PluginShellExtensionBsaManager", nullptr) == pse::kBsaDropClsidString, L"the right-drag menu is registered on its target");
    Check(Read(classes + L"CLSID\\" + pse::kBsaDropClsidString + L"\\InprocServer32", nullptr) == L"C:\\x.dll", L"the right-drag menu names the DLL");
    pse::RemoveBsaBrowser(HKEY_CURRENT_USER, classes, ext);
    Check(!Exists(classes + L"Directory\\shellex\\DragDropHandlers\\PluginShellExtensionBsaManager") && !Exists(classes + L"Drive\\shellex\\DragDropHandlers\\PluginShellExtensionBsaManager") && !Exists(classes + L"CLSID\\" + pse::kBsaDropClsidString), L"uninstall removes the right-drag menu");
    Check(pse::InstallFolderViewsMenu(HKEY_CURRENT_USER, classes, L"C:\\x.dll"), L"the Folder views menu installs");
    Check(Read(classes + L"Directory\\Background\\shellex\\ContextMenuHandlers\\PluginShellExtensionFolderViews", nullptr) == pse::kFolderViewsMenuClsidString, L"the Folder views menu is on folder backgrounds");
    pse::RemoveFolderViewsMenu(HKEY_CURRENT_USER, classes);
    Check(!Exists(classes + L"Directory\\Background\\shellex\\ContextMenuHandlers\\PluginShellExtensionFolderViews") && !Exists(classes + L"CLSID\\" + pse::kFolderViewsMenuClsidString), L"uninstall removes the Folder views menu");
    Check(pse::InstallPapyrusMenu(HKEY_CURRENT_USER, classes, L"C:\\x.dll"), L"the Papyrus menu installs");
    for (const wchar_t* e : {L".pex", L".psc"}) Check(Read(classes + L"SystemFileAssociations\\" + e + L"\\shellex\\ContextMenuHandlers\\PluginShellExtensionPapyrus", nullptr) == pse::kPapyrusMenuClsidString, L"the Papyrus menu is on its extension");
    pse::RemovePapyrusMenu(HKEY_CURRENT_USER, classes);
    Check(!Exists(classes + L"SystemFileAssociations\\.pex\\shellex\\ContextMenuHandlers\\PluginShellExtensionPapyrus") && !Exists(classes + L"CLSID\\" + pse::kPapyrusMenuClsidString), L"uninstall removes the Papyrus menu");
    Check(pse::InstallHashMenu(HKEY_CURRENT_USER, classes, L"C:\\x.dll"), L"the hash menu installs");
    Check(Read(classes + L"*\\shellex\\ContextMenuHandlers\\PluginShellExtensionHashes", nullptr) == pse::kHashMenuClsidString, L"the hash menu is on every file");
    pse::RemoveHashMenu(HKEY_CURRENT_USER, classes);
    Check(!Exists(classes + L"*\\shellex\\ContextMenuHandlers\\PluginShellExtensionHashes"), L"uninstall removes the hash menu");

    pse::InstallBsaBrowser(HKEY_CURRENT_USER, classes, ext, L"C:\\x.dll");
    const std::wstring later = L"SomeOtherTool.bsa";
    RegSetKeyValueW(HKEY_CURRENT_USER, extKey.c_str(), nullptr, REG_SZ, later.c_str(), static_cast<DWORD>((later.size() + 1) * sizeof(wchar_t)));
    pse::RemoveBsaBrowser(HKEY_CURRENT_USER, classes, ext);
    Check(Read(extKey, nullptr) == later, L"uninstall leaves a later association alone");

    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    pse::InstallBsaBrowser(HKEY_CURRENT_USER, classes, ext, L"C:\\x.dll");
    pse::RemoveBsaBrowser(HKEY_CURRENT_USER, classes, ext);
    Check(Read(extKey, nullptr).empty(), L"nothing is left set when nothing was there");
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, base.substr(0, base.size() - 1).c_str());
    return g_failures ? 1 : 0;
}

void Pump(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
}

IFolderView2* WaitForView(IExplorerBrowser* browser, int minItems, DWORD ms, IFolderView2* ignore = nullptr) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        Pump(50);
        IFolderView2* view = nullptr;
        if (SUCCEEDED(browser->GetCurrentView(IID_PPV_ARGS(&view))) && view) {
            int count = 0;
            if (view != ignore && SUCCEEDED(view->ItemCount(SVGIO_ALLVIEW, &count)) && count >= minItems) return view;
            view->Release();
        }
    }
    return nullptr;
}

size_t CountFiles(const std::wstring& dir) {
    size_t n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) n += CountFiles(dir + L"\\" + fd.cFileName);
        else ++n;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

int ExplorerTest(const wchar_t* dll, const wchar_t* archive, const wchar_t* dest) {
    const std::wstring classes = L"Software\\Classes\\", ext = L".psebsatest";
    wchar_t temp[MAX_PATH];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring test = std::wstring(temp) + L"psebsatest" + ext;
    DeleteFileW(test.c_str());
    if (!CreateHardLinkW(test.c_str(), archive, nullptr)) CopyFileW(archive, test.c_str(), FALSE);
    Check(pse::InstallBsaBrowser(HKEY_CURRENT_USER, classes, ext, dll), L"registered for a test extension");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    Pump(300);

    LPITEMIDLIST pidl = nullptr;
    HRESULT hr = SHParseDisplayName(test.c_str(), nullptr, &pidl, 0, nullptr);
    Check(SUCCEEDED(hr), L"Windows parses the archive's path");
    IShellFolder2* root = nullptr;
    hr = pidl ? SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&root)) : E_FAIL;
    Check(SUCCEEDED(hr), L"Windows binds the file to the folder");
    wprintf(L"     bind hr %08x\n", static_cast<unsigned>(hr));
    if (!root && pidl) {
        IShellFolder* plain = nullptr;
        HRESULT plainHr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&plain));
        wprintf(L"     IShellFolder: %08x\n", static_cast<unsigned>(plainHr));
        if (plain) plain->Release();
        IShellFolder* parent = nullptr;
        PCUITEMID_CHILD child = nullptr;
        if (SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child))) {
            SFGAOF attrs = SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_STREAM | SFGAO_FILESYSTEM;
            HRESULT attrHr = parent->GetAttributesOf(1, &child, &attrs);
            wprintf(L"     file attributes: %08x hr %08x\n", attrs, static_cast<unsigned>(attrHr));
            IShellFolder* viaParent = nullptr;
            HRESULT viaHr = parent->BindToObject(child, nullptr, IID_PPV_ARGS(&viaParent));
            wprintf(L"     BindToObject from parent: %08x\n", static_cast<unsigned>(viaHr));
            if (viaParent) viaParent->Release();
            parent->Release();
        }
        IPersistFolder* direct = nullptr;
        HRESULT createHr = CoCreateInstance(pse::kClsidBsaFolder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&direct));
        wprintf(L"     CoCreateInstance: %08x\n", static_cast<unsigned>(createHr));
        if (direct) {
            HRESULT initHr = direct->Initialize(pidl);
            wprintf(L"     Initialize: %08x\n", static_cast<unsigned>(initHr));
            direct->Release();
        }
        wchar_t assoc[256] = {};
        DWORD size = ARRAYSIZE(assoc);
        AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYDOCNAME, ext.c_str(), nullptr, assoc, &size);
        wprintf(L"     type name: %s\n", assoc);
        size = ARRAYSIZE(assoc);
        assoc[0] = 0;
        AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_SHELLEXTENSION, ext.c_str(), L"{000214E6-0000-0000-C000-000000000046}", assoc, &size);
        wprintf(L"     shell extension: %s\n", assoc);
    }

    if (root) {
        CLSID clsid{};
        IPersist* persist = nullptr;
        if (SUCCEEDED(root->QueryInterface(IID_PPV_ARGS(&persist)))) {
            persist->GetClassID(&clsid);
            persist->Release();
        }
        Check(IsEqualCLSID(clsid, pse::kClsidBsaFolder), L"the bound folder is ours");

        void* p = nullptr;
        Check(root->CreateViewObject(nullptr, IID_IDropTarget, &p) == E_NOINTERFACE, L"read-only: the folder is no drop target");
        IEnumIDList* e = nullptr;
        LPITEMIDLIST first = nullptr;
        ULONG got = 0;
        if (SUCCEEDED(root->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &e)) && e && e->Next(1, &first, &got) == S_OK) {
            PCUITEMID_CHILD one = first;
            SFGAOF attrs = SFGAO_CANDELETE | SFGAO_CANRENAME | SFGAO_CANMOVE | SFGAO_DROPTARGET | SFGAO_CANCOPY;
            root->GetAttributesOf(1, &one, &attrs);
            Check(!(attrs & (SFGAO_CANDELETE | SFGAO_CANRENAME | SFGAO_CANMOVE | SFGAO_DROPTARGET)), L"read-only: no delete, rename, move or drop");
            Check((attrs & SFGAO_CANCOPY) != 0, L"items can be copied");
            Check(root->SetNameOf(nullptr, first, L"x", SHGDN_NORMAL, nullptr) == E_ACCESSDENIED, L"read-only: rename is refused");
            p = nullptr;
            Check(root->GetUIObjectOf(nullptr, 1, &one, IID_IDropTarget, nullptr, &p) == E_NOINTERFACE, L"read-only: items refuse drops");
            CoTaskMemFree(first);
        }
        if (e) e->Release();
    }

    HWND window = CreateWindowExW(0, L"STATIC", L"bsabrowsetest", WS_OVERLAPPEDWINDOW, 0, 0, 900, 700, nullptr, nullptr, nullptr, nullptr);
    IExplorerBrowser* browser = nullptr;
    hr = CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser));
    Check(SUCCEEDED(hr), L"created Windows' real folder view");
    if (browser && pidl) {
        RECT rc{0, 0, 880, 660};
        FOLDERSETTINGS fs{FVM_DETAILS, 0};
        browser->Initialize(window, &rc, &fs);
        browser->SetOptions(EBO_NOBORDER);
        hr = browser->BrowseToIDList(pidl, SBSP_ABSOLUTE);
        Check(SUCCEEDED(hr), L"browsed to the archive");
        IFolderView2* view = WaitForView(browser, 1, 30000);
        Check(view != nullptr, L"the view lists the archive's contents");
        if (view) {
            int count = 0;
            view->ItemCount(SVGIO_ALLVIEW, &count);
            wprintf(L"     root items: %d\n", count);
            IShellFolder* folder = nullptr;
            view->GetFolder(IID_PPV_ARGS(&folder));
            // A folder item, opened the way a double-click does.
            int folderIndex = -1;
            for (int i = 0; i < count && folderIndex < 0; ++i) {
                LPITEMIDLIST item = nullptr;
                if (SUCCEEDED(view->Item(i, &item))) {
                    PCUITEMID_CHILD one = item;
                    SFGAOF attrs = SFGAO_FOLDER;
                    folder->GetAttributesOf(1, &one, &attrs);
                    if (attrs & SFGAO_FOLDER) folderIndex = i;
                    CoTaskMemFree(item);
                }
            }
            IFolderView2* inner = nullptr;
            if (folderIndex >= 0) {
                view->SelectItem(folderIndex, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
                IContextMenu* menu = nullptr;
                hr = view->Items(SVGIO_SELECTION, IID_PPV_ARGS(&menu));
                Check(SUCCEEDED(hr), L"got the right-click menu");
                if (menu) {
                    IUnknown_SetSite(menu, view);
                    HMENU popup = CreatePopupMenu();
                    menu->QueryContextMenu(popup, 0, 1, 0x7FFF, CMF_DEFAULTONLY);
                    const UINT def = GetMenuDefaultItem(popup, FALSE, 0);
                    Check(def == 1, L"Open is the default command");
                    CMINVOKECOMMANDINFO info{sizeof(info)};
                    info.hwnd = window;
                    info.lpVerb = MAKEINTRESOURCEA(def - 1);
                    info.nShow = SW_SHOWNORMAL;
                    hr = menu->InvokeCommand(&info);
                    Check(SUCCEEDED(hr), L"double-click on a folder runs");
                    IUnknown_SetSite(menu, nullptr);
                    menu->Release();
                    DestroyMenu(popup);
                }
                inner = WaitForView(browser, 1, 30000, view);
                Check(inner != nullptr, L"the view moved into the folder");
            }
            IFolderView2* source = inner ? inner : view;
            if (inner) {
                int n = 0;
                inner->ItemCount(SVGIO_ALLVIEW, &n);
                wprintf(L"     items in that folder: %d\n", n);
            }
            int n = 0;
            source->ItemCount(SVGIO_ALLVIEW, &n);
            for (int i = 0; i < n && i < 40; ++i) source->SelectItem(i, i ? SVSI_SELECT : SVSI_SELECT | SVSI_DESELECTOTHERS);
            IDataObject* data = nullptr;
            hr = source->Items(SVGIO_SELECTION, IID_PPV_ARGS(&data));
            Check(SUCCEEDED(hr) && data, L"got the selection to copy");
            LPITEMIDLIST destPidl = nullptr;
            IShellFolder* parent = nullptr;
            PCUITEMID_CHILD destChild = nullptr;
            IDropTarget* target = nullptr;
            if (data && SUCCEEDED(SHParseDisplayName(dest, nullptr, &destPidl, 0, nullptr)) &&
                SUCCEEDED(SHBindToParent(destPidl, IID_PPV_ARGS(&parent), &destChild)) &&
                SUCCEEDED(parent->GetUIObjectOf(window, 1, &destChild, IID_IDropTarget, nullptr, reinterpret_cast<void**>(&target)))) {
                DWORD effect = DROPEFFECT_COPY;
                POINTL pt{0, 0};
                hr = target->DragEnter(data, MK_LBUTTON, pt, &effect);
                Check(SUCCEEDED(hr) && (effect & DROPEFFECT_COPY), L"the destination accepts the drop as a copy");
                effect = DROPEFFECT_COPY;
                hr = target->Drop(data, MK_LBUTTON, pt, &effect);
                Check(SUCCEEDED(hr), L"dropped into a real folder");
                size_t last = 0;
                int quiet = 0;
                for (int i = 0; i < 300 && quiet < 15; ++i) {
                    Pump(200);
                    const size_t now = CountFiles(dest);
                    quiet = now == last && now ? quiet + 1 : 0;
                    last = now;
                }
                wprintf(L"     files written: %zu\n", last);
                Check(last > 0, L"files arrived in the destination");
                target->Release();
            }
            if (parent) parent->Release();
            if (destPidl) CoTaskMemFree(destPidl);
            if (data) data->Release();

            // Copy, the way Ctrl+C does it.
            source->SelectItem(0, SVSI_SELECT | SVSI_DESELECTOTHERS);
            IContextMenu* menu = nullptr;
            if (SUCCEEDED(source->Items(SVGIO_SELECTION, IID_PPV_ARGS(&menu))) && menu) {
                CMINVOKECOMMANDINFO info{sizeof(info)};
                info.hwnd = window;
                info.lpVerb = "copy";
                Check(SUCCEEDED(menu->InvokeCommand(&info)), L"Ctrl+C runs");
                IDataObject* clip = nullptr;
                const bool onClipboard = SUCCEEDED(OleGetClipboard(&clip)) && clip;
                Check(onClipboard, L"the clipboard holds the copy");
                if (clip) clip->Release();
                OleSetClipboard(nullptr);
                menu->Release();
            }
            if (folder) folder->Release();
            if (inner) inner->Release();
            view->Release();
        }
        browser->Destroy();
        browser->Release();
    }
    if (root) root->Release();
    if (pidl) CoTaskMemFree(pidl);
    DestroyWindow(window);
    pse::RemoveBsaBrowser(HKEY_CURRENT_USER, classes, ext);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    Pump(200);
    DeleteFileW(test.c_str());
    Check(!Exists(classes + pse::kBsaProgId), L"the test registration is gone");
    return g_failures ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (argc == 2 && !wcscmp(argv[1], L"--registration")) return Registration();
    if (argc == 2 && !wcscmp(argv[1], L"--budget")) return Budget();
    if (argc == 3 && !wcscmp(argv[1], L"--facts")) return Facts(argv[2]);
    if (argc == 5 && !wcscmp(argv[1], L"--extract")) return Extract(argv[2], argv[3], argv[4]);
    if (argc == 3 && !wcscmp(argv[1], L"--test")) return TestArchive(argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--hash")) return Hash(argv[2]);
    if (argc == 6 && !wcscmp(argv[1], L"--hashwindow")) return HashWindow(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 3 && !wcscmp(argv[1], L"--assetfacts")) return AssetFacts(argv[2]);
    if (argc == 4 && !wcscmp(argv[1], L"--filecolumns")) return FileColumns(argv[2], argv[3]);
    if (argc == 7 && !wcscmp(argv[1], L"--bsathumbs")) return BsaThumbs(argv[2], argv[3], argv[4], _wtoi(argv[5]), argv[6]);
    if (argc == 3 && !wcscmp(argv[1], L"--upgrade")) return HeaderUpgrade(argv[2]);
    if (argc == 4 && !wcscmp(argv[1], L"--pluginmenu")) return PluginMenu(argv[2], argv[3]);
    if (argc == 6 && !wcscmp(argv[1], L"--bsadrop")) return BsaDrop(argv[2], argv[3], argv[4], _wtoi(argv[5]));
    if (argc == 3 && !wcscmp(argv[1], L"--folderviews")) return views::Run(argv[2]);
    if (argc == 4 && !wcscmp(argv[1], L"--namewindow")) return NameWindowShot(argv[2], argv[3]);
    if (argc == 4 && !wcscmp(argv[1], L"--papyrus")) return PapyrusList(argv[2], argv[3]);
    if (argc == 5 && !wcscmp(argv[1], L"--propswindow")) return PropsWindow(argv[2], argv[3], argv[4]);
    if (argc == 5 && !wcscmp(argv[1], L"--reportwindow")) return ReportWindow(argv[2], argv[3], argv[4]);
    OleInitialize(nullptr);
    int rc = 2;
    if (argc >= 4 && !wcscmp(argv[1], L"--direct")) rc = Direct(argc, argv);
    else if (argc == 6 && !wcscmp(argv[1], L"--open")) rc = OpenInside(argv[2], argv[3], argv[4], argv[5]);
    else if (argc == 5 && !wcscmp(argv[1], L"--winner")) rc = WinnerTest(argv[2], argv[3], argv[4]);
    else if (argc == 5 && !wcscmp(argv[1], L"--explorer")) rc = ExplorerTest(argv[2], argv[3], argv[4]);
    else fwprintf(stderr, L"usage: see the top of bsabrowsetest.cpp\n");
    OleUninitialize();
    return rc;
}
