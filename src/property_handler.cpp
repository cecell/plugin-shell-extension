#include "property_handler.h"

#include <propsys.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <set>
#include <tuple>
#include <memory>
#include <vector>

#include "context_menu.h"
#include "dependents.h"
#include "debug_log.h"
#include "disk_cache.h"
#include "work_budget.h"
#include "embedded_patterns.h"
#include "itm.h"
#include "plugin_parser.h"

namespace pse {
namespace {

constexpr DWORD kPids[] = {kPidHeaderVersion, kPidRecordCount, kPidFlags, kPidLight,
                           kPidMasterCount,   kPidDeletedCount, kPidEslable, kPidMasters, kPidEslNote,
                           kPidFormVersion, kPidDeletedNavm, kPidFormCount, kPidItm, kPidDirty, kPidTipStatus, kPidDependentCount};

// The two values that need the full record walk, cached per process so a
// folder refresh does not rescan every plugin. Keyed by file name, size and
// last-write time (what the stream reports); a changed file gets a new key.
struct ScanSummary {
    bool ok = false;
    uint64_t deleted = 0;
    uint64_t deletedNavm = 0;
    uint64_t newRecords = 0;
    std::wstring eslable;
    bool needsNewerGame = false;
};
using ScanKey = std::tuple<std::wstring, uint64_t, uint64_t>;

std::mutex g_cacheLock;
std::map<ScanKey, ScanSummary> g_cache;
constexpr size_t kCacheLimit = 50000;

// The ITM check is slower still (it reads the masters). In Explorer it runs
// on a few background workers (WorkerThreadCount, from settings.json), smallest
// plugin first so most values appear quickly: the column says "Working" until
// the result is cached, then Explorer is told the file changed and asks
// again. Results are keyed by the plugin's and every master's size and write
// time, so cleaning a master refreshes its dependents too (on the next F5).
struct ItmSummary {
    bool ok = false;
    size_t count = 0;
};
std::map<std::wstring, ItmSummary> g_itmCache;
std::multimap<uint64_t, std::pair<std::wstring, std::wstring>> g_itmQueue;  // size -> (path, key)
std::set<std::wstring> g_itmPending;
int g_itmWorkers = 0;      // running
int g_itmWorkersWanted = 1;

// "size:writetime", or empty when the file is not there.
std::wstring FileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return L"";
    uint64_t size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    uint64_t time = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    return std::to_wstring(size) + L":" + std::to_wstring(time);
}

std::wstring LowerPath(std::wstring p) {
    if (!p.empty()) CharLowerBuffW(p.data(), static_cast<DWORD>(p.size()));
    return p;
}

// The cache key ("I|path|stamp|master stamps..."), or empty if a master is missing.
std::wstring ItmKey(const std::wstring& path, const std::vector<std::wstring>& masters) {
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/") + 1);
    std::wstring key = L"I|" + LowerPath(path) + L"|" + FileStamp(path);
    for (const auto& m : masters) {
        std::wstring stamp = FileStamp(dir + m);
        if (stamp.empty()) return L"";
        key += L"|" + stamp;
    }
    return key;
}

ItmSummary ComputeItm(const std::wstring& path) {
    ItmSummary s;
    if (const PatternCatalog* cat = EmbeddedPatterns()) {
        ItmResult r = FindItms(path, *cat);
        s.ok = r.ok && r.missingMasters.empty();
        s.count = r.itms.size();
    }
    return s;
}

void StoreItm(const std::wstring& key, const ItmSummary& s) {
    {
        std::lock_guard<std::mutex> c(g_cacheLock);
        if (g_itmCache.size() >= kCacheLimit) g_itmCache.clear();
        g_itmCache[key] = s;
    }
    if (s.ok) DiskCachePut(key, std::to_wstring(s.count));  // survives restarts
}

// From memory, else from the disk cache.
bool CachedItm(const std::wstring& key, ItmSummary* s) {
    {
        std::lock_guard<std::mutex> c(g_cacheLock);
        auto it = g_itmCache.find(key);
        if (it != g_itmCache.end()) {
            *s = it->second;
            return true;
        }
    }
    std::wstring value;
    if (!DiskCacheGet(key, &value)) return false;
    s->ok = true;
    s->count = static_cast<size_t>(_wtoi64(value.c_str()));
    std::lock_guard<std::mutex> c(g_cacheLock);
    g_itmCache[key] = *s;
    return true;
}

DWORD WINAPI ItmWorker(void* module) {
    // Background mode lowers this thread's CPU, disk and memory priority, so
    // a folder full of plugins never makes Explorer itself sluggish.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    // Explorer asks about a whole folder in one burst; wait for it to queue
    // so the first pick really is the smallest plugin.
    Sleep(250);
    for (;;) {
        std::pair<std::wstring, std::wstring> job;
        {
            std::lock_guard<std::mutex> c(g_cacheLock);
            // Stop when the queue is empty, or when the setting was lowered.
            if (g_itmQueue.empty() || g_itmWorkers > g_itmWorkersWanted) {
                --g_itmWorkers;
                break;
            }
            job = std::move(g_itmQueue.begin()->second);  // the smallest plugin
            g_itmQueue.erase(g_itmQueue.begin());
        }
        {
            WorkSlot slot(WorkKind::Background);  // shared with every other job in every process
            StoreItm(job.second, ComputeItm(job.first));
            slot.Pace();
        }
        {
            std::lock_guard<std::mutex> c(g_cacheLock);
            g_itmPending.erase(job.second);
        }
        // Explorer asks for the item's values again and now finds the result.
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, job.first.c_str(), nullptr);
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

// Queues the check and starts workers up to the setting; returns false if no
// worker is running and none could be started.
bool QueueItm(const std::wstring& path, const std::wstring& key, uint64_t size) {
    int wanted = WorkerThreadCount();  // reads settings.json when it changed
    std::lock_guard<std::mutex> c(g_cacheLock);
    g_itmWorkersWanted = wanted;
    if (g_itmPending.insert(key).second) g_itmQueue.emplace(size, std::make_pair(path, key));
    while (g_itmWorkers < g_itmWorkersWanted && g_itmWorkers < static_cast<int>(g_itmQueue.size())) {
        HMODULE module = nullptr;  // each worker keeps the DLL loaded until it is done
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ItmWorker), &module);
        HANDLE t = CreateThread(nullptr, 0, &ItmWorker, module, 0, nullptr);
        if (!t) {
            FreeLibrary(module);
            break;
        }
        CloseHandle(t);
        ++g_itmWorkers;
    }
    if (g_itmWorkers > 0) return true;
    g_itmPending.erase(key);
    for (auto it = g_itmQueue.begin(); it != g_itmQueue.end(); ++it) {
        if (it->second.second == key) {
            g_itmQueue.erase(it);
            break;
        }
    }
    return false;
}

