#include "asset_property_handler.h"

#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl.h>

// Defines the PKEY_ values in this file, so it comes after the headers above.
#include <initguid.h>

#include <propkey.h>

#include <asset/metadata.hpp>

#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "context_menu.h"
#include "property_handler.h"
#include "registration.h"

namespace pse {
namespace {

// Enough for almost every header; a long lip block or XWM packet table gets one
// retry with the bigger prefix, the same as the library's own probe_audio.
constexpr size_t kBigProbe = size_t{4} << 20;

bool ReadPrefix(IStream* stream, size_t want, std::vector<uint8_t>* out) {
    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    out->resize(want);
    size_t have = 0;
    while (have < want) {
        ULONG got = 0;
        HRESULT hr = stream->Read(out->data() + have, static_cast<ULONG>(want - have), &got);
        if (FAILED(hr)) return false;
        have += got;
        if (hr == S_FALSE || got == 0) break;
    }
    out->resize(have);
    return true;
}

uint32_t FullMipCount(const asset::DdsInfo& d) {
    uint32_t largest = d.width > d.height ? d.width : d.height;
    if (d.volume && d.depth > largest) largest = d.depth;
    uint32_t levels = 1;
    while (largest > 1) {
        largest >>= 1;
        ++levels;
    }
    return levels;
}

// The handler to pass values through to: the one recorded when ours was
// registered, else Windows' own.
CLSID WrappedHandlerClsid() {
    wchar_t text[64] = {};
    DWORD size = sizeof(text);
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kDdsPropertyHandlerClsidString;
    CLSID clsid;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), kWrappedHandlerValue, RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS &&
        SUCCEEDED(CLSIDFromString(text, &clsid)) && !IsEqualCLSID(clsid, kClsidDdsPropertyHandler))
        return clsid;
    return kClsidWindowsImageHandler;
}

