// Explorer property handler for .nif, .pex, .psc, .bsa, .hkx and .lip. The
// quick facts are read when the file is opened. The ones that need the whole
// file come from the saved results, or are worked out on the shared background
// workers: text columns say "Working" until then, number columns stay blank.
#include "file_property_handler.h"

#include <propsys.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "asset_facts.h"
#include "context_menu.h"
#include "disk_cache.h"
#include "property_handler.h"
#include "work_budget.h"

namespace pse {
namespace {

constexpr size_t kMemoryLimit = 50000;
constexpr wchar_t kWorking[] = L"Working";

std::mutex g_lock;
std::map<std::wstring, std::shared_ptr<const std::vector<AssetFact>>> g_results;
std::multimap<uint64_t, std::pair<std::wstring, std::wstring>> g_queue;  // size -> (path, key)
std::set<std::wstring> g_pending;
int g_workers = 0;

bool InExplorer() {
    static const bool explorer = [] {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* base = wcsrchr(exe, L'\\');
        return _wcsicmp(base ? base + 1 : exe, L"explorer.exe") == 0 || GetEnvironmentVariableW(L"PSE_ASYNC_TEST", nullptr, 0) > 0;
    }();
    return explorer;
}

std::shared_ptr<const std::vector<AssetFact>> Remember(const std::wstring& key, std::vector<AssetFact> facts) {
    auto shared = std::make_shared<const std::vector<AssetFact>>(std::move(facts));
    std::lock_guard<std::mutex> g(g_lock);
    if (g_results.size() >= kMemoryLimit) g_results.clear();
    g_results[key] = shared;
    return shared;
}

std::shared_ptr<const std::vector<AssetFact>> Cached(const std::wstring& key) {
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto it = g_results.find(key);
        if (it != g_results.end()) return it->second;
    }
    std::wstring value;
    std::vector<AssetFact> facts;
    if (!DiskCacheGet(key, &value) || !DecodeAssetFacts(value, &facts)) return nullptr;
    return Remember(key, std::move(facts));
}

void Compute(const std::wstring& path, const std::wstring& key) {
    std::vector<AssetFact> facts = ReadFullAssetFacts(path);
    DiskCachePut(key, EncodeAssetFacts(facts));
    Remember(key, std::move(facts));
}

DWORD WINAPI Worker(void* module) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    Sleep(250);  // Explorer asks about a whole folder in one burst
    for (;;) {
        std::pair<std::wstring, std::wstring> job;
        {
            std::lock_guard<std::mutex> g(g_lock);
            if (g_queue.empty() || g_workers > WorkerThreadCount()) {
                --g_workers;
                break;
            }
            job = std::move(g_queue.begin()->second);  // the smallest file
            g_queue.erase(g_queue.begin());
        }
        {
            WorkSlot slot(WorkKind::Background);
            Compute(job.first, job.second);
            slot.Pace();
        }
        {
            std::lock_guard<std::mutex> g(g_lock);
            g_pending.erase(job.second);
        }
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, job.first.c_str(), nullptr);
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

void Queue(const std::wstring& path, const std::wstring& key, uint64_t size) {
    const int wanted = WorkerThreadCount();
    std::lock_guard<std::mutex> g(g_lock);
    if (g_pending.insert(key).second) g_queue.emplace(size, std::make_pair(path, key));
    while (g_workers < wanted && g_workers < static_cast<int>(g_queue.size())) {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Worker), &module);
        HANDLE t = CreateThread(nullptr, 0, &Worker, module, 0, nullptr);
        if (!t) {
            FreeLibrary(module);
            break;
        }
        CloseHandle(t);
        ++g_workers;
    }
}

PROPERTYKEY KeyOf(DWORD pid) { return pid == kFactDuration ? PKEY_Media_Duration : PROPERTYKEY{kPluginFmtid, pid}; }

HRESULT ToVariant(const AssetFact& fact, PROPVARIANT* pv) {
    switch (fact.type) {
        case AssetFact::Type::Text:
            return fact.text.empty() ? S_OK : InitPropVariantFromString(fact.text.c_str(), pv);
        case AssetFact::Type::UInt32:
            return InitPropVariantFromUInt32(static_cast<ULONG>(fact.number), pv);
        case AssetFact::Type::UInt64:
            return InitPropVariantFromUInt64(fact.number, pv);
        case AssetFact::Type::Date: {
            FILETIME time{static_cast<DWORD>(fact.number), static_cast<DWORD>(fact.number >> 32)};
            return InitPropVariantFromFileTime(&time, pv);
        }
    }
    return S_OK;
}

