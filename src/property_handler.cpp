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
#include <vector>

#include "context_menu.h"
#include "embedded_patterns.h"
#include "itm.h"
#include "plugin_parser.h"

namespace pse {
namespace {

constexpr DWORD kPids[] = {kPidHeaderVersion, kPidRecordCount, kPidFlags, kPidLight,
                           kPidMasterCount,   kPidDeletedCount, kPidEslable, kPidMasters, kPidEslNote,
                           kPidFormVersion, kPidDeletedNavm, kPidFormCount, kPidItm};

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
// on one background worker: the column says "Working" until the result is
// cached, then Explorer is told the file changed and asks again. Results are
// keyed by the plugin's and every master's size and write time, so cleaning
// a master refreshes its dependents too (on the next F5).
struct ItmSummary {
    bool ok = false;
    size_t count = 0;
};
std::map<std::wstring, ItmSummary> g_itmCache;
std::deque<std::pair<std::wstring, std::wstring>> g_itmQueue;  // (path, key)
std::set<std::wstring> g_itmPending;
bool g_itmWorkerRunning = false;

// "size:writetime", or empty when the file is not there.
std::wstring FileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return L"";
    uint64_t size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    uint64_t time = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    return std::to_wstring(size) + L":" + std::to_wstring(time);
}

// The cache key, or empty if a master is missing.
std::wstring ItmKey(const std::wstring& path, const std::vector<std::wstring>& masters) {
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/") + 1);
    std::wstring key = path + L"|" + FileStamp(path);
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
    std::lock_guard<std::mutex> c(g_cacheLock);
    if (g_itmCache.size() >= kCacheLimit) g_itmCache.clear();
    g_itmCache[key] = s;
}

DWORD WINAPI ItmWorker(void* module) {
    // Background mode lowers this thread's CPU, disk and memory priority, so
    // a folder full of plugins never makes Explorer itself sluggish.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;) {
        std::pair<std::wstring, std::wstring> job;
        {
            std::lock_guard<std::mutex> c(g_cacheLock);
            if (g_itmQueue.empty()) {
                g_itmWorkerRunning = false;
                break;
            }
            job = std::move(g_itmQueue.front());
            g_itmQueue.pop_front();
        }
        StoreItm(job.second, ComputeItm(job.first));
        {
            std::lock_guard<std::mutex> c(g_cacheLock);
            g_itmPending.erase(job.second);
        }
        // Explorer asks for the item's values again and now finds the result.
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, job.first.c_str(), nullptr);
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

// Queues the check; returns false if no worker could be started.
bool QueueItm(const std::wstring& path, const std::wstring& key) {
    std::lock_guard<std::mutex> c(g_cacheLock);
    if (g_itmPending.insert(key).second) g_itmQueue.emplace_back(path, key);
    if (g_itmWorkerRunning) return true;
    HMODULE module = nullptr;  // the worker keeps the DLL loaded until it is done
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ItmWorker), &module);
    HANDLE t = CreateThread(nullptr, 0, &ItmWorker, module, 0, nullptr);
    if (!t) {
        FreeLibrary(module);
        g_itmPending.erase(key);
        g_itmQueue.pop_back();
        return false;
    }
    CloseHandle(t);
    g_itmWorkerRunning = true;
    return true;
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
                std::wstring itmKey = ItmKey(path_, header_.masters);
                if (itmKey.empty()) return InitPropVariantFromString(L"MissingMaster", pv);
                ItmSummary s;
                bool cached = false;
                {
                    std::lock_guard<std::mutex> c(g_cacheLock);
                    auto it = g_itmCache.find(itmKey);
                    if (it != g_itmCache.end()) {
                        s = it->second;
                        cached = true;
                    }
                }
                if (!cached) {
                    if (InExplorer() && QueueItm(path_, itmKey)) return InitPropVariantFromString(L"Working", pv);
                    s = ComputeItm(path_);
                    StoreItm(itmKey, s);
                }
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
                // Blank rather than 0, so plugins that need cleaning stand out.
                const ScanSummary& s = Scan();
                return s.ok && s.deleted ? InitPropVariantFromUInt32(static_cast<ULONG>(s.deleted), pv) : S_OK;
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

    const ScanSummary& Scan() {
        std::lock_guard<std::mutex> g(lock_);
        if (scanned_) return summary_;
        scanned_ = true;
        if (!std::get<0>(key_).empty()) {
            std::lock_guard<std::mutex> c(g_cacheLock);
            auto it = g_cache.find(key_);
            if (it != g_cache.end()) {
                summary_ = it->second;
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
        if (scan.ok && !std::get<0>(key_).empty()) {
            std::lock_guard<std::mutex> c(g_cacheLock);
            if (g_cache.size() >= kCacheLimit) g_cache.clear();
            g_cache[key_] = summary_;
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
