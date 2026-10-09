// plugin.cpp - ASAR format handler plugin for 7-Zip.
//
// Drop the built DLL into 7-Zip's "Formats" (or "Codecs") subfolder. 7-Zip
// discovers it via the CreateObject / GetNumberOfFormats /
// GetHandlerProperty2 exports defined at the bottom of this file, and then
// uses IInArchive to browse/extract and IOutArchive to create/update .asar
// archives.
//
// Format GUID follows the 7-Zip handler scheme
//   {23170F69-40C1-278A-1000-000110FA0000}
// where 0xFA is a free format ID (see vendor/7zip/CPP/7zip/Guid.txt).

#define NOMINMAX
#include <windows.h>

#define INITGUID
#include "CPP/7zip/Archive/IArchive.h"

#include <new>
#include <oleauto.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "asar/asar.h"

// {23170F69-40C1-278A-1000-000110FA0000}
static const GUID kFormatClassId = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0xFA, 0x00, 0x00}};

namespace {

constexpr UInt32 kMaxHeaderLen = 256u * 1024 * 1024;

// Minimal COM smart pointer (the SDK's CMyComPtr lives in a header we do not
// vendor on purpose).
template <class T>
class TComPtr {
public:
    TComPtr() = default;
    ~TComPtr() { Release(); }
    TComPtr(const TComPtr&) = delete;
    TComPtr& operator=(const TComPtr&) = delete;

    HRESULT QueryInterface(IUnknown* obj, REFIID iid) {
        Release();
        if (!obj) return E_POINTER;
        HRESULT hr = obj->QueryInterface(iid, reinterpret_cast<void**>(&m_p));
        return hr;
    }
    void Release() {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }
    void Attach(T* p) {  // takes over a reference the caller owns
        Release();
        m_p = p;
    }
    explicit operator bool() const { return m_p != nullptr; }
    T* operator->() { return m_p; }
    T* Get() { return m_p; }

private:
    T* m_p = nullptr;
};

// ---------------------------------------------------------------------------
// PROPVARIANT helpers
// ---------------------------------------------------------------------------

void PropSetBool(PROPVARIANT* p, bool v) {
    p->vt = VT_BOOL;
    p->boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
}

void PropSetU64(PROPVARIANT* p, uint64_t v) {
    p->vt = VT_UI8;
    p->uhVal.QuadPart = v;
}

void PropSetUi4(PROPVARIANT* p, uint32_t v) {
    p->vt = VT_UI4;
    p->ulVal = v;
}

void PropSetFileTime(PROPVARIANT* p, const FILETIME& ft) {
    p->vt = VT_FILETIME;
    p->filetime = ft;
}

void PropSetBstr(PROPVARIANT* p, const wchar_t* s) {
    p->vt = VT_BSTR;
    p->bstrVal = SysAllocString(s);
}

void PropSetBstrBytes(PROPVARIANT* p, const char* bytes, uint32_t len) {
    p->vt = VT_BSTR;
    p->bstrVal = SysAllocStringByteLen(bytes, len);
}

bool PropToWstring(const PROPVARIANT& v, std::wstring& out) {
    if (v.vt == VT_BSTR && v.bstrVal) {
        out.assign(v.bstrVal, SysStringLen(v.bstrVal));
        return true;
    }
    return false;
}

bool PropToU64(const PROPVARIANT& v, uint64_t& out) {
    switch (v.vt) {
        case VT_UI8: out = v.uhVal.QuadPart; return true;
        case VT_UI4: out = v.ulVal; return true;
        case VT_UI2: out = v.uiVal; return true;
        case VT_UI1: out = v.bVal; return true;
        case VT_I4: out = static_cast<uint64_t>(static_cast<int64_t>(v.lVal)); return true;
        case VT_I8: out = static_cast<uint64_t>(v.hVal.QuadPart); return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// Stream helpers
// ---------------------------------------------------------------------------

HRESULT ReadStreamFull(IInStream* s, void* buf, UInt32 want, UInt32* got) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    UInt32 done = 0;
    while (done < want) {
        UInt32 processed = 0;
        HRESULT hr = s->Read(p + done, want - done, &processed);
        if (FAILED(hr)) return hr;
        if (processed == 0) break;
        done += processed;
    }
    *got = done;
    return S_OK;
}

HRESULT SeekTo(IInStream* s, uint64_t pos) {
    UInt64 newPos = 0;
    return s->Seek(static_cast<Int64>(pos), STREAM_SEEK_SET, &newPos);
}

// In-memory sequential input stream over a byte vector (feeds update data).
class CMemInStream : public ISequentialInStream {
public:
    std::vector<uint8_t> data;
    size_t pos = 0;

    STDMETHOD(QueryInterface)(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_ISequentialInStream) {
            AddRef();
            *out = static_cast<ISequentialInStream*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++m_refs; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = --m_refs;
        if (r == 0) delete this;
        return r;
    }

    Z7_COM7F_IMF(Read(void* dataOut, UInt32 size, UInt32* processedSize)) {
        if (processedSize) *processedSize = 0;
        if (size == 0) return S_OK;
        UInt32 avail = static_cast<UInt32>(data.size() > pos ? data.size() - pos : 0);
        UInt32 take = size < avail ? size : avail;
        if (take) {
            memcpy(dataOut, data.data() + pos, take);
            pos += take;
        }
        if (processedSize) *processedSize = take;
        return S_OK;
    }

private:
    LONG m_refs = 1;
};

}  // namespace

// ---------------------------------------------------------------------------
// CAsarHandler - one COM object implementing both archive sides
// ---------------------------------------------------------------------------

class CAsarHandler final : public IInArchive, public IOutArchive {
public:
    CAsarHandler() = default;

