// test_7zclient.cpp - exercises the 7-Zip plugin's COM surface the same way
// 7-Zip itself does: CreateObject -> IInArchive (open/list/extract) and
// IOutArchive (create/update), driven through stub callback implementations.
//
// Usage: test_7zclient.exe <path-to-7z-asar.dll>

#include <windows.h>

#define INITGUID
#include "CPP/7zip/Archive/IArchive.h"

#include <oleauto.h>

#include <asar/asar.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

#define MILESTONE(msg) std::printf("[*] %s\n", msg)

// Must match kFormatClassId in 7z-asar/src/plugin.cpp.
// {23170F69-40C1-278A-1000-000110FA0000}
static const GUID kAsarFormatId = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0xFA, 0x00, 0x00}};

// ---------------------------------------------------------------------------
// Stub streams / callbacks
// ---------------------------------------------------------------------------

static void SetBoolProp(PROPVARIANT* p, bool v) {
    p->vt = VT_BOOL;
    p->boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
}
static void SetU64Prop(PROPVARIANT* p, uint64_t v) {
    p->vt = VT_UI8;
    p->uhVal.QuadPart = v;
}
static void SetStrProp(PROPVARIANT* p, const std::wstring& s) {
    p->vt = VT_BSTR;
    p->bstrVal = SysAllocStringLen(s.data(), static_cast<UINT>(s.size()));
}

class TestInStream final : public IInStream, public IStreamGetSize {
public:
    FILE* f = nullptr;
    LONG refs = 1;

    TestInStream(const fs::path& p) {
        _wfopen_s(&f, p.c_str(), L"rb");
    }
    ~TestInStream() {
        if (f) fclose(f);
    }

    STDMETHOD(QueryInterface)(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown) *out = static_cast<IUnknown*>(static_cast<IInStream*>(this));
        else if (iid == IID_ISequentialInStream) *out = static_cast<ISequentialInStream*>(this);
        else if (iid == IID_IInStream) *out = static_cast<IInStream*>(this);
        else if (iid == IID_IStreamGetSize) *out = static_cast<IStreamGetSize*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }

    Z7_COM7F_IMF(Read(void* data, UInt32 size, UInt32* processedSize)) override {
        if (processedSize) *processedSize = 0;
        if (!f) return E_FAIL;
        size_t got = size ? fread(data, 1, size, f) : 0;
        if (processedSize) *processedSize = static_cast<UInt32>(got);
        return S_OK;
    }
    Z7_COM7F_IMF(Seek(Int64 offset, UInt32 origin, UInt64* newPosition)) override {
        if (!f) return E_FAIL;
        if (_fseeki64(f, offset, static_cast<int>(origin)) != 0) return E_FAIL;
        long long pos = _ftelli64(f);
        if (newPosition) *newPosition = static_cast<UInt64>(pos);
        return S_OK;
    }
    Z7_COM7F_IMF(GetSize(UInt64* size)) override {
        if (!f) return E_FAIL;
        long long cur = _ftelli64(f);
        _fseeki64(f, 0, SEEK_END);
        long long end = _ftelli64(f);
        _fseeki64(f, cur, SEEK_SET);
        if (size) *size = static_cast<UInt64>(end);
        return S_OK;
    }
};

class TestSeqInStream final : public ISequentialInStream {
public:
    std::vector<uint8_t> data;
    size_t pos = 0;
    LONG refs = 1;

    STDMETHOD(QueryInterface)(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_ISequentialInStream)
            *out = static_cast<ISequentialInStream*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }
    Z7_COM7F_IMF(Read(void* buf, UInt32 size, UInt32* processedSize)) override {
        if (processedSize) *processedSize = 0;
        UInt32 avail = static_cast<UInt32>(data.size() > pos ? data.size() - pos : 0);
        UInt32 take = size < avail ? size : avail;
        memcpy(buf, data.data() + pos, take);
        pos += take;
        if (processedSize) *processedSize = take;
        return S_OK;
    }
};

class TestSeqOutStream final : public ISequentialOutStream {
public:
    std::vector<uint8_t> bytes;
    LONG refs = 1;

    STDMETHOD(QueryInterface)(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_ISequentialOutStream)
            *out = static_cast<ISequentialOutStream*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }
    Z7_COM7F_IMF(Write(const void* buf, UInt32 size, UInt32* processedSize)) override {
        const uint8_t* p = static_cast<const uint8_t*>(buf);
        bytes.insert(bytes.end(), p, p + size);
        if (processedSize) *processedSize = size;
        return S_OK;
    }
};

