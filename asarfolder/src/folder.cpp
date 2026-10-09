// folder.cpp - CAsarFolder: IShellFolder over an .asar archive.
#include "folder.h"

#include <propkey.h>
#include <propsys.h>
#include <shlguid.h>
#include <shlwapi.h>

#include "cache.h"

#include <algorithm>
#include <filesystem>

#include "ui.h"

namespace {

// CompareIDs convention: the SHORT part of the result gives the ordering.
constexpr HRESULT kCmpNegative = (HRESULT)(short)-1;
constexpr HRESULT kCmpPositive = (HRESULT)(short)1;

// Walks a pidl and returns the number of bytes up to (excluding) the
// terminator of the last item.
UINT PidlByteLength(PCIDLIST_ABSOLUTE pidl) {
    UINT total = 0;
    for (auto* item = reinterpret_cast<PCITEMID_CHILD>(pidl); item && item->mkid.cb;
         item = reinterpret_cast<PCITEMID_CHILD>(
             reinterpret_cast<const BYTE*>(item) + item->mkid.cb)) {
        total += item->mkid.cb;
    }
    return total;
}

// True when the last item of the pidl carries our magic.
bool LastItemIsOurs(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return false;
    const BYTE* base = reinterpret_cast<const BYTE*>(pidl);
    UINT total = PidlByteLength(pidl);
    UINT offset = 0;
    bool lastWasOurs = false;
    for (;;) {
        auto* item = reinterpret_cast<PCITEMID_CHILD>(base + offset);
        if (!item->mkid.cb) break;
        auto* asar = reinterpret_cast<const AsarPidlItem*>(item);
        lastWasOurs = (item->mkid.cb >= sizeof(AsarPidlItem) && asar->magic == kPidlMagic);
        offset += item->mkid.cb;
        if (offset >= total) break;
    }
    return lastWasOurs;
}

// TEMPORARY tracing (remove before ship).
inline void DbgLog(const char* what) {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::wstring log = std::wstring(temp) + L"asarfolder-dbg.log";
    HANDLE hf = CreateFileW(log.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(hf, what, (DWORD)strlen(what), &w, nullptr);
        CloseHandle(hf);
    }
}
inline void DbgLogGuid(const char* what, REFIID riid) {
    wchar_t buf[64] = {};
    StringFromGUID2(riid, buf, 64);
    char mb[80] = {};
    WideCharToMultiByte(CP_ACP, 0, buf, -1, mb, sizeof(mb), nullptr, nullptr);
    char line[128];
    snprintf(line, sizeof(line), "%s %s\r\n", what, mb);
    DbgLog(line);
}

namespace {
// No tracing in release builds.
}  // namespace

}  // namespace

// ---------------------------------------------------------------------------
// TEMPORARY: inert stubs for shell interfaces the namespace extension may be
// asked for. Every method logs (so the trace shows which ones Explorer actually
// calls) and returns E_NOTIMPL, which is safe for an optional interface.
// ---------------------------------------------------------------------------

namespace {

// {B3A4B685-B685-4805-99D9-5DEAD2873236} = IParentAndItem
const GUID kIID_IParentAndItem = {
    0xB3A4B685, 0xB685, 0x4805, {0x99, 0xD9, 0x5D, 0xEA, 0xD2, 0x87, 0x32, 0x36}};
// {CDE725B0-CCC9-4519-917E-325D72FAB4CE} (undocumented; asked for by Win11 defview)
const GUID kIID_Unknown_A = {
    0xCDE725B0, 0xCCC9, 0x4519, {0x91, 0x7E, 0x32, 0x5D, 0x72, 0xFA, 0xB4, 0xCE}};
// {1D3DB207-7AD5-4376-AD5B-56474FA7CC68} (undocumented; asked for by Win11 defview)
const GUID kIID_Unknown_B = {
    0x1D3DB207, 0x7AD5, 0x4376, {0xAD, 0x5B, 0x56, 0x47, 0x4F, 0xA7, 0xCC, 0x68}};
// {711B2CFD-93D1-422B-BDF4-69BE923F2449} = IShellFolder3 (Win11, undocumented)
const GUID kIID_IShellFolder3 = {
    0x711B2CFD, 0x93D1, 0x422B, {0xBD, 0xF4, 0x69, 0xBE, 0x92, 0x3F, 0x24, 0x49}};

#define STUB_METHOD(n)                                                     \
    STDMETHOD(n)() override {                                              \
        DbgLog(#n "\r\n");                                                 \
        return E_NOTIMPL;                                                  \
    }

// The SDK only forward-declares IParentAndItem in this configuration, so spell
// out the vtable (2 methods after IUnknown) ourselves.
struct IParentAndItemDecl : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetParentAndItem(PIDLIST_ABSOLUTE pidlParent,
                                                       IShellFolder* psf,
                                                       PCUITEMID_CHILD pidlChild) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParentAndItem(PIDLIST_ABSOLUTE* ppidlParent,
                                                       IShellFolder** ppsf,
                                                       PITEMID_CHILD* ppidlChild) = 0;
};

// Vtables are padded with extra no-argument methods so that even an
// undocumented layout cannot run past the end of the stub's vtable.
#define STUB_PAD_METHODS()                                                 \
    STDMETHOD(a1)() { DbgLog("?a1\r\n"); return E_NOTIMPL; }               \
    STDMETHOD(a2)() { DbgLog("?a2\r\n"); return E_NOTIMPL; }               \
    STDMETHOD(a3)() { DbgLog("?a3\r\n"); return E_NOTIMPL; }               \
    STDMETHOD(a4)() { DbgLog("?a4\r\n"); return E_NOTIMPL; }               \
    STDMETHOD(a5)() { DbgLog("?a5\r\n"); return E_NOTIMPL; }               \
    STDMETHOD(a6)() { DbgLog("?a6\r\n"); return E_NOTIMPL; }               \
    STDMETHOD(a7)() { DbgLog("?a7\r\n"); return E_NOTIMPL; }               \
    STDMETHOD(a8)() { DbgLog("?a8\r\n"); return E_NOTIMPL; }               \
    STDMETHOD(a9)() { DbgLog("?a9\r\n"); return E_NOTIMPL; }               \
    STDMETHOD(a10)() { DbgLog("?a10\r\n"); return E_NOTIMPL; }             \
    STDMETHOD(a11)() { DbgLog("?a11\r\n"); return E_NOTIMPL; }             \
    STDMETHOD(a12)() { DbgLog("?a12\r\n"); return E_NOTIMPL; }             \
    STDMETHOD(a13)() { DbgLog("?a13\r\n"); return E_NOTIMPL; }             \
    STDMETHOD(a14)() { DbgLog("?a14\r\n"); return E_NOTIMPL; }             \
    STDMETHOD(a15)() { DbgLog("?a15\r\n"); return E_NOTIMPL; }             \
    STDMETHOD(a16)() { DbgLog("?a16\r\n"); return E_NOTIMPL; }

// Stub object for an interface whose real vtable we do not know: it answers
// IUnknown/QueryInterface and pads the vtable so any call lands on a harmless
// method that logs. The Tag keeps the instantiations' vtables distinct.
template <typename Tag>
class CUnknownStub final : public IUnknown {
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 2; }
    STDMETHOD_(ULONG, Release)() override { return 2; }
    STUB_PAD_METHODS()
};