    // ---- IUnknown ----
    STDMETHOD(QueryInterface)(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown)
            *out = static_cast<IUnknown*>(static_cast<IInArchive*>(this));
        else if (iid == IID_IInArchive)
            *out = static_cast<IInArchive*>(this);
        else if (iid == IID_IOutArchive)
            *out = static_cast<IOutArchive*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&m_refs); }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }

    // ---- IInArchive / IOutArchive methods (declared via SDK macros) ----
    Z7_IFACE_COM7_IMP_NONFINAL(IInArchive)
    Z7_IFACE_COM7_IMP_NONFINAL(IOutArchive)

private:
    LONG m_refs = 1;
    TComPtr<IInStream> m_Stream;
    asar::Header m_Header;
    std::vector<asar::Entry> m_Items;
    UInt64 m_ArcSize = 0;
    bool m_HasArcSize = false;
    FILETIME m_MTime{};
    bool m_HasMTime = false;

    void Reset();
};

void CAsarHandler::Reset() {
    m_Stream.Release();
    m_Items.clear();
    m_Header = asar::Header{};
    m_ArcSize = 0;
    m_HasArcSize = false;
    m_HasMTime = false;
}

Z7_COM7F_IMF(CAsarHandler::Open(IInStream *stream, const UInt64 * /*maxCheckStartPosition*/,
                                IArchiveOpenCallback *openCallback)) {
    try {
        Reset();
        BYTE head[8];
        UInt32 got = 0;
        if (ReadStreamFull(stream, head, 8, &got) != S_OK || got != 8 ||
            head[0] != 4 || head[1] != 0 || head[2] != 0 || head[3] != 0) {
            Reset();
            return S_FALSE;
        }
        UInt32 headerLen = UInt32(head[4]) | (UInt32(head[5]) << 8) |
                           (UInt32(head[6]) << 16) | (UInt32(head[7]) << 24);
        if (headerLen < 8 || headerLen > kMaxHeaderLen) {
            Reset();
            return S_FALSE;
        }
        std::vector<BYTE> hb(headerLen);
        if (ReadStreamFull(stream, hb.data(), headerLen, &got) != S_OK || got != headerLen) {
            Reset();
            return S_FALSE;
        }
        // Reassemble the contiguous prefix (size block + header pickle).
        std::vector<BYTE> all(8 + hb.size());
        memcpy(all.data(), head, 8);
        memcpy(all.data() + 8, hb.data(), hb.size());
        std::string json;
        try {
            json = asar::ParseHeaderBlock(all.data(), all.size());
            m_Header = asar::ParseHeaderJson(json, 8 + headerLen);
            asar::Flatten(m_Header.root, m_Items);
        } catch (const std::exception&) {
            Reset();
            return S_FALSE;
        }

        stream->AddRef();
        m_Stream.Attach(stream);  // keep for extraction / updates

        TComPtr<IStreamGetSize> getSize;
        if (m_Stream && stream->QueryInterface(IID_IStreamGetSize, (void**)&getSize) == S_OK &&
            getSize.Get()) {
            UInt64 sz = 0;
            if (getSize->GetSize(&sz) == S_OK) {
                m_ArcSize = sz;
                m_HasArcSize = true;
            }
        }
        TComPtr<IStreamGetProps2> getProps;
        if (m_Stream && stream->QueryInterface(IID_IStreamGetProps2, (void**)&getProps) == S_OK &&
            getProps.Get()) {
            CStreamFileProps props{};
            if (getProps->GetProps2(&props) == S_OK) {
                m_MTime = props.MTime;
                m_HasMTime = true;
            }
        }
        if (openCallback) {
            UInt64 files = 1;
            UInt64 bytes = 8 + headerLen;
            openCallback->SetTotal(&files, &bytes);
            openCallback->SetCompleted(&files, &bytes);
        }
        return S_OK;
    } catch (const std::bad_alloc&) {
        Reset();
        return E_OUTOFMEMORY;
    } catch (...) {
        Reset();
        return E_FAIL;
    }
}