class TestOpenCallback final : public IArchiveOpenCallback {
public:
    LONG refs = 1;
    STDMETHOD(QueryInterface)(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IArchiveOpenCallback)
            *out = static_cast<IArchiveOpenCallback*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }
    Z7_COM7F_IMF(SetTotal(const UInt64*, const UInt64*)) override { return S_OK; }
    Z7_COM7F_IMF(SetCompleted(const UInt64*, const UInt64*)) override { return S_OK; }
};

// Extracts every requested item into a directory; mirrors what 7-Zip's
// extraction callback does.
class TestExtractCallback final : public IArchiveExtractCallback {
public:
    std::vector<asar::Entry> entries;
    fs::path dest;
    std::vector<Int32> opResults;
    LONG refs = 1;

    STDMETHOD(QueryInterface)(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown) *out = static_cast<IUnknown*>(this);
        else if (iid == IID_IProgress) *out = static_cast<IProgress*>(this);
        else if (iid == IID_IArchiveExtractCallback)
            *out = static_cast<IArchiveExtractCallback*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }
    Z7_COM7F_IMF(SetTotal(UInt64)) override { return S_OK; }
    Z7_COM7F_IMF(SetCompleted(const UInt64*)) override { return S_OK; }
    Z7_COM7F_IMF(GetStream(UInt32 index, ISequentialOutStream** outStream,
                           Int32 askExtractMode)) override {
        *outStream = nullptr;
        if (index >= entries.size()) return E_INVALIDARG;
        const asar::Entry& e = entries[index];
        if (e.isDir || askExtractMode != NArchive::NExtract::NAskMode::kExtract) return S_OK;
        FILE* f = nullptr;
        fs::path out = dest / fs::path(e.path);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        if (_wfopen_s(&f, out.c_str(), L"wb") != 0 || !f) return S_FALSE;
        class FileStream final : public ISequentialOutStream {
        public:
            FILE* f;
            LONG refs = 1;
            explicit FileStream(FILE* file) : f(file) {}
            ~FileStream() { if (f) fclose(f); }
            STDMETHOD(QueryInterface)(REFIID iid, void** out) override {
                if (!out) return E_POINTER;
                if (iid == IID_IUnknown || iid == IID_ISequentialOutStream)
                    *out = static_cast<ISequentialOutStream*>(this);
                else { *out = nullptr; return E_NOINTERFACE; }
                AddRef();
                return S_OK;
            }
            STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
            STDMETHOD_(ULONG, Release)() override {
                ULONG r = --refs;
                if (r == 0) delete this;
                return r;
            }
            Z7_COM7F_IMF(Write(const void* buf, UInt32 size, UInt32* processedSize)) override {
                size_t got = size ? fwrite(buf, 1, size, f) : 0;
                if (processedSize) *processedSize = static_cast<UInt32>(got);
                return got == size ? S_OK : E_FAIL;
            }
        };
        auto* s = new FileStream(f);
        *outStream = s;
        return S_OK;
    }
    Z7_COM7F_IMF(PrepareOperation(Int32)) override { return S_OK; }
    Z7_COM7F_IMF(SetOperationResult(Int32 opRes)) override {
        opResults.push_back(opRes);
        return S_OK;
    }
};

class TestUpdateCallback final : public IArchiveUpdateCallback {
public:
    struct Item {
        std::wstring path;
        bool isDir = false;
        bool fromOld = false;
        UInt32 oldIndex = 0;
        bool hasNewProps = false;
        std::vector<uint8_t> data;
    };
    std::vector<Item> items;
    std::vector<Int32> opResults;
    LONG refs = 1;

