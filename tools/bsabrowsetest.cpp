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
    Check(Read(prog + L"\\shell\\open", L"FriendlyAppName") == L"Plugin Shell Extension", L"the open verb has our name");
    Check(Read(prog + L"\\Application", L"ApplicationName") == L"Plugin Shell Extension", L"the type has our application name");
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
    if (argc == 5 && !wcscmp(argv[1], L"--propswindow")) return PropsWindow(argv[2], argv[3], argv[4]);
    if (argc == 5 && !wcscmp(argv[1], L"--reportwindow")) return ReportWindow(argv[2], argv[3], argv[4]);
    OleInitialize(nullptr);
    int rc = 2;
    if (argc >= 4 && !wcscmp(argv[1], L"--direct")) rc = Direct(argc, argv);
    else if (argc == 6 && !wcscmp(argv[1], L"--open")) rc = OpenInside(argv[2], argv[3], argv[4], argv[5]);
    else if (argc == 5 && !wcscmp(argv[1], L"--explorer")) rc = ExplorerTest(argv[2], argv[3], argv[4]);
    else fwprintf(stderr, L"usage: see the top of bsabrowsetest.cpp\n");
    OleUninitialize();
    return rc;
}