Z7_COM7F_IMF(CAsarHandler::Close()) {
    Reset();
    return S_OK;
}

Z7_COM7F_IMF(CAsarHandler::GetNumberOfItems(UInt32 *numItems)) {
    *numItems = static_cast<UInt32>(m_Items.size());
    return S_OK;
}

Z7_COM7F_IMF(CAsarHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value)) {
    if (index >= m_Items.size()) return E_INVALIDARG;
    const asar::Entry& e = m_Items[index];
    switch (propID) {
        case kpidPath:
            PropSetBstr(value, e.path.c_str());
            break;
        case kpidIsDir:
            PropSetBool(value, e.isDir);
            break;
        case kpidSize:
            PropSetU64(value, e.size);
            break;
        case kpidPackSize:
            PropSetU64(value, e.size);
            break;
        case kpidMTime:
            if (m_HasMTime) PropSetFileTime(value, m_MTime);
            break;
        case kpidSymLink:
            if (e.isLink) PropSetBstr(value, e.link.c_str());
            break;
        default:
            break;
    }
    return S_OK;
}

Z7_COM7F_IMF(CAsarHandler::Extract(const UInt32 *indices, UInt32 numItems, Int32 testMode,
                                   IArchiveExtractCallback *extractCallback)) {
    if (!extractCallback || !m_Stream) return E_FAIL;

    try {
        std::vector<UInt32> list;
        if (indices) {
            list.assign(indices, indices + numItems);
        } else {
            list.resize(m_Items.size());
            for (UInt32 i = 0; i < list.size(); ++i) list[i] = i;
        }
        UInt64 total = 0;
        for (UInt32 idx : list)
            if (idx < m_Items.size() && !m_Items[idx].isDir) total += m_Items[idx].size;
        extractCallback->SetTotal(total);

        UInt64 completed = 0;
        std::vector<uint8_t> buf(1u << 20);
        for (UInt32 idx : list) {
            if (idx >= m_Items.size()) continue;
            const asar::Entry& e = m_Items[idx];

            ISequentialOutStream* outStream = nullptr;
            Int32 askMode = testMode ? NArchive::NExtract::NAskMode::kTest
                                     : NArchive::NExtract::NAskMode::kExtract;
            HRESULT hr = extractCallback->GetStream(idx, &outStream, askMode);
            if (hr == S_FALSE) continue;  // caller chose to skip this item
            if (hr != S_OK) return hr;

            Int32 opResult = NArchive::NExtract::NOperationResult::kOK;
            if (outStream && !e.isDir) {
                if (e.isLink) {
                    opResult = NArchive::NExtract::NOperationResult::kUnsupportedMethod;
                } else if (e.unpacked) {
                    // unpacked payloads live in <archive>.unpacked/ beside the
                    // file; a plugin only sees a stream and cannot resolve it.
                    opResult = NArchive::NExtract::NOperationResult::kUnavailable;
                } else {
                    uint64_t off = 0;
                    for (char c : e.offset) off = off * 10 + (c - '0');
                    if (SeekTo(m_Stream.Get(), m_Header.dataStart + off) != S_OK) {
                        opResult = NArchive::NExtract::NOperationResult::kDataError;
                    } else {
                        uint64_t remaining = e.size;
                        while (remaining > 0 &&
                               opResult == NArchive::NExtract::NOperationResult::kOK) {
                            UInt32 want = static_cast<UInt32>(
                                (uint64_t(buf.size()) < remaining) ? buf.size() : remaining);
                            UInt32 got = 0;
                            if (ReadStreamFull(m_Stream.Get(), buf.data(), want, &got) != S_OK ||
                                got != want) {
                                opResult = NArchive::NExtract::NOperationResult::kUnexpectedEnd;
                                break;
                            }
                            UInt32 written = 0;
                            if (outStream->Write(buf.data(), got, &written) != S_OK ||
                                written != got) {
                                opResult = NArchive::NExtract::NOperationResult::kDataError;
                            }
                            remaining -= got;
                            completed += got;
                            extractCallback->SetCompleted(&completed);
                        }
                    }
                }
            }
            if (outStream) outStream->Release();
            extractCallback->SetOperationResult(opResult);
        }
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

Z7_COM7F_IMF(CAsarHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value)) {
    switch (propID) {
        case kpidPhySize:
            if (m_HasArcSize) PropSetU64(value, m_ArcSize);
            break;
        case kpidExtension:
            PropSetBstr(value, L"asar");
            break;
        default:
            break;
    }
    return S_OK;
}

Z7_COM7F_IMF(CAsarHandler::GetNumberOfProperties(UInt32 *numProps)) {
    *numProps = 4;
    return S_OK;
}

Z7_COM7F_IMF(CAsarHandler::GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID,
                                           VARTYPE *varType)) {
    static const struct {
        PROPID id;
        const wchar_t* n;
        VARTYPE vt;
    } kProps[] = {
        {kpidPath, L"Path", VT_BSTR},
        {kpidIsDir, L"Folder", VT_BOOL},
        {kpidSize, L"Size", VT_UI8},
        {kpidMTime, L"MTime", VT_FILETIME},
    };
    if (index >= ARRAYSIZE(kProps)) return E_INVALIDARG;
    if (name) *name = SysAllocString(kProps[index].n);
    if (propID) *propID = kProps[index].id;
    if (varType) *varType = kProps[index].vt;
    return S_OK;
}