// Watchdog: when a master changes (cleaned, updated), the plugins Explorer has
// shown that list it get refreshed, so their #ITM / Dirty are checked again
// without F5. One watcher thread per folder; it lives as long as Explorer.
std::mutex g_watchLock;
std::map<std::wstring, std::set<std::wstring>> g_dependents;  // lower master path -> plugin paths
std::set<std::wstring> g_watchedDirs;                          // lower folder paths

bool IsPluginName(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = LowerPath(name.substr(dot));
    return ext == L".esp" || ext == L".esm" || ext == L".esl";
}

DWORD WINAPI WatchThread(void* param) {
    std::unique_ptr<std::wstring> dir(static_cast<std::wstring*>(param));
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    HANDLE h = CreateFileW(dir->c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<uint8_t> buf(64 * 1024);
    std::set<std::wstring> changed;  // lower paths, waiting for the save to settle
    bool overflow = false;
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        if (!ReadDirectoryChangesW(h, buf.data(), static_cast<DWORD>(buf.size()), FALSE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                       FILE_NOTIFY_CHANGE_SIZE,
                                   nullptr, &ov, nullptr)) {
            break;
        }
        // Wait for changes; once some came in, 1.5 s of quiet means the save
        // (xEdit writes a temporary file, then renames it) is finished.
        bool quiet = false;
        while (WaitForSingleObject(ev, changed.empty() && !overflow ? INFINITE : 1500) == WAIT_TIMEOUT) {
            quiet = true;
            std::vector<std::wstring> refresh;
            {
                std::lock_guard<std::mutex> g(g_watchLock);
                for (const auto& [master, deps] : g_dependents) {
                    if (overflow || changed.count(master)) refresh.insert(refresh.end(), deps.begin(), deps.end());
                }
            }
            changed.clear();
            overflow = false;
            for (const auto& p : refresh) {
                DebugLog(L"watchdog: master changed, refreshing %s", p.c_str());
                SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, p.c_str(), nullptr);
            }
        }
        (void)quiet;
        DWORD bytes = 0;
        if (!GetOverlappedResult(h, &ov, &bytes, FALSE)) break;
        if (bytes == 0) {  // too many changes to list: refresh everything known
            overflow = true;
            continue;
        }
        for (DWORD off = 0;;) {
            auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf.data() + off);
            std::wstring name(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
            if (IsPluginName(name)) changed.insert(LowerPath(*dir + name));
            if (!fni->NextEntryOffset) break;
            off += fni->NextEntryOffset;
        }
    }
    CloseHandle(ev);
    CloseHandle(h);
    return 0;
}