struct TagUnknownA {};
struct TagUnknownB {};

class CParentAndItemStub final : public IParentAndItemDecl {
public:
    // parentPidl: absolute pidl of the *containing folder* (e.g. the Desktop
    // pidl list ending at the .asar file's parent).
    explicit CParentAndItemStub(PIDLIST_ABSOLUTE parentPidl) {
        m_parent = parentPidl ? ILClone(parentPidl) : nullptr;
    }
    ~CParentAndItemStub() {
        if (m_parent) ILFree(m_parent);
        if (m_psf) m_psf->Release();
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(static_cast<IParentAndItemDecl*>(this));
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&m_refs); }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }

    STDMETHOD(SetParentAndItem)(PIDLIST_ABSOLUTE pidlParent, IShellFolder* psf,
                                PCUITEMID_CHILD) override {
        DbgLog("IParentAndItem::SetParentAndItem\r\n");
        if (m_parent) {
            ILFree(m_parent);
            m_parent = nullptr;
        }
        m_parent = pidlParent ? ILClone(pidlParent) : nullptr;
        if (m_psf) {
            m_psf->Release();
            m_psf = nullptr;
        }
        m_psf = psf;
        if (m_psf) m_psf->AddRef();
        return S_OK;
    }

    STDMETHOD(GetParentAndItem)(PIDLIST_ABSOLUTE* ppidlParent, IShellFolder** ppsf,
                                PITEMID_CHILD* ppidlChild) override {
        DbgLog("IParentAndItem::GetParentAndItem\r\n");
        if (ppidlParent) *ppidlParent = nullptr;
        if (ppsf) *ppsf = nullptr;
        if (ppidlChild) *ppidlChild = nullptr;
        if (!m_parent) return E_FAIL;

        IShellFolder* parent = m_psf;
        PCUITEMID_CHILD child = nullptr;
        IShellFolder* bound = nullptr;
        if (SUCCEEDED(SHBindToParent(m_parent, IID_IShellFolder,
                                     reinterpret_cast<void**>(&bound), &child))) {
            if (parent) parent->AddRef();
            else parent = bound;
            if (bound && bound != parent) bound->Release();
        }

        if (ppidlParent) {
            PIDLIST_ABSOLUTE folderPidl = ILClone(m_parent);
            if (folderPidl) ILRemoveLastID(folderPidl);
            *ppidlParent = folderPidl;
        }
        if (ppidlChild) *ppidlChild = child ? ILCloneFirst(child) : nullptr;
        if (ppsf) *ppsf = parent;
        else if (parent) parent->Release();
        return S_OK;
    }

private:
    LONG m_refs = 1;
    PIDLIST_ABSOLUTE m_parent = nullptr;
    IShellFolder* m_psf = nullptr;
};