Z7_COM7F_IMF(CAsarHandler::GetNumberOfArchiveProperties(UInt32 *numProps)) {
    *numProps = 0;
    return S_OK;
}

Z7_COM7F_IMF(CAsarHandler::GetArchivePropertyInfo(UInt32 /*index*/, BSTR * /*name*/,
                                                  PROPID * /*propID*/, VARTYPE * /*varType*/)) {
    return E_INVALIDARG;
}

Z7_COM7F_IMF(CAsarHandler::GetFileTimeType(UInt32 *type)) {
    *type = NFileTimeType::kWindows;
    return S_OK;
}

Z7_COM7F_IMF(CAsarHandler::UpdateItems(ISequentialOutStream *outStream, UInt32 numItems,
                                       IArchiveUpdateCallback *updateCallback)) {
    if (!outStream || !updateCallback) return E_FAIL;
    try {
        uint64_t oldDataSize = 0;
        if (m_HasArcSize && m_Stream.Get() && m_ArcSize >= m_Header.dataStart)
            oldDataSize = m_ArcSize - m_Header.dataStart;
        if (!m_HasArcSize && numItems > 0) {
            // Updating an existing archive requires knowing where its data
            // section ends; without a sized stream we cannot preserve it.
            bool hasOld = false;
            for (UInt32 i = 0; i < numItems && !hasOld; ++i) {
                Int32 newData = 0, newProps = 0;
                UInt32 idx = 0;
                if (updateCallback->GetUpdateItemInfo(i, &newData, &newProps, &idx) == S_OK &&
                    newData == 0)
                    hasOld = true;
            }
            if (hasOld) return E_FAIL;
        }

        struct PlanItem {
            bool isOld = false;
            std::wstring path;
            bool isDir = false;
            bool isLink = false;
            bool unpacked = false;
            std::wstring link;
            uint64_t size = 0;
            std::string oldOffset;
            std::vector<uint8_t> data;  // new files only
        };

        std::vector<PlanItem> plan;
        plan.reserve(numItems);

        // Pass 1: ask the callback about every item of the new archive.
        for (UInt32 i = 0; i < numItems; ++i) {
            Int32 newData = 0, newProps = 0;
            UInt32 indexInArchive = 0;
            if (updateCallback->GetUpdateItemInfo(i, &newData, &newProps, &indexInArchive) != S_OK)
                return E_FAIL;
            if (newData == 0) {
                // Item carried over from the existing archive.
                if (indexInArchive >= m_Items.size()) return E_INVALIDARG;
                PlanItem pi;
                pi.isOld = true;
                const asar::Entry& e = m_Items[indexInArchive];
                pi.path = e.path;
                pi.isDir = e.isDir;
                pi.isLink = e.isLink;
                pi.unpacked = e.unpacked;
                pi.link = e.link;
                pi.size = e.size;
                pi.oldOffset = e.offset;
                if (newProps != 0) {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (updateCallback->GetProperty(i, kpidPath, &v) == S_OK) {
                        std::wstring p;
                        if (PropToWstring(v, p) && !p.empty()) pi.path = p;
                    }
                    PropVariantClear(&v);
                }
                plan.push_back(std::move(pi));
            } else {
                // New item coming from the outside world.
                PlanItem pi;
                PROPVARIANT v;
                PropVariantInit(&v);
                if (updateCallback->GetProperty(i, kpidPath, &v) != S_OK) return E_FAIL;
                const bool hasPath = PropToWstring(v, pi.path);
                PropVariantClear(&v);
                if (!hasPath || pi.path.empty()) return E_INVALIDARG;
                PropVariantInit(&v);
                if (updateCallback->GetProperty(i, kpidIsDir, &v) == S_OK && v.vt == VT_BOOL)
                    pi.isDir = (v.boolVal != VARIANT_FALSE);
                PropVariantClear(&v);
                if (pi.isDir) {
                    plan.push_back(std::move(pi));
                } else {
                    PropVariantInit(&v);
                    if (updateCallback->GetProperty(i, kpidSize, &v) == S_OK) {
                        uint64_t sz = 0;
                        if (PropToU64(v, sz)) pi.size = sz;
                    }
                    PropVariantClear(&v);

                    ISequentialInStream* inStream = nullptr;
                    HRESULT hr = updateCallback->GetStream(i, &inStream);
                    if (hr == S_FALSE || (hr == S_OK && !inStream))
                        continue;  // caller skipped the item
                    if (hr != S_OK) return hr;

                    static const UInt32 kChunk = 1u << 20;
                    UInt64 readTotal = 0;
                    for (;;) {
                        pi.data.resize(static_cast<size_t>(readTotal) + kChunk);
                        UInt32 processed = 0;
                        hr = inStream->Read(pi.data.data() + readTotal, kChunk, &processed);
                        if (hr != S_OK) {
                            inStream->Release();
                            return hr;
                        }
                        readTotal += processed;
                        if (processed == 0) break;
                        if (pi.size && readTotal > pi.size) break;  // longer than declared
                    }
                    inStream->Release();
                    if (pi.size != 0 && readTotal != pi.size) return E_FAIL;
                    pi.data.resize(static_cast<size_t>(readTotal));
                    pi.size = readTotal;
                    updateCallback->SetOperationResult(NArchive::NUpdate::NOperationResult::kOK);
                    plan.push_back(std::move(pi));
                }
            }
        }

        // Pass 2: rebuild the header tree. Old entries keep their offsets, new
        // payloads are appended after the existing data section.
        asar::Node newRoot;
        newRoot.isDir = true;
        uint64_t cursor = oldDataSize;
        std::vector<PlanItem*> newFileOrder;
        for (PlanItem& pi : plan) {
            std::vector<std::wstring> comps = asar::SplitArchivePath(pi.path);
            asar::Node node;
            if (pi.isOld) {
                node.isDir = pi.isDir;
                node.isLink = pi.isLink;
                node.unpacked = pi.unpacked;
                node.link = pi.link;
                node.size = pi.size;
                node.offset = pi.oldOffset;
            } else if (pi.isDir) {
                node.isDir = true;
            } else {
                node.size = pi.size;
                node.offset = std::to_string(cursor);
                cursor += pi.size;
                node.integrity = asar::ComputeIntegrity(pi.data.data(), pi.data.size());
                newFileOrder.push_back(&pi);
            }
            asar::InsertNode(newRoot, comps, std::move(node));
        }

        std::string json = asar::SerializeHeaderJson(newRoot);
        std::vector<uint8_t> block = asar::BuildHeaderBlock(json);

        // Pass 3: write header, then old data, then appended payloads.
        if (outStream->Write(block.data(), static_cast<UInt32>(block.size()), nullptr) != S_OK)
            return E_FAIL;
        if (oldDataSize && m_Stream.Get()) {
            if (SeekTo(m_Stream.Get(), m_Header.dataStart) != S_OK) return E_FAIL;
            uint64_t remaining = oldDataSize;
            std::vector<uint8_t> buf(1u << 20);
            while (remaining > 0) {
                UInt32 want = static_cast<UInt32>(
                    (uint64_t(buf.size()) < remaining) ? buf.size() : remaining);
                UInt32 got = 0;
                if (ReadStreamFull(m_Stream.Get(), buf.data(), want, &got) != S_OK || got != want)
                    return E_FAIL;
                if (outStream->Write(buf.data(), got, nullptr) != S_OK) return E_FAIL;
                remaining -= got;
            }
        }
        for (PlanItem* pi : newFileOrder) {
            if (pi->data.empty()) continue;
            if (outStream->Write(pi->data.data(), static_cast<UInt32>(pi->data.size()),
                                 nullptr) != S_OK)
                return E_FAIL;
        }

        UInt64 done = 1;
        updateCallback->SetCompleted(&done);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (const std::exception&) {
        return E_FAIL;
    }
}

// ---------------------------------------------------------------------------
// Plugin exports (signatures: vendor/7zip/CPP/7zip/Archive/IArchive.h)
// ---------------------------------------------------------------------------

namespace {

HRESULT GetFormatProperty(PROPID propID, PROPVARIANT* value) {
    switch (propID) {
        case NArchive::NHandlerPropID::kName:
            PropSetBstr(value, L"Asar");
            break;
        case NArchive::NHandlerPropID::kExtension:
            PropSetBstr(value, L"asar");
            break;
        case NArchive::NHandlerPropID::kAddExtension:
            PropSetBstr(value, L"asar");
            break;
        case NArchive::NHandlerPropID::kUpdate:
            PropSetBool(value, true);
            break;
        case NArchive::NHandlerPropID::kKeepName:
            PropSetBool(value, false);
            break;
        case NArchive::NHandlerPropID::kClassID:
            PropSetBstrBytes(value, reinterpret_cast<const char*>(&kFormatClassId),
                             sizeof(GUID));
            break;
        case NArchive::NHandlerPropID::kSignature:
            PropSetBstrBytes(value, "\x04\x00\x00\x00", 4);
            break;
        case NArchive::NHandlerPropID::kSignatureOffset:
            PropSetUi4(value, 0);
            break;
        default:
            break;
    }
    return S_OK;
}

}  // namespace

extern "C" {

HRESULT WINAPI CreateObject(const GUID* clsID, const GUID* iid,
                                                  void** outObject) {
    if (!clsID || !iid || !outObject) return E_POINTER;
    *outObject = nullptr;
    if (*clsID != kFormatClassId) return CLASS_E_CLASSNOTAVAILABLE;
    if (*iid != IID_IInArchive && *iid != IID_IOutArchive) return E_NOINTERFACE;
    CAsarHandler* handler = new (std::nothrow) CAsarHandler();
    if (!handler) return E_OUTOFMEMORY;
    const HRESULT hr = handler->QueryInterface(*iid, outObject);
    handler->Release();
    return hr;
}

HRESULT WINAPI GetNumberOfFormats(UInt32* numFormats) {
    if (numFormats) *numFormats = 1;
    return S_OK;
}

HRESULT WINAPI GetHandlerProperty2(UInt32 index, PROPID propID,
                                                         PROPVARIANT* value) {
    if (index != 0) return E_INVALIDARG;
    return GetFormatProperty(propID, value);
}

HRESULT WINAPI GetHandlerProperty(PROPID propID, PROPVARIANT* value) {
    return GetFormatProperty(propID, value);
}

}  // extern "C"
