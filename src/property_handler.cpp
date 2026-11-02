#include "property_handler.h"

#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl.h>

#include <map>
#include <mutex>
#include <new>
#include <string>
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

// The ITM check is slower still (it reads the masters), so it has its own
// cache and runs only when the #ITM value is asked for.
struct ItmSummary {
    bool ok = false;
    bool missingMaster = false;
    size_t count = 0;
};
std::map<ScanKey, ItmSummary> g_itmCache;
constexpr size_t kCacheLimit = 50000;

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
                // Blank when there are none, like #Deleted.
                const ItmSummary& s = Itm();
                if (s.missingMaster) return InitPropVariantFromString(L"MissingMaster", pv);
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

    const ItmSummary& Itm() {
        std::lock_guard<std::mutex> g(lock_);
        if (itmDone_) return itm_;
        itmDone_ = true;
        if (!std::get<0>(key_).empty()) {
            std::lock_guard<std::mutex> c(g_cacheLock);
            auto it = g_itmCache.find(key_);
            if (it != g_itmCache.end()) {
                itm_ = it->second;
                return itm_;
            }
        }
        const PatternCatalog* cat = EmbeddedPatterns();
        if (!cat) return itm_;
        ItmResult r = FindItms(path_, *cat);
        itm_.ok = r.ok;
        itm_.missingMaster = !r.missingMasters.empty();
        itm_.count = r.itms.size();
        if (r.ok && !std::get<0>(key_).empty()) {
            std::lock_guard<std::mutex> c(g_cacheLock);
            if (g_itmCache.size() >= kCacheLimit) g_itmCache.clear();
            g_itmCache[key_] = itm_;
        }
        return itm_;
    }

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
    bool itmDone_ = false;
    ItmSummary itm_;
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