// ---------------------------------------------------------------------------
// TEMPORARY/diagnosis: IShellItem for the archive file itself. Win11's shell
// keeps turning the bound IShellFolder back into an IShellItem; without this
// the view pipeline bails out with E_NOINTERFACE.
// ---------------------------------------------------------------------------
class CAsarShellItem final : public IShellItem {
public:
    explicit CAsarShellItem(PIDLIST_ABSOLUTE filePidl) {
        m_pidl = filePidl ? ILClone(filePidl) : nullptr;
        if (m_pidl) {
            wchar_t path[MAX_PATH] = {};
            if (SHGetPathFromIDListW(m_pidl, path)) {
                m_path = path;
                size_t slash = m_path.find_last_of(L'\\');
                m_name = slash == std::wstring::npos ? m_path : m_path.substr(slash + 1);
            }
        }
    }
    ~CAsarShellItem() {
        if (m_pidl) ILFree(m_pidl);
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(static_cast<IShellItem*>(this));
        } else if (riid == IID_IShellItem) {
            *ppv = static_cast<IShellItem*>(this);
        } else {
            *ppv = nullptr;
            DbgLogGuid("IShellItem QI refused", riid);
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&m_refs); }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }

    STDMETHOD(BindToHandler)(IBindCtx* pbc, REFGUID bhid, REFIID riid,
                               void** ppv) override {
        DbgLogGuid("IShellItem::BindToHandler bhid", bhid);
        DbgLogGuid("IShellItem::BindToHandler riid", riid);
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (bhid == BHID_SFObject) {
            // The view pipeline turns our folder back into an item and then
            // asks for the folder again. Hand back a freshly initialized
            // CAsarFolder for the same archive pidl.
            CAsarFolder* folder = new (std::nothrow) CAsarFolder();
            if (!folder) return E_OUTOFMEMORY;
            HRESULT hr = folder->Initialize(m_pidl);
            if (FAILED(hr)) {
                folder->Release();
                return hr;
            }
            hr = folder->QueryInterface(riid, ppv);
            folder->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    STDMETHOD(GetParent)(IShellItem** ppsi) override {
        DbgLog("IShellItem::GetParent\r\n");
        if (!ppsi) return E_POINTER;
        *ppsi = nullptr;
        if (!m_pidl) return E_FAIL;
        PIDLIST_ABSOLUTE parent = ILClone(m_pidl);
        if (!parent) return E_OUTOFMEMORY;
        ILRemoveLastID(parent);
        HRESULT hr = SHCreateItemFromIDList(parent, IID_IShellItem,
                                            reinterpret_cast<void**>(ppsi));
        ILFree(parent);
        return hr;
    }

    STDMETHOD(GetDisplayName)(SIGDN sigdn, PWSTR* ppszName) override {
        DbgLog("IShellItem::GetDisplayName\r\n");
        if (!ppszName) return E_POINTER;
        *ppszName = nullptr;
        std::wstring text;
        switch (sigdn) {
            case SIGDN_NORMALDISPLAY:
            case SIGDN_PARENTRELATIVE:
            case SIGDN_PARENTRELATIVEEDITING:
            case SIGDN_PARENTRELATIVEPARSING:
            case SIGDN_PARENTRELATIVEFORADDRESSBAR:
            case SIGDN_PARENTRELATIVEFORUI:
                text = m_name;
                break;
            default:
                text = m_path;
                break;
        }
        size_t cb = (text.size() + 1) * sizeof(wchar_t);
        auto* out = static_cast<wchar_t*>(CoTaskMemAlloc(cb));
        if (!out) return E_OUTOFMEMORY;
        memcpy(out, text.c_str(), cb);
        *ppszName = out;
        return S_OK;
    }

    STDMETHOD(GetAttributes)(SFGAOF sfgaoMask, SFGAOF* psfgaoAttribs) override {
        DbgLog("IShellItem::GetAttributes\r\n");
        if (!psfgaoAttribs) return E_POINTER;
        *psfgaoAttribs = sfgaoMask & (SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_FILESYSTEM |
                                      SFGAO_CANCOPY | SFGAO_STREAM | SFGAO_HASSUBFOLDER);
        return S_OK;
    }

    STDMETHOD(Compare)(IShellItem* psi, SICHINTF hint, int* piOrder) override {
        DbgLog("IShellItem::Compare\r\n");
        if (!psi || !piOrder) return E_POINTER;
        PWSTR other = nullptr;
        int cmp = 1;
        if (SUCCEEDED(psi->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &other)) && other) {
            cmp = _wcsicmp(m_path.c_str(), other);
            CoTaskMemFree(other);
        }
        *piOrder = cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
        return S_OK;
    }

private:
    LONG m_refs = 1;
    PIDLIST_ABSOLUTE m_pidl = nullptr;
    std::wstring m_path;
    std::wstring m_name;
};

}  // namespace

// Byte length of the filesystem prefix of a pidl we produced, i.e. the offset
// where the first of our private items starts. 0 when the pidl has no such item.
UINT FilesystemPrefixBytes(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return 0;
    const BYTE* base = reinterpret_cast<const BYTE*>(pidl);
    UINT total = PidlByteLength(pidl);
    for (UINT offset = 0; offset < total;) {
        auto* item = reinterpret_cast<PCITEMID_CHILD>(base + offset);
        if (!item->mkid.cb) break;
        if (item->mkid.cb >= sizeof(AsarPidlItem) &&
            reinterpret_cast<const AsarPidlItem*>(item)->magic == kPidlMagic) {
            return offset;
        }
        offset += item->mkid.cb;
    }
    return 0;
}

// Splits an absolute pidl into filesystem prefix + our trailing items.
bool SplitArchivePidl(PCIDLIST_ABSOLUTE pidl, std::wstring& archivePath,
                      std::wstring& subPath) {
    archivePath.clear();
    subPath.clear();
    if (!pidl) return false;

    // Count the byte length of the trailing run of our items.
    const BYTE* base = reinterpret_cast<const BYTE*>(pidl);
    UINT total = PidlByteLength(pidl);
    UINT offset = 0;
    UINT ourStart = total;  // one past the end of the filesystem part
    bool sawOurs = false;
    for (;;) {
        auto* item = reinterpret_cast<PCITEMID_CHILD>(base + offset);
        if (!item->mkid.cb) break;
        auto* asar = reinterpret_cast<const AsarPidlItem*>(item);
        bool ours = (item->mkid.cb >= sizeof(AsarPidlItem) && asar->magic == kPidlMagic);
        if (ours) {
            if (!sawOurs) ourStart = offset;
            sawOurs = true;
        } else if (!ours && sawOurs) {
            // filesystem item after ours: not a pidl we produced
            return false;
        }
        offset += item->mkid.cb;
        if (offset >= total) break;
    }
    if (!sawOurs) return false;

    // Path from the filesystem prefix.
    std::vector<BYTE> prefix(base, base + ourStart);
    prefix.push_back(0);
    prefix.push_back(0);
    wchar_t path[MAX_PATH] = {};
    if (!SHGetPathFromIDListW(reinterpret_cast<PCIDLIST_ABSOLUTE>(prefix.data()), path))
        return false;
    archivePath = path;

    // Sub path from our items.
    for (UINT pos = ourStart; pos < total;) {
        auto* item = reinterpret_cast<PCITEMID_CHILD>(base + pos);
        if (!item->mkid.cb) break;
        subPath += PidlItemName(item);
        pos += item->mkid.cb;
        if (pos < total) subPath += L'/';
    }
    return true;
}