class Handler final : public IInitializeWithFile, public IPropertyStore, public IPropertyStoreCapabilities {
public:
    Handler() { InterlockedIncrement(&g_dllRefs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithFile) *ppv = static_cast<IInitializeWithFile*>(this);
        else if (riid == IID_IPropertyStore) *ppv = static_cast<IPropertyStore*>(this);
        else if (riid == IID_IPropertyStoreCapabilities) *ppv = static_cast<IPropertyStoreCapabilities*>(this);
        else {
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

    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD) override {
        if (!path) return E_INVALIDARG;
        std::lock_guard<std::mutex> g(lock_);
        if (!path_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        try {
            path_ = path;
            if (!HasAssetFacts(path_)) return S_OK;
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) return S_OK;
            size_ = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            std::wstring lower = path_;
            CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
            const uint64_t time = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
            key_ = L"A|" + lower + L"|" + std::to_wstring(size_) + L":" + std::to_wstring(time);
            quick_ = ReadQuickAssetFacts(path_);
            columns_ = AssetFactColumns(path_);
            for (const AssetFact& f : quick_) AddKey(f.pid);
            for (const AssetFact& f : columns_) AddKey(f.pid);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    IFACEMETHODIMP GetCount(DWORD* count) override {
        if (!count) return E_POINTER;
        std::lock_guard<std::mutex> g(lock_);
        *count = static_cast<DWORD>(keys_.size());
        return S_OK;
    }
    IFACEMETHODIMP GetAt(DWORD index, PROPERTYKEY* key) override {
        if (!key) return E_POINTER;
        std::lock_guard<std::mutex> g(lock_);
        if (index >= keys_.size()) return E_INVALIDARG;
        *key = KeyOf(keys_[index]);
        return S_OK;
    }
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        if (!pv) return E_POINTER;
        PropVariantInit(pv);
        DWORD pid = 0;
        if (IsEqualPropertyKey(key, PKEY_Media_Duration)) pid = kFactDuration;
        else if (key.fmtid == kPluginFmtid) pid = key.pid;
        else return S_OK;
        std::lock_guard<std::mutex> g(lock_);
        try {
            const AssetFact* column = nullptr;
            for (const AssetFact& f : columns_) {
                if (f.pid == pid) column = &f;
            }
            if (column || !quick_.empty()) {
                if (!full_ && column) Load();
                if (full_) {
                    for (const AssetFact& f : *full_) {
                        if (f.pid == pid) return ToVariant(f, pv);
                    }
                }
            }
            for (const AssetFact& f : quick_) {
                if (f.pid == pid) return ToVariant(f, pv);
            }
            if (column && !full_ && column->type == AssetFact::Type::Text) return InitPropVariantFromString(kWorking, pv);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }
    IFACEMETHODIMP SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP Commit() override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY) override { return S_FALSE; }

private:
    ~Handler() { InterlockedDecrement(&g_dllRefs); }

    void AddKey(DWORD pid) {
        for (DWORD known : keys_) {
            if (known == pid) return;
        }
        keys_.push_back(pid);
    }

    void Load() {
        if (key_.empty()) return;
        full_ = Cached(key_);
        if (full_) return;
        if (InExplorer()) {
            Queue(path_, key_, size_);
        } else {
            Compute(path_, key_);
            full_ = Cached(key_);
        }
    }

    long refs_ = 1;
    std::mutex lock_;
    std::wstring path_, key_;
    uint64_t size_ = 0;
    std::vector<AssetFact> quick_, columns_;
    std::vector<DWORD> keys_;
    std::shared_ptr<const std::vector<AssetFact>> full_;
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
        auto* obj = new (std::nothrow) Handler();
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

HRESULT CreateFilePropertyHandlerFactory(REFIID riid, void** ppv) {
    auto* factory = new (std::nothrow) Factory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

}  // namespace pse