// Remembers which masters `path` depends on and watches their folder.
void WatchForMasterChanges(const std::wstring& path, const std::vector<std::wstring>& masters) {
    if (masters.empty()) return;
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/") + 1);
    std::lock_guard<std::mutex> g(g_watchLock);
    for (const auto& m : masters) g_dependents[LowerPath(dir + m)].insert(path);
    if (!g_watchedDirs.insert(LowerPath(dir)).second) return;
    // The watcher keeps the DLL loaded for the rest of Explorer's life.
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&WatchThread), &module);
    if (HANDLE t = CreateThread(nullptr, 0, &WatchThread, new std::wstring(dir), 0, nullptr)) CloseHandle(t);
}

// Only Explorer gets "Working"; anything else (the search indexer) waits for
// the real value, so a placeholder never gets stored.
bool InExplorer() {
    static const bool explorer = [] {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* base = wcsrchr(exe, L'\\');
        // PSE_ASYNC_TEST=1 lets proptest exercise the Explorer path.
        return _wcsicmp(base ? base + 1 : exe, L"explorer.exe") == 0 ||
               GetEnvironmentVariableW(L"PSE_ASYNC_TEST", nullptr, 0) > 0;
    }();
    return explorer;
}

class PropertyHandler final : public IInitializeWithFile, public IPropertyStore, public IPropertyStoreCapabilities {
public:
    PropertyHandler() { InterlockedIncrement(&g_dllRefs); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithFile) {
            *ppv = static_cast<IInitializeWithFile*>(this);
        } else if (riid == IID_IPropertyStore) {
            *ppv = static_cast<IPropertyStore*>(this);
        } else if (riid == IID_IPropertyStoreCapabilities) {
            *ppv = static_cast<IPropertyStoreCapabilities*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // IInitializeWithFile (not a stream: the ITM check and the missing-master
    // marks need the plugin's folder). Only the header is read here; the
    // record walk waits until a value that needs it is asked for.
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD) override {
        if (!path) return E_INVALIDARG;
        std::lock_guard<std::mutex> g(lock_);
        if (!path_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        path_ = path;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
            key_ = ScanKey(path_, (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow,
                           (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
                               fa.ftLastWriteTime.dwLowDateTime);
        }
        header_ = ReadPluginHeader(path_);
        return S_OK;
    }

    // IPropertyStore
    IFACEMETHODIMP GetCount(DWORD* count) override {
        if (!count) return E_POINTER;
        *count = header_.ok ? ARRAYSIZE(kPids) : 0;
        return S_OK;
    }
    IFACEMETHODIMP GetAt(DWORD index, PROPERTYKEY* key) override {
        if (!key) return E_POINTER;
        if (!header_.ok || index >= ARRAYSIZE(kPids)) return E_INVALIDARG;
        *key = {kPluginFmtid, kPids[index]};
        return S_OK;
    }
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        if (!pv) return E_POINTER;
        PropVariantInit(pv);
        if (!header_.ok || key.fmtid != kPluginFmtid) return S_OK;  // VT_EMPTY: no value
        switch (key.pid) {
            case kPidHeaderVersion:
                return header_.hasHedr ? InitPropVariantFromString(VersionText(header_.version).c_str(), pv) : S_OK;
            case kPidRecordCount:
                return header_.hasHedr ? InitPropVariantFromUInt32(static_cast<ULONG>(header_.numRecords), pv) : S_OK;
            case kPidFlags: {
                std::wstring flags = FlagsText(header_.flags);
                return flags.empty() ? S_OK : InitPropVariantFromString(flags.c_str(), pv);
            }
            case kPidLight:
                return InitPropVariantFromString(header_.light ? L"True" : L"False", pv);
            case kPidMasterCount:
                return InitPropVariantFromUInt32(static_cast<ULONG>(header_.masters.size()), pv);
            case kPidDependentCount:
                return InitPropVariantFromUInt32(static_cast<ULONG>(DependentCount(path_)), pv);
            case kPidMasters: {
                // A master missing from the plugin's folder is marked "X ".
                if (header_.masters.empty()) return S_OK;
                std::vector<std::wstring> shown = MarkMissingMasters(path_, header_.masters);
                std::vector<PCWSTR> names;
                for (const auto& m : shown) names.push_back(m.c_str());
                return InitPropVariantFromStringVector(names.data(), static_cast<ULONG>(names.size()), pv);
            }
            case kPidItm: {
                // "MissingMaster" at once; "Working" while the check runs;
                // then the count, blank when there are none (like #Deleted).
                ItmSummary s;
                std::wstring state = ItmValue(&s);
                if (!state.empty()) return InitPropVariantFromString(state.c_str(), pv);
                return s.ok && s.count ? InitPropVariantFromString(std::to_wstring(s.count).c_str(), pv) : S_OK;
            }
            case kPidEslable:
                // Short form for the column; the dialog says "Already flagged".
                if (header_.light) return InitPropVariantFromString(L"Already", pv);
                return InitPropVariantFromString(Scan().eslable.c_str(), pv);
            case kPidFormVersion:
                return InitPropVariantFromUInt32(header_.formVersion, pv);
            case kPidDeletedNavm: {
                const ScanSummary& s = Scan();
                return s.ok && s.deletedNavm ? InitPropVariantFromUInt32(static_cast<ULONG>(s.deletedNavm), pv) : S_OK;
            }
            case kPidFormCount: {
                const ScanSummary& s = Scan();
                return s.ok ? InitPropVariantFromUInt32(static_cast<ULONG>(s.newRecords), pv) : S_OK;
            }
            case kPidEslNote:
                if (header_.light) return S_OK;
                return Scan().needsNewerGame ? InitPropVariantFromString(kNeedsNewerGameText, pv) : S_OK;
            case kPidDeletedCount: {
                // Placed objects only (navmeshes have Del NAVM). Blank rather
                // than 0, so plugins that need cleaning stand out.
                const ScanSummary& s = Scan();
                uint64_t n = s.deleted - s.deletedNavm;
                return s.ok && n ? InitPropVariantFromUInt32(static_cast<ULONG>(n), pv) : S_OK;
            }
            case kPidDirty: {
                std::wstring dirty = DirtyText();
                return dirty.empty() ? S_OK : InitPropVariantFromString(dirty.c_str(), pv);
            }
            case kPidTipStatus: {
                // Two values on one tooltip line (the tooltip gives every
                // property its own line).
                std::wstring dirty = DirtyText();
                std::wstring line = dirty.empty() ? L"" : L"Dirty: " + dirty + L"        ";
                line += header_.light ? L"Is Light: True" : L"Is Light: False";
                return InitPropVariantFromString(line.c_str(), pv);
            }
        }
        return S_OK;
    }
    IFACEMETHODIMP SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP Commit() override { return STG_E_ACCESSDENIED; }

    // IPropertyStoreCapabilities: everything is read-only.
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY) override { return S_FALSE; }

private:
    ~PropertyHandler() { InterlockedDecrement(&g_dllRefs); }