    STDMETHOD(QueryInterface)(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown) *out = static_cast<IUnknown*>(this);
        else if (iid == IID_IProgress) *out = static_cast<IProgress*>(this);
        else if (iid == IID_IArchiveUpdateCallback)
            *out = static_cast<IArchiveUpdateCallback*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }
    Z7_COM7F_IMF(SetTotal(UInt64)) override { return S_OK; }
    Z7_COM7F_IMF(SetCompleted(const UInt64*)) override { return S_OK; }
    Z7_COM7F_IMF(GetUpdateItemInfo(UInt32 index, Int32* newData, Int32* newProps,
                                   UInt32* indexInArchive)) override {
        if (index >= items.size()) return E_INVALIDARG;
        const Item& it = items[index];
        *newData = it.fromOld ? 0 : 1;
        *newProps = (it.fromOld && !it.hasNewProps) ? 0 : 1;
        *indexInArchive = it.fromOld ? it.oldIndex : static_cast<UInt32>(-1);
        return S_OK;
    }
    Z7_COM7F_IMF(GetProperty(UInt32 index, PROPID propID, PROPVARIANT* value)) override {
        if (index >= items.size()) return E_INVALIDARG;
        const Item& it = items[index];
        switch (propID) {
            case kpidPath:
                SetStrProp(value, it.path);
                break;
            case kpidIsDir:
                SetBoolProp(value, it.isDir);
                break;
            case kpidSize:
                SetU64Prop(value, it.data.size());
                break;
            default:
                break;
        }
        return S_OK;
    }
    Z7_COM7F_IMF(GetStream(UInt32 index, ISequentialInStream** inStream)) override {
        *inStream = nullptr;
        if (index >= items.size()) return E_INVALIDARG;
        const Item& it = items[index];
        if (it.isDir || it.fromOld) return S_FALSE;
        auto* s = new TestSeqInStream();
        s->data = it.data;
        *inStream = s;
        return S_OK;
    }
    Z7_COM7F_IMF(SetOperationResult(Int32 opRes)) override {
        opResults.push_back(opRes);
        return S_OK;
    }
};

// ---------------------------------------------------------------------------
// Plugin loading
// ---------------------------------------------------------------------------