class AssetHandler final : public IInitializeWithStream, public IPropertyStore, public IPropertyStoreCapabilities {
public:
    explicit AssetHandler(bool dds) : dds_(dds) { InterlockedIncrement(&g_dllRefs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) {
            *ppv = static_cast<IInitializeWithStream*>(this);
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

    // IInitializeWithStream. A file the headers don't make sense of simply has
    // no properties: Explorer shows blanks, as for any file it can't read.
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override {
        if (!stream) return E_INVALIDARG;
        std::lock_guard<std::mutex> g(lock_);
        if (initialized_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        initialized_ = true;
        try {
            if (dds_) LoadDds(stream, mode);
            else Load(stream);
        } catch (const std::bad_alloc&) {
            Clear();
        }
        return S_OK;
    }

    IFACEMETHODIMP GetCount(DWORD* count) override {
        if (!count) return E_POINTER;
        std::lock_guard<std::mutex> g(lock_);
        DWORD inner = 0;
        if (inner_ && FAILED(inner_->GetCount(&inner))) inner = 0;
        *count = static_cast<DWORD>(values_.size()) + inner;
        return S_OK;
    }
    IFACEMETHODIMP GetAt(DWORD index, PROPERTYKEY* key) override {
        if (!key) return E_POINTER;
        std::lock_guard<std::mutex> g(lock_);
        if (index < values_.size()) {
            *key = values_[index].key;
            return S_OK;
        }
        if (inner_) return inner_->GetAt(index - static_cast<DWORD>(values_.size()), key);
        return E_INVALIDARG;
    }
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        if (!pv) return E_POINTER;
        PropVariantInit(pv);
        std::lock_guard<std::mutex> g(lock_);
        for (const auto& v : values_)
            if (IsEqualPropertyKey(v.key, key)) return PropVariantCopy(pv, &v.value);
        if (inner_) return inner_->GetValue(key, pv);
        return S_OK;
    }
    IFACEMETHODIMP SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP Commit() override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY) override { return S_FALSE; }

private:
    struct Entry {
        PROPERTYKEY key;
        PROPVARIANT value;
    };

    ~AssetHandler() {
        Clear();
        if (inner_) inner_->Release();
        InterlockedDecrement(&g_dllRefs);
    }

    void Clear() {
        for (auto& v : values_) PropVariantClear(&v.value);
        values_.clear();
    }

    void AddUInt32(REFPROPERTYKEY key, uint32_t value) {
        Entry e{key, {}};
        if (SUCCEEDED(InitPropVariantFromUInt32(value, &e.value))) values_.push_back(e);
    }
    void AddUInt64(REFPROPERTYKEY key, uint64_t value) {
        Entry e{key, {}};
        if (SUCCEEDED(InitPropVariantFromUInt64(value, &e.value))) values_.push_back(e);
    }

    void AddText(DWORD pid, const std::wstring& text) {
        Entry e{PROPERTYKEY{kPluginFmtid, pid}, {}};
        if (SUCCEEDED(InitPropVariantFromString(text.c_str(), &e.value))) values_.push_back(e);
    }

    void AddImage(uint32_t width, uint32_t height, uint32_t bitDepth) {
        AddUInt32(PKEY_Image_HorizontalSize, width);
        AddUInt32(PKEY_Image_VerticalSize, height);
        const std::wstring size = std::to_wstring(width) + L" x " + std::to_wstring(height);
        Entry e{PKEY_Image_Dimensions, {}};
        if (SUCCEEDED(InitPropVariantFromString(size.c_str(), &e.value))) values_.push_back(e);
        if (bitDepth) AddUInt32(PKEY_Image_BitDepth, bitDepth);
    }

    void AddAudio(const asset::AudioInfo& a, const std::vector<uint8_t>& bytes) {
        if (a.durationSeconds > 0) AddUInt64(PKEY_Media_Duration, static_cast<uint64_t>(a.durationSeconds * 10000000.0 + 0.5));
        if (a.bitRate) AddUInt32(PKEY_Audio_EncodingBitrate, a.bitRate);
        if (a.sampleRate) AddUInt32(PKEY_Audio_SampleRate, a.sampleRate);
        if (a.channels) AddUInt32(PKEY_Audio_ChannelCount, a.channels);
        if (a.container != asset::AudioContainer::Fuz) return;
        // Our own columns as well: Explorer doesn't offer Windows' sample rate
        // and channel count in its list of columns.
        if (a.sampleRate) AddUInt32(PROPERTYKEY{kPluginFmtid, kPidSampleRate}, a.sampleRate);
        if (a.channels) AddUInt32(PROPERTYKEY{kPluginFmtid, kPidChannels}, a.channels);
        AddUInt32(PROPERTYKEY{kPluginFmtid, kPidFuzVersion}, a.fuzVersion);
        AddText(kPidHasLip, a.hasLip ? L"Yes" : L"No");
        // The lip data sits right after the 12-byte FUZE header.
        asset::LipInfo lip;
        if (a.hasLip && a.lipBytes >= 16 && 12 + uint64_t{a.lipBytes} <= bytes.size() &&
            asset::read_lip(bytes.data() + 12, a.lipBytes, lip) == asset::Status::Ok) {
            AddUInt32(PROPERTYKEY{kPluginFmtid, kPidLipVersion}, lip.version);
            AddUInt32(PROPERTYKEY{kPluginFmtid, kPidLipUnits}, lip.units);
        }
        const wchar_t* format = a.payload == asset::AudioContainer::Xwm ? L"xWMA" : a.payload == asset::AudioContainer::Wav ? L"WAV" : nullptr;
        if (format) {
            Entry e{PROPERTYKEY{kPluginFmtid, kPidAudioFormat}, {}};
            if (SUCCEEDED(InitPropVariantFromString(format, &e.value))) values_.push_back(e);
        }
    }

    void LoadDds(IStream* stream, DWORD mode) {
        std::vector<uint8_t> buf;
        asset::DdsInfo d;
        const bool ok = ReadPrefix(stream, asset::kDdsProbeBytes, &buf) && asset::read_dds(buf.data(), buf.size(), d) == asset::Status::Ok;
        if (ok) {
            const uint32_t full = FullMipCount(d);
            const std::wstring text = d.mipmaps <= full ? std::to_wstring(d.mipmaps) + L" of " + std::to_wstring(full) : std::to_wstring(d.mipmaps);
            AddText(kPidMipmaps, text);
            const std::string format = d.format == asset::DdsFormat::Other || d.format == asset::DdsFormat::Unknown ? std::string() : asset::to_string(d.format);
            if (!format.empty()) AddText(kPidTexFormat, std::wstring(format.begin(), format.end()));
            AddText(kPidTexSrgb, d.srgb ? L"Yes" : L"No");
            AddText(kPidTexAlpha, d.hasAlpha ? L"Yes" : L"No");
            AddText(kPidTexCubemap, d.cubemap ? L"Yes" : L"No");
        }
        // Windows' handler gets the stream from the start, as it would have directly.
        LARGE_INTEGER zero{};
        IInitializeWithStream* init = nullptr;
        if (SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) &&
            SUCCEEDED(CoCreateInstance(WrappedHandlerClsid(), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&init)))) {
            if (SUCCEEDED(init->Initialize(stream, mode))) init->QueryInterface(IID_PPV_ARGS(&inner_));
            init->Release();
        }
        // Without it, at least the size is still shown.
        if (!inner_ && ok) AddImage(d.width, d.height, d.bitsPerPixel);
    }

    void Load(IStream* stream) {
        std::vector<uint8_t> buf;
        if (!ReadPrefix(stream, asset::kAudioProbeBytes, &buf) || buf.empty()) return;
        // detect() first: the Creation Kit writes DDS data under .tga names.
        const asset::Kind kind = asset::detect(buf.data(), buf.size());
        if (kind == asset::Kind::Dds) {
            asset::DdsInfo d;
            if (asset::read_dds(buf.data(), buf.size(), d) == asset::Status::Ok) AddImage(d.width, d.height, d.bitsPerPixel);
        } else if (kind == asset::Kind::Fuz || kind == asset::Kind::Xwm || kind == asset::Kind::Wav) {
            asset::AudioInfo a;
            asset::Status s = asset::read_audio(buf.data(), buf.size(), a);
            const bool partial = s == asset::Status::Truncated || (s == asset::Status::Ok && (a.dpdsTruncated || !a.dataChunkFound));
            if (partial && buf.size() >= asset::kAudioProbeBytes && ReadPrefix(stream, kBigProbe, &buf)) {
                a = asset::AudioInfo();
                s = asset::read_audio(buf.data(), buf.size(), a);
            }
            if (s == asset::Status::Ok) AddAudio(a, buf);
        } else if (kind == asset::Kind::Unknown) {
            asset::TgaInfo t;  // TGA has no magic, so it's whatever detect() couldn't name
            if (asset::read_tga(buf.data(), buf.size(), t) == asset::Status::Ok) AddImage(t.width, t.height, t.bitsPerPixel);
        }
    }

    long refs_ = 1;
    const bool dds_;
    std::mutex lock_;
    bool initialized_ = false;
    std::vector<Entry> values_;
    IPropertyStore* inner_ = nullptr;  // .dds: the handler being wrapped
};

class Factory final : public IClassFactory {
public:
    explicit Factory(bool dds) : dds_(dds) { InterlockedIncrement(&g_dllRefs); }
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
        auto* obj = new (std::nothrow) AssetHandler(dds_);
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
    const bool dds_;
};

HRESULT MakeFactory(bool dds, REFIID riid, void** ppv) {
    auto* factory = new (std::nothrow) Factory(dds);
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

}  // namespace

HRESULT CreateAssetPropertyHandlerFactory(REFIID riid, void** ppv) { return MakeFactory(false, riid, ppv); }
HRESULT CreateDdsPropertyHandlerFactory(REFIID riid, void** ppv) { return MakeFactory(true, riid, ppv); }

}  // namespace pse