PIDLIST_RELATIVE MakeChildPidl(const asar::Entry& entry) {
    // Store only the leaf name: PIDLs are relative to their parent folder.
    // Storing the full path broke nested navigation (absolute pidls would
    // concatenate to "docs/docs/readme.md"). Display already strips to leaf;
    // full paths are reconstructed via CAsarFolder::ChildPath().
    std::wstring leaf = entry.path;
    size_t slash = leaf.find_last_of(L'/');
    if (slash != std::wstring::npos) leaf = leaf.substr(slash + 1);
    // Hard bound: SHITEMID cb is USHORT and Explorer copies PIDLs onto
    // stacks in places; refuse absurd names instead of overflowing.
    if (leaf.empty() || leaf.size() > 512) return nullptr;
    size_t nameChars = leaf.size() + 1;
    size_t nameBytes = nameChars * sizeof(wchar_t);
    // name[] declared as [1]: allocate the full size.
    size_t cb = sizeof(AsarPidlItem) - sizeof(wchar_t) + nameBytes;
    auto* item = static_cast<AsarPidlItem*>(CoTaskMemAlloc(cb));
    if (!item) return nullptr;
    memset(item, 0, cb);
    item->cb = static_cast<USHORT>(cb);
    item->magic = kPidlMagic;
    item->isDir = entry.isDir ? 1 : 0;
    item->size = entry.size;
    wcscpy_s(item->name, nameChars, leaf.c_str());
    return reinterpret_cast<PIDLIST_RELATIVE>(item);
}