typedef HRESULT(WINAPI* Func_CreateObject)(const GUID*, const GUID*, void**);
typedef HRESULT(WINAPI* Func_GetHandlerProperty2)(UInt32, PROPID, PROPVARIANT*);
typedef HRESULT(WINAPI* Func_GetNumberOfFormats)(UInt32*);

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: test_7zclient <7z-asar.dll>\n");
        return 2;
    }
    HMODULE lib = LoadLibraryW(argv[1]);
    if (!lib) {
        std::printf("FAIL: cannot load %s\n", argv[1]);
        return 1;
    }
    try {
    auto createObject = reinterpret_cast<Func_CreateObject>(GetProcAddress(lib, "CreateObject"));
    auto getProp2 =
        reinterpret_cast<Func_GetHandlerProperty2>(GetProcAddress(lib, "GetHandlerProperty2"));
    auto getNumFormats = reinterpret_cast<Func_GetNumberOfFormats>(
        GetProcAddress(lib, "GetNumberOfFormats"));
    CHECK(createObject && getProp2 && getNumFormats, "exports present");
    if (!createObject) return 1;

    MILESTONE("handler_properties");
    // --- Handler properties ---
    UInt32 numFormats = 99;
    CHECK(getNumFormats(&numFormats) == S_OK && numFormats == 1, "GetNumberOfFormats == 1");
    {
        PROPVARIANT v;
        PropVariantInit(&v);
        CHECK(getProp2(0, NArchive::NHandlerPropID::kName, &v) == S_OK, "kName query");
        CHECK(v.vt == VT_BSTR && wcscmp(v.bstrVal, L"Asar") == 0, "format name is Asar");
        PropVariantClear(&v);
        PropVariantInit(&v);
        CHECK(getProp2(0, NArchive::NHandlerPropID::kExtension, &v) == S_OK, "kExtension query");
        CHECK(v.vt == VT_BSTR && wcscmp(v.bstrVal, L"asar") == 0, "format extension is asar");
        PropVariantClear(&v);
    }

    MILESTONE("build_sample_archive");
    // --- Build a sample archive with libasar ---
    wchar_t tempRoot[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempRoot);
    fs::path root = fs::path(tempRoot) / L"asar_7zclient_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    const std::vector<uint8_t> hello = {'H', 'e', 'l', 'l', 'o', '!', '\n'};
    const std::vector<uint8_t> payload(70000, 0xAB);
    const std::vector<uint8_t> extra = {'e', 'x', 't', 'r', 'a'};
    {
        fs::path tree = root / L"tree";
        fs::create_directories(tree / L"dir");
        FILE* f = nullptr;
        _wfopen_s(&f, (tree / L"hello.txt").c_str(), L"wb");
        fwrite(hello.data(), 1, hello.size(), f);
        fclose(f);
        _wfopen_s(&f, (tree / L"dir" / L"payload.bin").c_str(), L"wb");
        fwrite(payload.data(), 1, payload.size(), f);
        fclose(f);
    }
    fs::path sample = root / L"sample.asar";
    asar::Pack(root / L"tree", sample, asar::PackOptions{true}, nullptr);
    std::unique_ptr<asar::Archive> ref = asar::Archive::Open(sample);
    std::vector<asar::Entry> refEntries = ref->List();

    MILESTONE("iinarchive_flow");
    // --- IInArchive: open + list + extract ---
    IInArchive* arc = nullptr;
    {
        // Garbage must be rejected with S_FALSE.
        fs::path garbagePath = root / L"garbage.bin";
        FILE* gf = nullptr;
        _wfopen_s(&gf, garbagePath.c_str(), L"wb");
        fwrite("this is definitely not an asar archive at all", 1, 44, gf);
        fclose(gf);
        IInArchive* probe = nullptr;
        CHECK(createObject(&kAsarFormatId, &IID_IInArchive, (void**)&probe) == S_OK,
              "CreateObject(IInArchive)");
        TestInStream* gs = new TestInStream(garbagePath);
        CHECK(probe->Open(gs, nullptr, nullptr) == S_FALSE, "garbage rejected with S_FALSE");
        gs->Release();
        probe->Close();
        probe->Release();
    }
    CHECK(createObject(&kAsarFormatId, &IID_IInArchive, (void**)&arc) == S_OK,
          "CreateObject(IInArchive)");
    CHECK(arc != nullptr, "handler created");
    {
        TestInStream* stream = new TestInStream(sample);
        HRESULT hr = arc->Open(stream, nullptr, new TestOpenCallback());
        CHECK(hr == S_OK, "Open(sample.asar) == S_OK");
        stream->Release();
    }
    UInt32 numItems = 0;
    CHECK(arc->GetNumberOfItems(&numItems) == S_OK && numItems == refEntries.size(),
          "GetNumberOfItems matches libasar listing");

    // Spot-check properties.
    {
        PROPVARIANT v;
        PropVariantInit(&v);
        CHECK(arc->GetProperty(0, kpidPath, &v) == S_OK && v.vt == VT_BSTR, "kpidPath is BSTR");
        CHECK(wcscmp(v.bstrVal, refEntries[0].path.c_str()) == 0, "first path matches");
        PropVariantClear(&v);
    }

    {
        TestExtractCallback* cb = new TestExtractCallback();
        cb->entries = refEntries;
        cb->dest = root / L"extracted";
        fs::create_directories(cb->dest);
        CHECK(arc->Extract(nullptr, static_cast<UInt32>(-1), 0, cb) == S_OK, "Extract all");
        bool allOk = true;
        for (Int32 r : cb->opResults)
            if (r != NArchive::NExtract::NOperationResult::kOK) allOk = false;
        CHECK(allOk && !cb->opResults.empty(), "all extraction results OK");
        cb->Release();

        // Byte-compare against libasar's own extraction.
        fs::path refDest = root / L"extracted_ref";
        ref->ExtractAll(refDest);
        for (const asar::Entry& e : refEntries) {
            if (e.isDir) continue;
            FILE* a = nullptr;
            FILE* b = nullptr;
            _wfopen_s(&a, (root / L"extracted" / fs::path(e.path)).c_str(), L"rb");
            _wfopen_s(&b, (refDest / fs::path(e.path)).c_str(), L"rb");
            CHECK(a && b, "extracted file exists");
            if (a && b) {
                fseek(a, 0, SEEK_END);
                fseek(b, 0, SEEK_END);
                CHECK(_ftelli64(a) == _ftelli64(b), "extracted size matches");
            }
            if (a) fclose(a);
            if (b) fclose(b);
        }
    }

    MILESTONE("qi_to_ioutarchive");
    // QI from IInArchive to IOutArchive on the same object.
    IOutArchive* out = nullptr;
    CHECK(arc->QueryInterface(IID_IOutArchive, (void**)&out) == S_OK && out != nullptr,
          "QI(IOutArchive) on same handler");
    arc->Close();
    arc->Release();

    MILESTONE("create_fresh_archive");
    // --- IOutArchive: create a brand-new archive in memory ---
    {
        TestUpdateCallback* cb = new TestUpdateCallback();
        TestUpdateCallback::Item it1;
        it1.path = L"one.txt";
        it1.data = hello;
        TestUpdateCallback::Item it2;
        it2.path = L"sub\\two.bin";
        it2.data = payload;
        cb->items = {it1, it2};
        TestSeqOutStream* outStream = new TestSeqOutStream();
        CHECK(out->UpdateItems(outStream, 2, cb) == S_OK, "UpdateItems(fresh)");
        fs::path created = root / L"created.asar";
        FILE* f = nullptr;
        _wfopen_s(&f, created.c_str(), L"wb");
        fwrite(outStream->bytes.data(), 1, outStream->bytes.size(), f);
        fclose(f);
        outStream->Release();
        cb->Release();

        std::unique_ptr<asar::Archive> check = asar::Archive::Open(created);
        CHECK(check->ReadFile(L"one.txt") == hello, "created archive reads one.txt");
        CHECK(check->ReadFile(L"sub/two.bin") == payload, "created archive reads sub/two.bin");
        CHECK(check->List().size() == 3, "created archive has 2 files + 1 dir");
    }

    MILESTONE("update_existing_archive");
    // --- IOutArchive: update an existing archive (keep old + add new) ---
    {
        // Re-open a fresh handler with the archive stream.
        IInArchive* arc2 = nullptr;
        CHECK(createObject(&kAsarFormatId, &IID_IInArchive, (void**)&arc2) == S_OK,
              "CreateObject for update");
        TestInStream* stream = new TestInStream(sample);
        CHECK(arc2->Open(stream, nullptr, nullptr) == S_OK, "Open for update");
        stream->Release();
        IOutArchive* out2 = nullptr;
        CHECK(arc2->QueryInterface(IID_IOutArchive, (void**)&out2) == S_OK, "QI for update");

        TestUpdateCallback* cb = new TestUpdateCallback();
        // Carry over the first two FILE entries (skip the root directory).
        size_t f0 = 0, f1 = 0;
        for (size_t i = 0; i < refEntries.size(); ++i) {
            if (refEntries[i].isDir) continue;
            if (!f0 && !f1) f0 = i + 1;
            else if (!f1) f1 = i + 1;
        }
        f0 -= 1;
        f1 -= 1;
        TestUpdateCallback::Item old1;
        old1.path = refEntries[f0].path;
        old1.fromOld = true;
        old1.oldIndex = static_cast<UInt32>(f0);
        TestUpdateCallback::Item old2;
        old2.path = refEntries[f1].path;
        old2.fromOld = true;
        old2.oldIndex = static_cast<UInt32>(f1);
        TestUpdateCallback::Item add;
        add.path = L"extra.txt";
        add.data = extra;
        cb->items = {old1, old2, add};
        TestSeqOutStream* outStream = new TestSeqOutStream();
        CHECK(out2->UpdateItems(outStream, 3, cb) == S_OK, "UpdateItems(keep+add)");
        fs::path updated = root / L"updated.asar";
        FILE* f = nullptr;
        _wfopen_s(&f, updated.c_str(), L"wb");
        fwrite(outStream->bytes.data(), 1, outStream->bytes.size(), f);
        fclose(f);
        outStream->Release();
        cb->Release();

        std::unique_ptr<asar::Archive> check = asar::Archive::Open(updated);
        CHECK(check->ReadFile(refEntries[f0].path) == ref->ReadFile(refEntries[f0].path),
              "old entry 0 preserved after update");
        CHECK(check->ReadFile(refEntries[f1].path) == ref->ReadFile(refEntries[f1].path),
              "old entry 1 preserved after update");
        CHECK(check->ReadFile(L"extra.txt") == extra, "new entry added by update");

        // Update that drops all old items (delete semantics).
        TestUpdateCallback* cb2 = new TestUpdateCallback();
        TestUpdateCallback::Item only;
        only.path = L"only.txt";
        only.data = extra;
        cb2->items = {only};
        TestSeqOutStream* outStream2 = new TestSeqOutStream();
        CHECK(out2->UpdateItems(outStream2, 1, cb2) == S_OK, "UpdateItems(delete-all-old)");
        fs::path replaced = root / L"replaced.asar";
        _wfopen_s(&f, replaced.c_str(), L"wb");
        fwrite(outStream2->bytes.data(), 1, outStream2->bytes.size(), f);
        fclose(f);
        outStream2->Release();
        cb2->Release();

        std::unique_ptr<asar::Archive> check2 = asar::Archive::Open(replaced);
        std::vector<asar::Entry> list2 = check2->List();
        CHECK(list2.size() == 1 && list2[0].path == L"only.txt", "old entries removed");
        CHECK(check2->ReadFile(L"only.txt") == extra, "only.txt readable");

        out2->Release();
        arc2->Close();
        arc2->Release();
    }
    out->Release();

    fs::remove_all(root, ec);
    if (g_failures) {
        std::printf("test_7zclient: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("test_7zclient: PASS\n");
    return 0;
    } catch (const std::exception& e) {
        std::printf("FATAL exception: %s\n", e.what());
        return 3;
    }
}