    // True when #Deleted or #ITM is not zero. #Deleted alone can settle it;
    // otherwise it waits for the ITM check like #ITM ("Working" /
    // "MissingMaster"). Empty when the plugin could not be scanned.
    std::wstring DirtyText() {
        const ScanSummary& sc = Scan();
        if (!sc.ok) return L"";
        if (sc.deleted > sc.deletedNavm) return L"True";
        ItmSummary s;
        std::wstring state = ItmValue(&s);
        if (!state.empty()) return state;
        if (!s.ok) return L"";
        return s.count ? L"True" : L"False";
    }

    // The ITM result into *s and an empty string, or "MissingMaster" /
    // "Working" while there is no result (#ITM and Dirty share this).
    std::wstring ItmValue(ItmSummary* s) {
        std::wstring itmKey = ItmKey(path_, header_.masters);
        if (itmKey.empty()) return L"MissingMaster";
        if (InExplorer()) WatchForMasterChanges(path_, header_.masters);
        if (CachedItm(itmKey, s)) return L"";
        if (InExplorer() && QueueItm(path_, itmKey, std::get<1>(key_))) return L"Working";
        *s = ComputeItm(path_);
        StoreItm(itmKey, *s);
        return L"";
    }

    const ScanSummary& Scan() {
        std::lock_guard<std::mutex> g(lock_);
        if (scanned_) return summary_;
        scanned_ = true;
        bool keyed = !std::get<0>(key_).empty();
        // "S|path|size|time" -> "ok,deleted,navm,forms,newer,eslable"
        std::wstring diskKey = L"S|" + LowerPath(std::get<0>(key_)) + L"|" + std::to_wstring(std::get<1>(key_)) +
                               L"|" + std::to_wstring(std::get<2>(key_));
        if (keyed) {
            {
                std::lock_guard<std::mutex> c(g_cacheLock);
                auto it = g_cache.find(key_);
                if (it != g_cache.end()) {
                    summary_ = it->second;
                    return summary_;
                }
            }
            std::wstring v;
            unsigned long long ok = 0, del = 0, navm = 0, forms = 0, newer = 0;
            int used = 0;
            if (DiskCacheGet(diskKey, &v) &&
                swscanf_s(v.c_str(), L"%llu,%llu,%llu,%llu,%llu,%n", &ok, &del, &navm, &forms, &newer, &used) == 5 &&
                used > 0) {
                summary_.ok = ok != 0;
                summary_.deleted = del;
                summary_.deletedNavm = navm;
                summary_.newRecords = forms;
                summary_.needsNewerGame = newer != 0;
                summary_.eslable = v.substr(used);
                std::lock_guard<std::mutex> c(g_cacheLock);
                g_cache[key_] = summary_;
                return summary_;
            }
        }
        GroupScan scan = ScanGroups(path_, header_);
        summary_.ok = scan.ok;
        summary_.deleted = scan.totalDeleted;
        summary_.deletedNavm = scan.deletedNavm;
        summary_.newRecords = scan.newRecords;
        EslStatus esl = ClassifyEsl(header_, scan);
        summary_.eslable = EslStatusText(esl, true);
        summary_.needsNewerGame = EslNeedsNewerGame(header_, scan, esl);
        if (scan.ok && keyed) {
            {
                std::lock_guard<std::mutex> c(g_cacheLock);
                if (g_cache.size() >= kCacheLimit) g_cache.clear();
                g_cache[key_] = summary_;
            }
            DiskCachePut(diskKey, std::to_wstring(summary_.ok) + L"," + std::to_wstring(summary_.deleted) + L"," +
                                      std::to_wstring(summary_.deletedNavm) + L"," +
                                      std::to_wstring(summary_.newRecords) + L"," +
                                      std::to_wstring(summary_.needsNewerGame) + L"," + summary_.eslable);
        }
        return summary_;
    }

    long refs_ = 1;
    std::mutex lock_;
    std::wstring path_;
    ScanKey key_;
    PluginHeader header_;
    bool scanned_ = false;
    ScanSummary summary_;
};

class Factory final : public IClassFactory {
public:
    Factory() { InterlockedIncrement(&g_dllRefs); }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* obj = new (std::nothrow) PropertyHandler();
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_dllRefs);
        else InterlockedDecrement(&g_dllRefs);
        return S_OK;
    }

private:
    ~Factory() { InterlockedDecrement(&g_dllRefs); }
    long refs_ = 1;
};

}  // namespace

HRESULT CreatePropertyHandlerFactory(REFIID riid, void** ppv) {
    auto* factory = new (std::nothrow) Factory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

}  // namespace pse