namespace {

std::wstring LeafNameOf(const asar::Entry& entry) {
    size_t slash = entry.path.find_last_of(L'/');
    return slash == std::wstring::npos ? entry.path : entry.path.substr(slash + 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

HRESULT CAsarFolder::EnsureLoaded() {
    if (m_loaded) return S_OK;
    try {
        // Shared cache: repeat binds/enums in one Explorer session must not
        // re-parse a 30k-entry header every time (multi-second UI hangs).
        // The tree is shared read-only; folders keep a subtree pointer, so
        // navigation costs O(depth) map lookups instead of O(entries) copies.
        m_header = asarcache::GetHeader(m_archivePath);
        m_node = &m_header->root;
        if (!m_subPath.empty()) {
            for (const std::wstring& c : asar::SplitArchivePath(m_subPath)) {
                auto it = m_node->files.find(c);
                if (it == m_node->files.end() || !it->second.isDir) {
                    m_node = nullptr;
                    break;
                }
                m_node = &it->second;
            }
        }
        m_loaded = true;
        return m_node ? S_OK : E_FAIL;
    } catch (const std::exception&) {
        m_header.reset();
        m_node = nullptr;
        m_loaded = true;  // remember the failure; empty folder
        return E_FAIL;
    } catch (...) {
        m_header.reset();
        m_node = nullptr;
        m_loaded = true;
        return E_FAIL;
    }
}

const asar::Node* CAsarFolder::FindNode(const std::wstring& name) const {
    // Full in-archive path; case-insensitive walk from the subtree root.
    if (!m_node || !m_header) return nullptr;
    const asar::Node* n = &m_header->root;
    std::vector<std::wstring> comps = asar::SplitArchivePath(name);
    for (const std::wstring& c : comps) {
        auto it = n->files.find(c);
        if (it == n->files.end()) return nullptr;
        n = &it->second;
    }
    return n;
}

std::wstring CAsarFolder::ChildPath(const std::wstring& name) const {
    if (m_subPath.empty()) return name;
    return m_subPath + L"/" + name;
}

HRESULT CAsarFolder::InitDirect(const std::wstring& archivePath,
                                const std::wstring& subPath) {
    m_archivePath = archivePath;
    m_subPath = subPath;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IUnknown / IPersist
// ---------------------------------------------------------------------------

CAsarFolder::~CAsarFolder() {
    if (m_pidl) ILFree(m_pidl);
}

STDMETHODIMP CAsarFolder::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown) *ppv = static_cast<IUnknown*>(static_cast<IShellFolder*>(this));
    else if (riid == IID_IShellFolder) *ppv = static_cast<IShellFolder*>(this);
    else if (riid == IID_IShellFolder2) *ppv = static_cast<IShellFolder2*>(this);
    else if (riid == IID_IObjectWithFolderEnumMode)
        *ppv = static_cast<IObjectWithFolderEnumMode*>(this);
    else if (riid == IID_IShellIcon) *ppv = static_cast<IShellIcon*>(this);
    else if (riid == IID_IPersist) *ppv = static_cast<IPersist*>(this);
    else if (riid == IID_IPersistFolder) *ppv = static_cast<IPersistFolder*>(this);
    else if (riid == IID_IPersistFolder2) *ppv = static_cast<IPersistFolder2*>(this);
    else if (riid == IID_IPersistFolder3) *ppv = static_cast<IPersistFolder3*>(this);
    else if (riid == kIID_IParentAndItem) {
        DbgLogGuid("QI -> IParentAndItem", riid);
        auto* stub = new (std::nothrow) CParentAndItemStub(m_pidl);
        if (!stub) return E_OUTOFMEMORY;
        *ppv = static_cast<IParentAndItemDecl*>(stub);
        return S_OK;
    } else if (riid == IID_IShellItem) {
        // Prefer the system item for the archive file itself: it already
        // supports IParentAndItem, IPersistFolder2-related queries and all
        // the undocumented helpers the Win11 view pipeline probes for. Our
        // minimal CAsarShellItem stub refused them and SHCreateShellFolderView
        // bailed out with E_NOINTERFACE.
        if (m_pidl) {
            HRESULT sysHr = SHCreateItemFromIDList(m_pidl, IID_IShellItem, ppv);
            if (SUCCEEDED(sysHr)) {
                DbgLog("QI -> IShellItem (system)\r\n");
                return sysHr;
            }
        }
        DbgLogGuid("QI -> IShellItem (fallback stub)", riid);
        auto* item = new (std::nothrow) CAsarShellItem(m_pidl);
        if (!item) return E_OUTOFMEMORY;
        *ppv = static_cast<IShellItem*>(item);
        return S_OK;
    } else {
        *ppv = nullptr;
        DbgLogGuid("QI refused", riid);
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CAsarFolder::AddRef() { return InterlockedIncrement(&m_refs); }

STDMETHODIMP_(ULONG) CAsarFolder::Release() {
    ULONG r = InterlockedDecrement(&m_refs);
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP CAsarFolder::GetClassID(CLSID* clsid) {
    *clsid = CLSID_AsarFolder;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IPersistFolder / IPersistFolder3
// ---------------------------------------------------------------------------

STDMETHODIMP CAsarFolder::Initialize(PCIDLIST_ABSOLUTE pidl) {
    DbgLog("Initialize\r\n");
    if (m_pidl) ILFree(m_pidl);
    m_pidl = pidl ? ILClone(pidl) : nullptr;
    std::wstring archivePath, subPath;
    if (SplitArchivePidl(const_cast<PCIDLIST_ABSOLUTE>(pidl), archivePath, subPath)) {
        m_archivePath = archivePath;
        m_subPath = subPath;
        DbgLog("Initialize -> subpath ok\r\n");
        return S_OK;
    }
    // A plain filesystem pidl without our tail: the .asar file itself (root).
    wchar_t path[MAX_PATH] = {};
    if (pidl && SHGetPathFromIDListW(pidl, path)) {
        m_archivePath = path;
        m_subPath.clear();
        DbgLog("Initialize -> fs path ok\r\n");
        return S_OK;
    }
    DbgLog("Initialize -> E_INVALIDARG\r\n");
    return E_INVALIDARG;
}

STDMETHODIMP CAsarFolder::GetCurFolder(PIDLIST_ABSOLUTE* ppidl) {
    DbgLog("GetCurFolder\r\n");
    if (!ppidl) return E_POINTER;
    *ppidl = m_pidl ? ILClone(m_pidl) : nullptr;
    return m_pidl ? S_OK : S_FALSE;
}

STDMETHODIMP CAsarFolder::InitializeEx(IBindCtx*, PCIDLIST_ABSOLUTE pidlRoot,
                                       const PERSIST_FOLDER_TARGET_INFO*) {
    DbgLog("InitializeEx\r\n");
    return Initialize(pidlRoot);
}

STDMETHODIMP CAsarFolder::GetFolderTargetInfo(PERSIST_FOLDER_TARGET_INFO*) {
    return E_NOTIMPL;
}

// ---------------------------------------------------------------------------
// IShellFolder
// ---------------------------------------------------------------------------

namespace {

class CEnumIdList final : public IEnumIDList {
public:
    CEnumIdList() = default;
    ~CEnumIdList() {
        for (PITEMID_CHILD p : m_items)
            if (p) CoTaskMemFree(p);
    }

    void Add(PITEMID_CHILD pidl) { m_items.push_back(pidl); }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumIDList)
            *ppv = static_cast<IEnumIDList*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&m_refs); }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }

    STDMETHOD(Next)(ULONG celt, PITEMID_CHILD* rgelt, ULONG* pceltFetched) override {
        if (!rgelt && celt > 0) return E_POINTER;
        if (pceltFetched) *pceltFetched = 0;
        ULONG fetched = 0;
        while (fetched < celt && m_index < m_items.size()) {
            // Caller frees; give it its own copy (single item + terminator).
            // Copy only the stored item bytes; the terminator is written
            // fresh (never over-read past the stored buffer).
            ULONG itemCb = reinterpret_cast<PCITEMID_CHILD>(m_items[m_index])->mkid.cb;
            ULONG cb = itemCb + sizeof(USHORT);
            auto* copy = static_cast<PITEMID_CHILD>(CoTaskMemAlloc(cb));
            if (!copy) return E_OUTOFMEMORY;
            memcpy(copy, m_items[m_index], itemCb);
            *reinterpret_cast<USHORT*>(reinterpret_cast<BYTE*>(copy) + cb - sizeof(USHORT)) = 0;
            rgelt[fetched++] = copy;
            ++m_index;
        }
        if (pceltFetched) *pceltFetched = fetched;
        return fetched == celt ? S_OK : S_FALSE;
    }
    STDMETHOD(Skip)(ULONG celt) override {
        m_index += celt;
        return S_OK;
    }
    STDMETHOD(Reset)() override {
        m_index = 0;
        return S_OK;
    }
    STDMETHOD(Clone)(IEnumIDList** ppenum) override {
        if (!ppenum) return E_POINTER;
        auto* clone = new (std::nothrow) CEnumIdList();
        if (!clone) return E_OUTOFMEMORY;
        for (PITEMID_CHILD p : m_items) {
            if (!p) continue;
            ULONG itemCb = reinterpret_cast<PCITEMID_CHILD>(p)->mkid.cb;
            ULONG cb = itemCb + sizeof(USHORT);
            auto* copy = static_cast<PITEMID_CHILD>(CoTaskMemAlloc(cb));
            if (!copy) { clone->Release(); return E_OUTOFMEMORY; }
            memcpy(copy, p, itemCb);
            *reinterpret_cast<USHORT*>(reinterpret_cast<BYTE*>(copy) + cb - sizeof(USHORT)) = 0;
            clone->Add(copy);
        }
        clone->m_index = m_index;
        *ppenum = clone;
        return S_OK;
    }

private:
    LONG m_refs = 1;
    std::vector<PITEMID_CHILD> m_items;
    size_t m_index = 0;
};

}  // namespace

STDMETHODIMP CAsarFolder::ParseDisplayName(HWND, IBindCtx*, PWSTR pszDisplayName,
                                           ULONG* pchEaten, PIDLIST_RELATIVE* ppidl,
                                           DWORD* pdwAttributes) {
    if (!ppidl) return E_POINTER;
    *ppidl = nullptr;
    if (!pszDisplayName) return E_INVALIDARG;
    HRESULT hr = EnsureLoaded();
    if (FAILED(hr)) return hr;

    std::wstring wanted = pszDisplayName;
    // Normalize: backslashes to forward slashes, strip leading separators.
    std::replace(wanted.begin(), wanted.end(), L'\\', L'/');
    while (!wanted.empty() && wanted.front() == L'/') wanted.erase(wanted.begin());

    const asar::Node* n = m_node;
    size_t from = 0;
    while (from <= wanted.size()) {
        size_t slash = wanted.find(L'/', from);
        std::wstring part =
            wanted.substr(from, slash == std::wstring::npos ? std::wstring::npos
                                                            : slash - from);
        if (!part.empty()) {
            if (!n || !n->isDir) return MK_E_SYNTAX;
            auto it = n->files.find(part);
            if (it == n->files.end()) return MK_E_SYNTAX;
            n = &it->second;
        }
        if (slash == std::wstring::npos) break;
        from = slash + 1;
    }
    if (!n || n == m_node) return MK_E_SYNTAX;

    asar::Entry tmp;
    tmp.path = wanted;
    tmp.isDir = n->isDir;
    tmp.isLink = n->isLink;
    tmp.size = n->size;
    *ppidl = MakeChildPidl(tmp);
    if (!*ppidl) return E_OUTOFMEMORY;
    if (pdwAttributes) {
        ULONG mask = *pdwAttributes;
        DWORD attrs = SFGAO_CANCOPY;
        if (n->isDir) attrs |= SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_HASPROPSHEET;
        *pdwAttributes = mask & attrs;
    }
    if (pchEaten) *pchEaten = static_cast<ULONG>(wcslen(pszDisplayName));
    return S_OK;
}

STDMETHODIMP CAsarFolder::EnumObjects(HWND, SHCONTF grfFlags, IEnumIDList** ppeid) {
    DbgLog("EnumObjects grf=");
    {
        char b[32];
        snprintf(b, sizeof(b), "0x%08lX\r\n", (unsigned long)grfFlags);
        DbgLog(b);
    }
    if (!ppeid) return E_POINTER;
    *ppeid = nullptr;
    HRESULT hr = EnsureLoaded();
    if (FAILED(hr)) return hr;

    bool wantFolders = (grfFlags & SHCONTF_FOLDERS) != 0;
    bool wantFiles = (grfFlags & SHCONTF_NONFOLDERS) != 0;

    auto* enumobj = new (std::nothrow) CEnumIdList();
    if (!enumobj) return E_OUTOFMEMORY;
    if (!m_node) return E_FAIL;
    // Immediate children straight off the shared tree: no flatten, no filter,
    // no copying. The map key is the leaf name relative to this folder.
    for (const auto& kv : m_node->files) {
        bool isDir = kv.second.isDir;
        if (isDir && !wantFolders) continue;
        if (!isDir && !wantFiles) continue;
        asar::Entry tmp;
        tmp.path = kv.first;  // leaf, relative to this folder
        tmp.isDir = isDir;
        tmp.isLink = kv.second.isLink;
        tmp.size = kv.second.size;
        PITEMID_CHILD pidl = MakeChildPidl(tmp);
        if (pidl) enumobj->Add(pidl);
    }
    *ppeid = enumobj;
    return S_OK;
}

STDMETHODIMP CAsarFolder::BindToObject(PCUIDLIST_RELATIVE pidl, IBindCtx*, REFIID riid,
                                       void** ppvOut) {
    DbgLogGuid("BindToObject", riid);
    if (!ppvOut) return E_POINTER;
    *ppvOut = nullptr;
    if (!pidl) return E_INVALIDARG;
    HRESULT hr = EnsureLoaded();
    if (FAILED(hr)) return hr;

    // Walk the requested items through the shared tree. Intermediate items
    // must be folders; the last may be a file (property-store binds).
    const asar::Node* n = m_node;
    std::wstring sub = m_subPath;
    int count = 0;
    const asar::Node* last = nullptr;
    const BYTE* base = reinterpret_cast<const BYTE*>(pidl);
    for (;;) {
        auto* item = reinterpret_cast<PCITEMID_CHILD>(base);
        if (!item->mkid.cb) break;
        std::wstring leaf = PidlItemName(item);
        if (leaf.empty() || leaf.find(L'/') != std::wstring::npos)
            return E_INVALIDARG;
        if (!n->isDir) return E_INVALIDARG;
        auto it = n->files.find(leaf);
        if (it == n->files.end()) return E_INVALIDARG;
        n = &it->second;
        sub = sub.empty() ? leaf : sub + L"/" + leaf;
        last = n;
        const BYTE* next = base + item->mkid.cb;
        ++count;
        base = next;
    }
    if (count == 0 || !last) return E_INVALIDARG;

    // Property store binds work for files and folders (sizes in Explorer).
    if (riid == IID_IPropertyStore) {
        PCUITEMID_CHILD single = nullptr;
        // Re-derive single relative pidl for the store (leaf only).
        // CreateItemPropertyStore takes apidl array; build one from last leaf.
        // Simplest: use the original pidl's last item via ILFindLastID.
        PCUIDLIST_RELATIVE lastId = pidl;
        const BYTE* b2 = reinterpret_cast<const BYTE*>(pidl);
        PCUIDLIST_RELATIVE cur = pidl;
        for (;;) {
            auto* it = reinterpret_cast<PCUIDLIST_RELATIVE>(const_cast<BYTE*>(b2));
            if (!it->mkid.cb) break;
            cur = it;
            b2 += it->mkid.cb;
        }
        lastId = cur;
        PCUITEMID_CHILD arr[1] = {
            reinterpret_cast<PCUITEMID_CHILD>(const_cast<PUIDLIST_RELATIVE>(lastId))};
        return CreateItemPropertyStore(1, arr, riid, ppvOut);
    }

    if (!last->isDir) return E_INVALIDARG;  // only folders bind to folders

    auto* child = new (std::nothrow) CAsarFolder();
    if (!child) return E_OUTOFMEMORY;
    child->InitDirect(m_archivePath, n->isDir ? sub : std::wstring());
    // Preserve the absolute pidl so GetCurFolder succeeds for subfolders.
    // GetFolder/automation fails with E_FAIL when GetCurFolder returns S_FALSE.
    if (m_pidl && pidl) {
        PIDLIST_ABSOLUTE abs = ILCombine(m_pidl, pidl);
        if (abs) {
            if (child->m_pidl) ILFree(child->m_pidl);
            child->m_pidl = abs;
        }
    }
    hr = child->QueryInterface(riid, ppvOut);
    child->Release();
    return hr;
}

STDMETHODIMP CAsarFolder::BindToStorage(PCUIDLIST_RELATIVE, IBindCtx*, REFIID,
                                        void** ppvObj) {
    if (ppvObj) *ppvObj = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP CAsarFolder::CompareIDs(LPARAM, PCUIDLIST_RELATIVE pidl1,
                                     PCUIDLIST_RELATIVE pidl2) {
    std::wstring n1 = PidlItemName(pidl1);
    std::wstring n2 = PidlItemName(pidl2);
    // Folders first, then case-insensitive by name.
    const AsarPidlItem* i1 = PidlAsItem(pidl1);
    const AsarPidlItem* i2 = PidlAsItem(pidl2);
    if (i1 && i2 && i1->isDir != i2->isDir) return i1->isDir ? kCmpNegative : kCmpPositive;
    int cmp = _wcsicmp(n1.c_str(), n2.c_str());
    if (cmp < 0) return kCmpNegative;
    if (cmp > 0) return kCmpPositive;
    return 0;
}

STDMETHODIMP CAsarFolder::CreateViewObject(HWND, REFIID riid, void** ppvOut) {
    DbgLog("CreateViewObject");
    wchar_t buf[64] = {};
    StringFromGUID2(riid, buf, 64);
    char mb[80] = {};
    WideCharToMultiByte(CP_ACP, 0, buf, -1, mb, sizeof(mb), nullptr, nullptr);
    char line[160];
    snprintf(line, sizeof(line), "CreateViewObject %s\r\n", mb);
    DbgLog(line);
    if (!ppvOut) return E_POINTER;
    *ppvOut = nullptr;
    // The shell view: without this Explorer cannot display the folder at all
    // (navigation would silently do nothing). The system default view gives
    // the same experience as .zip folders.
    if (riid == IID_IShellView) {
        DbgLog("  -> building SFV_CREATE\r\n");
        // Make sure the archive really loads before handing out a view: a
        // folder whose IShellFolder calls fail makes the view creator bail out.
        HRESULT loadHr = EnsureLoaded();
        {
            char b[64];
            snprintf(b, sizeof(b), "  EnsureLoaded -> 0x%08lX children=%u\r\n",
                     (unsigned long)loadHr,
                     (unsigned)(m_node ? m_node->files.size() : 0));
            DbgLog(b);
        }
        SFV_CREATE sfv = {};
        sfv.cbSize = sizeof(sfv);
        sfv.pshf = static_cast<IShellFolder*>(this);
        HRESULT hr = SHCreateShellFolderView(&sfv,
                                             reinterpret_cast<IShellView**>(ppvOut));
        snprintf(line, sizeof(line), "  SHCreateShellFolderView -> 0x%08lX\r\n",
                 (unsigned long)hr);
        DbgLog(line);
        return hr;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP CAsarFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                          SFGAOF* rgfInOut) {
    DbgLog("GetAttributesOf\r\n");
    if (!rgfInOut) return E_POINTER;
    // cidl == 0 asks for the attributes of the folder itself. The shell relies
    // on this (it is how a folder reports SFGAO_FOLDER), so it must succeed.
    if (cidl == 0) {
        *rgfInOut &= SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_HASSUBFOLDER |
                     SFGAO_CANCOPY | SFGAO_STREAM | SFGAO_HASPROPSHEET;
        return S_OK;
    }
    if (!apidl) return E_INVALIDARG;
    SFGAOF result = *rgfInOut;
    for (UINT i = 0; i < cidl; ++i) {
        const AsarPidlItem* item = PidlAsItem(apidl[i]);
        if (!item) return E_INVALIDARG;
        SFGAOF one = SFGAO_CANCOPY | SFGAO_STREAM | SFGAO_HASPROPSHEET;
        if (item->isDir) one |= SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_HASSUBFOLDER;
        result &= one;
    }
    *rgfInOut = result;
    return S_OK;
}

STDMETHODIMP CAsarFolder::GetUIObjectOf(HWND hwnd, UINT cidl,
                                        PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                                        UINT*, void** ppvOut) {
    DbgLogGuid("GetUIObjectOf", riid);
    if (!ppvOut) return E_POINTER;
    *ppvOut = nullptr;
    if (cidl > 0 && !apidl) return E_INVALIDARG;
    if (riid == IID_IExtractIconW) {
        return CreateItemIcon(hwnd, cidl, apidl, riid, ppvOut);
    }
    if (riid == IID_IDataObject) {
        return CreateItemDataObject(m_archivePath, m_subPath, cidl, apidl, riid,
                                    ppvOut);
    }
    if (riid == IID_IContextMenu) {
        // Manual verbs only (Open/Copy/Properties). SHCreateDefaultContextMenu
        // crashes Explorer on some archives (dolilsheba.asar) in system code,
        // so it is intentionally not used here.
        return CreateItemMenu(m_archivePath, m_subPath, m_pidl, cidl, apidl,
                              riid, ppvOut);
    }
    if (riid == IID_IPropertyStore) {
        return CreateItemPropertyStore(cidl, apidl, riid, ppvOut);
    }
    if (riid == IID_IEnumExplorerCommand) {
        return CreateItemExplorerCommands(m_archivePath, m_subPath, m_pidl,
                                          cidl, apidl, riid, ppvOut);
    }
    return E_NOINTERFACE;
}

STDMETHODIMP CAsarFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF uFlags,
                                           STRRET* lpName) {
    std::wstring name = PidlItemName(pidl);
    if (name.empty()) return E_INVALIDARG;
    size_t slash = name.find_last_of(L'/');
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    // Every flag Explorer uses inside the view is parent-relative; return the
    // leaf name for all of them.
    wchar_t* buf = static_cast<wchar_t*>(CoTaskMemAlloc((name.size() + 1) * sizeof(wchar_t)));
    if (!buf) return E_OUTOFMEMORY;
    wcscpy_s(buf, name.size() + 1, name.c_str());
    lpName->uType = STRRET_WSTR;
    lpName->pOleStr = buf;
    return S_OK;
}

STDMETHODIMP CAsarFolder::SetNameOf(HWND, PCUITEMID_CHILD, LPCWSTR, SHGDNF,
                                    PITEMID_CHILD*) {
    return E_FAIL;  // the ASAR format has no in-place rename
}

// ---------------------------------------------------------------------------
// IShellFolder2 - Details view columns
// ---------------------------------------------------------------------------

STDMETHODIMP CAsarFolder::GetDefaultSearchGUID(GUID*) { return E_NOTIMPL; }

STDMETHODIMP CAsarFolder::EnumSearches(IEnumExtraSearch** ppenum) {
    if (ppenum) *ppenum = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP CAsarFolder::GetDefaultColumn(DWORD, ULONG* pSort, ULONG* pDisplay) {
    if (pSort) *pSort = 0;
    if (pDisplay) *pDisplay = 0;
    return S_OK;
}

STDMETHODIMP CAsarFolder::GetDefaultColumnState(UINT iColumn, SHCOLSTATEF* pcsFlags) {
    if (!pcsFlags) return E_POINTER;
    switch (iColumn) {
        case 0: *pcsFlags = SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT; break;
        case 1: *pcsFlags = SHCOLSTATE_TYPE_INT | SHCOLSTATE_ONBYDEFAULT; break;
        case 2: *pcsFlags = SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT; break;
        default: *pcsFlags = SHCOLSTATE_SECONDARYUI; break;
    }
    return S_OK;
}

STDMETHODIMP CAsarFolder::GetDetailsEx(PCUITEMID_CHILD pidl,
                                       const SHCOLUMNID* pscid, VARIANT* pv) {
    if (!pscid || !pv) return E_POINTER;
    VariantInit(pv);
    const AsarPidlItem* item = pidl ? PidlAsItem(pidl) : nullptr;
    // System.Size (PKEY_Size): file payload size; dirs have no size.
    if (IsEqualPropertyKey(*pscid, PKEY_Size)) {
        if (!item || item->isDir) return E_NOTIMPL;
        pv->vt = VT_UI8;
        pv->ullVal = item->size;
        return S_OK;
    }
    // System.ItemNameDisplay (PKEY_ItemNameDisplay): leaf name as BSTR.
    if (IsEqualPropertyKey(*pscid, PKEY_ItemNameDisplay)) {
        if (!item) return E_NOTIMPL;
        std::wstring name = PidlItemName(pidl);
        BSTR b = SysAllocStringLen(name.c_str(), (UINT)name.size());
        if (!b) return E_OUTOFMEMORY;
        pv->vt = VT_BSTR;
        pv->bstrVal = b;
        return S_OK;
    }
    return E_NOTIMPL;
}

STDMETHODIMP CAsarFolder::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn,
                                       SHELLDETAILS* psd) {
    if (!psd) return E_POINTER;
    psd->fmt = LVCFMT_LEFT;
    psd->cxChar = 0;
    switch (iColumn) {
        case 0: {  // Name
            std::wstring name;
            if (pidl) {
                name = PidlItemName(pidl);
                size_t slash = name.find_last_of(L'/');
                if (slash != std::wstring::npos) name = name.substr(slash + 1);
            }
            psd->str.uType = STRRET_WSTR;
            psd->str.pOleStr = static_cast<wchar_t*>(
                CoTaskMemAlloc((name.size() + 1) * sizeof(wchar_t)));
            if (!psd->str.pOleStr) return E_OUTOFMEMORY;
            wcscpy_s(psd->str.pOleStr, name.size() + 1, name.c_str());
            return S_OK;
        }
        case 1: {  // Size
            if (!pidl) return S_OK;
            const AsarPidlItem* item = PidlAsItem(pidl);
            if (!item || item->isDir) return E_NOTIMPL;
            wchar_t buf[32];
            swprintf_s(buf, L"%llu", (unsigned long long)item->size);
            psd->str.uType = STRRET_WSTR;
            psd->str.pOleStr = static_cast<wchar_t*>(
                CoTaskMemAlloc((wcslen(buf) + 1) * sizeof(wchar_t)));
            if (!psd->str.pOleStr) return E_OUTOFMEMORY;
            wcscpy_s(psd->str.pOleStr, wcslen(buf) + 1, buf);
            return S_OK;
        }
        default:
            return E_NOTIMPL;
    }
}

STDMETHODIMP CAsarFolder::MapColumnToSCID(UINT, SHCOLUMNID*) { return E_NOTIMPL; }

// ---------------------------------------------------------------------------
// IObjectWithFolderEnumMode
// ---------------------------------------------------------------------------

STDMETHODIMP CAsarFolder::SetMode(FOLDER_ENUM_MODE feMode) {
    m_enumMode = feMode;
    return S_OK;
}

STDMETHODIMP CAsarFolder::GetMode(FOLDER_ENUM_MODE* pfeMode) {
    if (!pfeMode) return E_POINTER;
    *pfeMode = m_enumMode;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IShellIcon - defview falls back to GetUIObjectOf(IExtractIconW)
// ---------------------------------------------------------------------------

STDMETHODIMP CAsarFolder::GetIconOf(PCUITEMID_CHILD pidl, UINT flags,
                                     PINT lpIconLoc) {
    // System imagelist index so list/details/tiles views show real icons
    // (folders + per-extension) instead of generics. IExtractIconW remains
    // for size-specific extraction.
    if (!lpIconLoc) return E_POINTER;
    const AsarPidlItem* item = PidlAsItem(pidl);
    if (!item) return E_INVALIDARG;
    std::wstring leaf = PidlItemName(pidl);
    if (leaf.empty()) return E_INVALIDARG;
    DWORD attrs =
        item->isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    DWORD extra = SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES;
    if (flags & GIL_OPENICON) extra |= SHGFI_OPENICON;
    SHFILEINFOW sfi = {};
    if (!SHGetFileInfoW(leaf.c_str(), attrs, &sfi, sizeof(sfi), extra))
        return S_FALSE;  // fall back to IExtractIconW
    *lpIconLoc = sfi.iIcon;
    return S_OK;
}
