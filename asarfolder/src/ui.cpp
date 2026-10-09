// ui.cpp - per-item shell UI objects.
#include "ui.h"

#include "cache.h"
#include <propkey.h>
#include <propsys.h>
#include <servprov.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>

namespace {
// No tracing in release builds.
}  // namespace

#include <cstring>
#include <cstdio>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// CItemIcon
// ---------------------------------------------------------------------------

class CItemIcon final : public IExtractIconW {
public:
    CItemIcon(const std::wstring& leaf, bool isDir) : m_leaf(leaf), m_isDir(isDir) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IExtractIconW)
            *ppv = static_cast<IExtractIconW*>(this);
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

    STDMETHOD(GetIconLocation)(UINT, LPWSTR szIconFile, UINT cchMax, int* piIndex,
                               UINT* pwFlags) override {
        // GIL_NOTFILENAME forces the shell to call Extract(), where we hand
        // out the icon the file extension would have on the filesystem.
        // No GIL_PERCLASS: we return the same "*" location for folders and
        // every file type, so per-class caching would share one (generic)
        // icon across all items.
        if (cchMax < 4 || !piIndex || !pwFlags) return E_POINTER;
        szIconFile[0] = L'*';
        szIconFile[1] = 0;
        *piIndex = 0;
        *pwFlags = GIL_NOTFILENAME;
        return S_OK;
    }

    STDMETHOD(Extract)(LPCWSTR, UINT, HICON* hiconLarge, HICON* hiconSmall,
                       UINT) override {
        if (!hiconLarge && !hiconSmall) return E_POINTER;
        SHFILEINFOW sfi = {};
        DWORD attrs = m_isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (hiconLarge) {
            *hiconLarge = nullptr;
            if (SHGetFileInfoW(m_leaf.c_str(), attrs, &sfi, sizeof(sfi),
                               SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | SHGFI_LARGEICON)) {
                *hiconLarge = sfi.hIcon;
            }
        }
        if (hiconSmall) {
            *hiconSmall = nullptr;
            if (SHGetFileInfoW(m_leaf.c_str(), attrs, &sfi, sizeof(sfi),
                               SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | SHGFI_SMALLICON)) {
                *hiconSmall = sfi.hIcon;
            }
        }
        return S_OK;
    }

private:
    LONG m_refs = 1;
    std::wstring m_leaf;
    bool m_isDir;
};

// ---------------------------------------------------------------------------
// CArchiveStream - read-only IStream over one entry's payload
// ---------------------------------------------------------------------------

class CArchiveStream final : public IStream {
public:
    CArchiveStream(const std::wstring& archivePath, uint64_t offset, uint64_t size)
        : m_offset(offset), m_size(size) {
        _wfopen_s(&m_file, archivePath.c_str(), L"rb");
    }
    ~CArchiveStream() {
        if (m_file) fclose(m_file);
    }

    bool ok() const { return m_file != nullptr; }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream)
            *ppv = static_cast<IStream*>(this);
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

    STDMETHOD(Read)(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (pcbRead) *pcbRead = 0;
        if (!m_file) return STG_E_INVALIDFUNCTION;
        if (cb == 0) return S_OK;
        uint64_t remaining = m_size - m_pos;
        if (remaining == 0) return S_FALSE;
        ULONG want = static_cast<ULONG>((uint64_t(cb) < remaining) ? cb : remaining);
        if (_fseeki64(m_file, static_cast<long long>(m_offset + m_pos), SEEK_SET) != 0)
            return STG_E_INVALIDFUNCTION;
        ULONG got = static_cast<ULONG>(fread(pv, 1, want, m_file));
        m_pos += got;
        if (pcbRead) *pcbRead = got;
        return S_OK;
    }

    STDMETHOD(Write)(const void*, ULONG, ULONG*) override {
        return STG_E_ACCESSDENIED;
    }

    STDMETHOD(Seek)(LARGE_INTEGER dlibMove, DWORD dwOrigin,
                    ULARGE_INTEGER* plibNewPosition) override {
        uint64_t newPos = m_pos;
        if (dwOrigin == STREAM_SEEK_SET) newPos = uint64_t(dlibMove.QuadPart);
        else if (dwOrigin == STREAM_SEEK_CUR) newPos = m_pos + uint64_t(dlibMove.QuadPart);
        else if (dwOrigin == STREAM_SEEK_END) newPos = m_size + uint64_t(dlibMove.QuadPart);
        else return STG_E_INVALIDFUNCTION;
        if (newPos > m_size) newPos = m_size;
        m_pos = newPos;
        if (plibNewPosition) plibNewPosition->QuadPart = LONGLONG(m_pos);
        return S_OK;
    }

    STDMETHOD(SetSize)(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }

    STDMETHOD(CopyTo)(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead,
                      ULARGE_INTEGER* pcbWritten) override {
        uint64_t remaining =
            cb.QuadPart < (m_size - m_pos) ? uint64_t(cb.QuadPart) : (m_size - m_pos);
        uint64_t done = 0;
        BYTE buf[65536];
        while (done < remaining) {
            ULONG want = static_cast<ULONG>(
                (uint64_t(sizeof(buf)) < remaining - done) ? sizeof(buf)
                                                           : remaining - done);
            ULONG got = 0;
            HRESULT hr = Read(buf, want, &got);
            if (FAILED(hr) || got == 0) break;
            ULONG written = 0;
            if (FAILED(pstm->Write(buf, got, &written))) break;
            done += written;
        }
        if (pcbRead) pcbRead->QuadPart = done;
        if (pcbWritten) pcbWritten->QuadPart = done;
        return S_OK;
    }

    STDMETHOD(Commit)(DWORD) override { return STG_E_INVALIDFUNCTION; }
    STDMETHOD(Revert)() override { return STG_E_INVALIDFUNCTION; }
    STDMETHOD(LockRegion)(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    STDMETHOD(UnlockRegion)(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    STDMETHOD(Stat)(STATSTG* pstatstg, DWORD) override {
        if (!pstatstg) return STG_E_INVALIDPOINTER;
        memset(pstatstg, 0, sizeof(*pstatstg));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = m_size;
        return S_OK;
    }
    STDMETHOD(Clone)(IStream**) override { return E_NOTIMPL; }

private:
    LONG m_refs = 1;
    FILE* m_file = nullptr;
    uint64_t m_offset = 0;
    uint64_t m_size = 0;
    uint64_t m_pos = 0;
};

// ---------------------------------------------------------------------------
// CAsarDataObject - CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS
// ---------------------------------------------------------------------------

struct Descriptor {
    std::wstring name;
    uint64_t absOffset = 0;  // absolute offset of the payload in the archive
    uint64_t size = 0;
    bool isDir = false;
};

class CAsarDataObject final : public IDataObject {
public:
    CAsarDataObject(const std::wstring& archivePath, const std::wstring& subPath,
                    UINT cidl, PCUITEMID_CHILD_ARRAY apidl) {
        m_archivePath = archivePath;
        try {
            // Shared cache (no re-parse): data objects are constructed
            // speculatively on hover/selection/right-click.
            std::shared_ptr<const asar::Header> header =
                asarcache::GetHeader(archivePath);
            for (UINT i = 0; i < cidl; ++i) {
                const AsarPidlItem* item = PidlAsItem(apidl[i]);
                if (!item) continue;
                std::wstring leaf = PidlItemName(apidl[i]);  // leaf name
                std::wstring full =
                    subPath.empty() ? leaf : subPath + L"/" + leaf;
                Descriptor d;
                d.name = leaf;
                d.isDir = item->isDir != 0;
                d.size = item->size;
                const asar::Node* node = header->root.Find(full);
                if (node && !node->isDir && !node->isLink) {
                    uint64_t off = 0;
                    for (char c : node->offset) off = off * 10 + (c - '0');
                    d.absOffset = header->dataStart + off;
                }
                m_items.push_back(d);
            }
        } catch (const std::exception&) {
            // leave items without offsets; contents will fail to stream
        } catch (...) {
        }
        m_fmtFileDescriptor.cfFormat =
            static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
        m_fmtFileContents.cfFormat =
            static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject)
            *ppv = static_cast<IDataObject*>(this);
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

    STDMETHOD(GetData)(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override {
        if (!pformatetcIn || !pmedium) return E_POINTER;
        pmedium->pUnkForRelease = nullptr;
        pmedium->tymed = 0;
        if (pformatetcIn->cfFormat == m_fmtFileDescriptor.cfFormat &&
            (pformatetcIn->tymed & TYMED_HGLOBAL)) {
            size_t n = m_items.size();
            size_t cb = sizeof(FILEGROUPDESCRIPTORW) +
                        (n ? n - 1 : 0) * sizeof(FILEDESCRIPTORW);
            HGLOBAL h = GlobalAlloc(GHND, cb);
            if (!h) return STG_E_MEDIUMFULL;
            auto* grp = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(h));
            if (!grp) {
                GlobalFree(h);
                return STG_E_MEDIUMFULL;
            }
            grp->cItems = static_cast<UINT>(n);
            for (UINT i = 0; i < grp->cItems; ++i) {
                FILEDESCRIPTORW& d = grp->fgd[i];
                memset(&d, 0, sizeof(d));
                d.dwFlags = FD_PROGRESSUI | FD_ATTRIBUTES;
                if (m_items[i].isDir) {
                    d.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                } else {
                    d.dwFlags |= FD_FILESIZE;
                    d.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
                    d.nFileSizeLow = static_cast<DWORD>(m_items[i].size & 0xFFFFFFFFu);
                    d.nFileSizeHigh = static_cast<DWORD>(m_items[i].size >> 32);
                }
                wcscpy_s(d.cFileName, m_items[i].name.c_str());
            }
            GlobalUnlock(h);
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = h;
            return S_OK;
        }
        if (pformatetcIn->cfFormat == m_fmtFileContents.cfFormat &&
            (pformatetcIn->tymed & TYMED_ISTREAM)) {
            LONG index = pformatetcIn->lindex;
            if (index < 0 || size_t(index) >= m_items.size() || m_items[size_t(index)].isDir)
                return DV_E_LINDEX;
            const Descriptor& d = m_items[size_t(index)];
            auto* stream = new (std::nothrow)
                CArchiveStream(m_archivePath, d.absOffset, d.size);
            if (!stream) return E_OUTOFMEMORY;
            if (!stream->ok()) {
                stream->Release();
                return STG_E_FILENOTFOUND;
            }
            pmedium->tymed = TYMED_ISTREAM;
            pmedium->pstm = stream;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    STDMETHOD(GetDataHere)(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }

    STDMETHOD(QueryGetData)(FORMATETC* pformatetc) override {
        if (!pformatetc) return E_POINTER;
        if (pformatetc->cfFormat == m_fmtFileDescriptor.cfFormat &&
            (pformatetc->tymed & TYMED_HGLOBAL))
            return S_OK;
        if (pformatetc->cfFormat == m_fmtFileContents.cfFormat &&
            (pformatetc->tymed & TYMED_ISTREAM))
            return S_OK;
        return DV_E_FORMATETC;
    }

    STDMETHOD(GetCanonicalFormatEtc)(FORMATETC* pformatectIn,
                                     FORMATETC* pformatetcOut) override {
        if (!pformatetcOut) return E_POINTER;
        *pformatetcOut = *pformatectIn;
        pformatetcOut->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    STDMETHOD(SetData)(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }

    STDMETHOD(EnumFormatEtc)(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override {
        if (!ppenumFormatEtc) return E_POINTER;
        if (dwDirection == DATADIR_SET) return E_NOTIMPL;
        FORMATETC fmts[2] = {};
        fmts[0].cfFormat = m_fmtFileDescriptor.cfFormat;
        fmts[0].dwAspect = DVASPECT_CONTENT;
        fmts[0].lindex = -1;
        fmts[0].tymed = TYMED_HGLOBAL;
        fmts[1].cfFormat = m_fmtFileContents.cfFormat;
        fmts[1].dwAspect = DVASPECT_CONTENT;
        fmts[1].lindex = -1;
        fmts[1].tymed = TYMED_ISTREAM;
        return SHCreateStdEnumFmtEtc(2, fmts, ppenumFormatEtc);
    }

    STDMETHOD(DAdvise)(FORMATETC*, DWORD, IAdviseSink*, DWORD* pdwConnection) override {
        if (pdwConnection) *pdwConnection = 0;
        return OLE_E_ADVISENOTSUPPORTED;
    }
    STDMETHOD(DUnadvise)(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHOD(EnumDAdvise)(IEnumSTATDATA** ppenumAdvise) override {
        if (ppenumAdvise) *ppenumAdvise = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    LONG m_refs = 1;
    std::wstring m_archivePath;
    std::vector<Descriptor> m_items;
    FORMATETC m_fmtFileDescriptor = {};
    FORMATETC m_fmtFileContents = {};
};

// ---------------------------------------------------------------------------
// CItemMenu - "Open" on selected items
// ---------------------------------------------------------------------------

class CItemMenu final : public IContextMenu, public IObjectWithSite {
public:
    CItemMenu(const std::wstring& archivePath, const std::wstring& subPath,
              PCIDLIST_ABSOLUTE parentPidl, UINT cidl,
              PCUITEMID_CHILD_ARRAY apidl)
        : m_archivePath(archivePath), m_subPath(subPath) {
        m_parent = parentPidl ? ILClone(parentPidl) : nullptr;
        for (UINT i = 0; i < cidl; ++i) {
            const AsarPidlItem* item = PidlAsItem(apidl[i]);
            if (!item) continue;
            std::wstring leaf = PidlItemName(apidl[i]);
            if (leaf.empty()) continue;
            Sel s;
            s.leaf = leaf;
            s.full = subPath.empty() ? leaf : subPath + L"/" + leaf;
            s.isDir = item->isDir != 0;
            s.size = item->size;
            size_t cb = apidl[i]->mkid.cb + sizeof(USHORT);
            auto* copy = static_cast<PITEMID_CHILD>(CoTaskMemAlloc(cb));
            if (copy) {
                memcpy(copy, apidl[i], cb);
                *reinterpret_cast<USHORT*>(reinterpret_cast<BYTE*>(copy) + cb -
                                           sizeof(USHORT)) = 0;
                s.pidl = copy;
            }
            if (!item->isDir) m_files.push_back(s.full);
            m_sel.push_back(s);
            if (item->isDir) m_hasDirs = true;
            else m_hasFiles = true;
        }
    }
    ~CItemMenu() {
        for (auto& s : m_sel)
            if (s.pidl) CoTaskMemFree(s.pidl);
        if (m_parent) ILFree(m_parent);
        if (m_site) m_site->Release();
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IContextMenu)
            *ppv = static_cast<IContextMenu*>(this);
        else if (riid == IID_IObjectWithSite)
            *ppv = static_cast<IObjectWithSite*>(this);
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

    // IObjectWithSite: defview sets the view site so folder Open can browse
    // the same window (like .zip) instead of opening a new one.
    STDMETHOD(SetSite)(IUnknown* punk) override {
        if (m_site) {
            m_site->Release();
            m_site = nullptr;
        }
        m_site = punk;
        if (m_site) m_site->AddRef();
        return S_OK;
    }
    STDMETHOD(GetSite)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (!m_site) {
            *ppv = nullptr;
            return E_FAIL;
        }
        return m_site->QueryInterface(riid, ppv);
    }

    STDMETHOD(QueryContextMenu)(HMENU hmenu, UINT indexMenu, UINT idCmdFirst,
                                UINT, UINT uFlags) override {
        if ((uFlags & CMF_DEFAULTONLY) || m_sel.empty())
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        UINT added = 0;
        // Open (default) for files and folders. Files extract to temp;
        // folders browse in place via the view site (fallback: new window).
        m_openId = 0xFFFFFFFF;
        if (!m_sel.empty()) {
            MENUITEMINFOW mii = {};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
            mii.dwTypeData = const_cast<LPWSTR>(L"&Open");
            mii.wID = idCmdFirst + added;
            mii.fState = MFS_ENABLED | MFS_DEFAULT;
            if (InsertMenuItemW(hmenu, indexMenu + added, TRUE, &mii)) {
                m_openId = added;
                ++added;
            }
        }
        // 1: Copy (files only; folder copy would be non-recursive).
        m_copyId = 0xFFFFFFFF;
        if (m_hasFiles) {
            MENUITEMINFOW mii = {};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
            mii.dwTypeData = const_cast<LPWSTR>(L"&Copy");
            mii.wID = idCmdFirst + added;
            mii.fState = MFS_ENABLED;
            if (InsertMenuItemW(hmenu, indexMenu + added, TRUE, &mii)) {
                m_copyId = added;
                ++added;
            }
        }
        // Separator + Properties.
        m_propId = 0xFFFFFFFF;
        {
            MENUITEMINFOW sep = {};
            sep.cbSize = sizeof(sep);
            sep.fMask = MIIM_TYPE | MIIM_ID;
            sep.fType = MFT_SEPARATOR;
            sep.wID = idCmdFirst + added;
            if (InsertMenuItemW(hmenu, indexMenu + added, TRUE, &sep)) ++added;
            MENUITEMINFOW mii = {};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
            mii.dwTypeData = const_cast<LPWSTR>(L"P&roperties");
            mii.wID = idCmdFirst + added;
            mii.fState = MFS_ENABLED;
            if (InsertMenuItemW(hmenu, indexMenu + added, TRUE, &mii)) {
                m_propId = added;
                ++added;
            }
        }
        m_total = added;
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, added);
    }

    STDMETHOD(InvokeCommand)(LPCMINVOKECOMMANDINFO pici) override {
        if (!pici) return E_POINTER;
        int id = -1;
        if (IS_INTRESOURCE(pici->lpVerb)) {
            id = LOWORD(reinterpret_cast<UINT_PTR>(pici->lpVerb));
        } else if (pici->lpVerb) {
            std::string v = pici->lpVerb;
            for (auto& c : v) c = (char)tolower((unsigned char)c);
            if (v == "open") id = 0;
            else if (v == "copy" && m_hasFiles) id = (int)m_copyId;
            else if (v == "properties") id = (int)m_propId;
        }
        if (id == (int)m_openId && m_openId != 0xFFFFFFFF)
            return OpenItems(pici->hwnd) ? S_OK : E_FAIL;
        if (id >= 0 && (UINT)id == m_copyId && m_hasFiles)
            return CopyFiles() ? S_OK : E_FAIL;
        if (id >= 0 && (UINT)id == m_propId) {
            ShowProperties(pici->hwnd);
            return S_OK;
        }
        return E_FAIL;
    }

    STDMETHOD(GetCommandString)(UINT_PTR idCmd, UINT uType, UINT*, CHAR* pszName,
                                UINT cchMax) override {
        if (cchMax == 0) return E_FAIL;
        const wchar_t* verb = nullptr;
        const wchar_t* help = nullptr;
        if (idCmd == m_openId && m_openId != 0xFFFFFFFF) {
            verb = L"open";
            help = L"Opens this item from the archive";
        } else if (idCmd == m_copyId && m_hasFiles) {
            verb = L"copy";
            help = L"Copies this item to the clipboard";
        } else if (idCmd == m_propId) {
            verb = L"properties";
            help = L"Shows properties for this item";
        } else {
            return E_INVALIDARG;
        }
        if (uType == GCS_VERBW) {
            lstrcpynW(reinterpret_cast<LPWSTR>(pszName), verb, cchMax);
            return S_OK;
        }
        if (uType == GCS_HELPTEXTW) {
            lstrcpynW(reinterpret_cast<LPWSTR>(pszName), help, cchMax);
            return S_OK;
        }
        return E_NOTIMPL;
    }

private:
    struct Sel {
        std::wstring leaf;
        std::wstring full;
        bool isDir = false;
        uint64_t size = 0;
        PITEMID_CHILD pidl = nullptr;
    };

    // Browse a subfolder in the SAME window via the view site (like .zip).
    // Falls back to a new window when no site/browser is available
    // (e.g., automation without a view).
    bool BrowseToFolder(PCUIDLIST_RELATIVE rel) {
        PIDLIST_ABSOLUTE abs = nullptr;
        if (m_parent && rel) abs = ILCombine(m_parent, rel);
        if (!abs) return false;
        bool ok = false;
        if (m_site) {
            IServiceProvider* sp = nullptr;
            if (SUCCEEDED(m_site->QueryInterface(IID_IServiceProvider,
                                                 reinterpret_cast<void**>(&sp))) &&
                sp) {
                IShellBrowser* br = nullptr;
                HRESULT hrTop = sp->QueryService(SID_STopLevelBrowser,
                                                 IID_IShellBrowser,
                                                 reinterpret_cast<void**>(&br));
                if (SUCCEEDED(hrTop) && br) {
                    HRESULT hrB = br->BrowseObject(abs, SBSP_SAMEBROWSER);
                    if (SUCCEEDED(hrB)) ok = true;
                    br->Release();
                    br = nullptr;
                }
                if (!ok) {
                    HRESULT hrSh = sp->QueryService(
                        SID_SShellBrowser, IID_IShellBrowser,
                        reinterpret_cast<void**>(&br));
                    if (SUCCEEDED(hrSh) && br) {
                        HRESULT hrB = br->BrowseObject(abs, SBSP_SAMEBROWSER);
                        if (SUCCEEDED(hrB)) ok = true;
                        br->Release();
                    }
                }
                sp->Release();
            }
        }
        if (!ok) {
            if (SUCCEEDED(SHOpenFolderAndSelectItems(abs, 0, nullptr, 0))) ok = true;
        }
        ILFree(abs);
        return ok;
    }

    bool OpenItems(HWND hwnd) {
        if (m_sel.empty()) return false;
        bool did = false;
        try {
            std::unique_ptr<asar::Archive> archive;
            if (m_hasFiles) archive = asar::Archive::Open(m_archivePath);
            wchar_t temp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, temp);
            std::wstring stem =
                std::filesystem::path(m_archivePath).stem().wstring();
            for (const Sel& s : m_sel) {
                if (s.isDir) {
                    if (s.pidl &&
                        BrowseToFolder(
                            reinterpret_cast<PCUIDLIST_RELATIVE>(s.pidl)))
                        did = true;
                    continue;
                }
                std::vector<uint8_t> data = archive->ReadFile(s.full);
                std::filesystem::path out = std::filesystem::path(temp) /
                                            L"asarfolder" / stem /
                                            std::filesystem::path(s.full);
                std::error_code ec;
                std::filesystem::create_directories(out.parent_path(), ec);
                FILE* f = nullptr;
                if (_wfopen_s(&f, out.c_str(), L"wb") == 0 && f) {
                    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
                    fclose(f);
                    ShellExecuteW(hwnd, L"open", out.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                    did = true;
                }
            }
            return did;
        } catch (const std::exception&) {
            return did;
        }
    }

    bool CopyFiles() {
        std::vector<PCUITEMID_CHILD> files;
        for (const Sel& s : m_sel) {
            if (!s.isDir && s.pidl) files.push_back(s.pidl);
        }
        if (files.empty()) return false;
        IDataObject* data = nullptr;
        HRESULT hr = CreateItemDataObject(
            m_archivePath, m_subPath, (UINT)files.size(), files.data(),
            IID_IDataObject, reinterpret_cast<void**>(&data));
        if (FAILED(hr) || !data) return false;
        hr = OleSetClipboard(data);
        data->Release();
        return SUCCEEDED(hr);
    }

    void ShowProperties(HWND hwnd) {
        std::wstring text;
        if (m_sel.size() == 1) {
            const Sel& s = m_sel[0];
            wchar_t sz[64] = {};
            if (!s.isDir)
                swprintf_s(sz, L"%llu bytes", (unsigned long long)s.size);
            else
                wcscpy_s(sz, L"File folder");
            text = L"Name: " + s.leaf + L"\r\nType: " +
                   (s.isDir ? L"File folder"
                            : std::wstring(L"File (") + ExtensionOf(s.leaf) +
                                  L")") +
                   L"\r\nSize: " + sz + L"\r\nLocation: " + m_archivePath;
        } else {
            size_t nF = 0, nD = 0;
            for (const Sel& s : m_sel) (s.isDir ? nD : nF)++;
            wchar_t buf[128] = {};
            swprintf_s(buf, L"%zu file(s), %zu folder(s) selected.", nF, nD);
            text = buf;
        }
        MessageBoxW(hwnd ? hwnd : nullptr, text.c_str(), L"ASAR Properties",
                    MB_OK | MB_ICONINFORMATION);
    }

    static std::wstring ExtensionOf(const std::wstring& leaf) {
        size_t dot = leaf.find_last_of(L'.');
        if (dot == std::wstring::npos || dot + 1 >= leaf.size())
            return L"File";
        return leaf.substr(dot + 1) + L" File";
    }

    LONG m_refs = 1;
    std::wstring m_archivePath;
    std::wstring m_subPath;
    PIDLIST_ABSOLUTE m_parent = nullptr;
    IUnknown* m_site = nullptr;
    std::vector<Sel> m_sel;
    std::vector<std::wstring> m_files;
    bool m_hasFiles = false;
    bool m_hasDirs = false;
    UINT m_copyId = 0xFFFFFFFF;
    UINT m_propId = 0xFFFFFFFF;
    UINT m_openId = 0xFFFFFFFF;
    UINT m_total = 0;
};

// ---------------------------------------------------------------------------
// CItemPropStore - IPropertyStore for System.Size / System.ItemNameDisplay.
// Explorer's details view and FolderItem.Size use the property system, not
// IShellFolder2::GetDetailsOf, so without this sizes show as 0.
// ---------------------------------------------------------------------------

class CItemPropStore final : public IPropertyStore {
public:
    CItemPropStore(const std::wstring& leaf, bool isDir, uint64_t size)
        : m_leaf(leaf), m_isDir(isDir), m_size(size) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPropertyStore)
            *ppv = static_cast<IPropertyStore*>(this);
        else {
            *ppv = nullptr;
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

    STDMETHOD(GetCount)(DWORD* pcProps) override {
        if (!pcProps) return E_POINTER;
        *pcProps = m_isDir ? 1 : 2;  // Name (+ Size for files)
        return S_OK;
    }
    STDMETHOD(GetAt)(DWORD iProp, PROPERTYKEY* pkey) override {
        if (!pkey) return E_POINTER;
        if (iProp == 0) {
            *pkey = PKEY_ItemNameDisplay;
            return S_OK;
        }
        if (iProp == 1 && !m_isDir) {
            *pkey = PKEY_Size;
            return S_OK;
        }
        return E_INVALIDARG;
    }
    STDMETHOD(GetValue)(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        if (!pv) return E_POINTER;
        memset(pv, 0, sizeof(*pv));
        if (IsEqualPropertyKey(key, PKEY_Size)) {
            if (m_isDir) return E_NOTIMPL;
            pv->vt = VT_UI8;
            pv->uhVal.QuadPart = m_size;
            return S_OK;
        }
        if (IsEqualPropertyKey(key, PKEY_ItemNameDisplay)) {
            size_t cb = (m_leaf.size() + 1) * sizeof(wchar_t);
            wchar_t* out =
                static_cast<wchar_t*>(CoTaskMemAlloc(cb));
            if (!out) return E_OUTOFMEMORY;
            memcpy(out, m_leaf.c_str(), cb);
            pv->vt = VT_LPWSTR;
            pv->pwszVal = out;
            return S_OK;
        }
        return E_NOTIMPL;
    }
    STDMETHOD(SetValue)(REFPROPERTYKEY, REFPROPVARIANT) override {
        return STG_E_ACCESSDENIED;  // read-only archive
    }
    STDMETHOD(Commit)() override { return S_OK; }

private:
    LONG m_refs = 1;
    std::wstring m_leaf;
    bool m_isDir = false;
    uint64_t m_size = 0;
};

// ---------------------------------------------------------------------------
// CCompositeMenu - merges our custom "&Open" with the system default menu
// (Cut/Copy/Properties/etc. via SHCreateDefaultContextMenu). Without this,
// returning only our minimal menu suppresses all standard verbs: folders
// showed an empty menu and files showed only Open.
// ---------------------------------------------------------------------------

class CCompositeMenu final : public IContextMenu {
public:
    CCompositeMenu(IContextMenu* custom, IContextMenu* def)
        : m_custom(custom), m_def(def) {
        if (m_custom) m_custom->AddRef();
        if (m_def) m_def->AddRef();
    }
    ~CCompositeMenu() {
        if (m_custom) m_custom->Release();
        if (m_def) m_def->Release();
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IContextMenu)
            *ppv = static_cast<IContextMenu*>(this);
        else {
            *ppv = nullptr;
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

    STDMETHOD(QueryContextMenu)(HMENU hmenu, UINT indexMenu, UINT idCmdFirst,
                                UINT idCmdLast, UINT uFlags) override {
        m_customCount = 0;
        m_defCount = 0;
        UINT nextIndex = indexMenu;
        UINT nextId = idCmdFirst;
        if (m_custom) {
            HRESULT hr = m_custom->QueryContextMenu(hmenu, nextIndex, nextId,
                                                    idCmdLast, uFlags);
            if (SUCCEEDED(hr)) {
                UINT added = (UINT)HRESULT_CODE(hr);
                m_customCount = added;
                nextIndex += added;
                nextId += added;
            }
        }
        if (m_def && nextId <= idCmdLast) {
            HRESULT hr =
                m_def->QueryContextMenu(hmenu, nextIndex, nextId, idCmdLast, uFlags);
            if (SUCCEEDED(hr)) {
                m_defCount = (UINT)HRESULT_CODE(hr);
            } else {
                // Defaults unavailable; custom items still stand.
                return MAKE_HRESULT(SEVERITY_SUCCESS, 0, m_customCount);
            }
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, m_customCount + m_defCount);
    }

    STDMETHOD(InvokeCommand)(LPCMINVOKECOMMANDINFO pici) override {
        if (!pici) return E_POINTER;
        // String verbs ("open", "copy", ...): try custom first, then default.
        if (!IS_INTRESOURCE(pici->lpVerb)) {
            if (m_custom) {
                HRESULT hr = m_custom->InvokeCommand(pici);
                if (SUCCEEDED(hr)) return hr;
            }
            if (m_def) return m_def->InvokeCommand(pici);
            return E_FAIL;
        }
        int id = LOWORD(reinterpret_cast<UINT_PTR>(pici->lpVerb));
        if ((UINT)id < m_customCount) {
            return m_custom ? m_custom->InvokeCommand(pici) : E_FAIL;
        }
        if (!m_def) return E_FAIL;
        // Shift the id down for the default menu.
        int defId = id - (int)m_customCount;
        // Copy the struct (handle EX version) and rewrite the verb id.
        BYTE buf[sizeof(CMINVOKECOMMANDINFOEX)] = {};
        size_t copySize = pici->cbSize < sizeof(buf) ? pici->cbSize : sizeof(buf);
        memcpy(buf, pici, copySize);
        auto* adj = reinterpret_cast<CMINVOKECOMMANDINFOEX*>(buf);
        adj->lpVerb = MAKEINTRESOURCEA(defId);
        // Keep cbSize as the original to avoid over-read by the callee.
        adj->cbSize = (UINT)copySize;
        return m_def->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(adj));
    }

    STDMETHOD(GetCommandString)(UINT_PTR idCmd, UINT uType, UINT* reserved,
                                CHAR* pszName, UINT cchMax) override {
        if (idCmd < m_customCount) {
            return m_custom ? m_custom->GetCommandString(idCmd, uType, reserved,
                                                         pszName, cchMax)
                            : E_INVALIDARG;
        }
        if (!m_def) return E_INVALIDARG;
        return m_def->GetCommandString(idCmd - m_customCount, uType, reserved,
                                       pszName, cchMax);
    }

private:
    LONG m_refs = 1;
    IContextMenu* m_custom = nullptr;
    IContextMenu* m_def = nullptr;
    UINT m_customCount = 0;
    UINT m_defCount = 0;
};

// ---------------------------------------------------------------------------
// Modern (Win11) context menu: IEnumExplorerCommand / IExplorerCommand.
// The classic IContextMenu above surfaces under "Show more options"; these
// commands render in the top-level modern menu. No extra registration is
// needed: the shell queries IID_IEnumExplorerCommand via GetUIObjectOf.
// Read-only archive: Open (default, Enter), Open in new window (folders),
// Copy (files), Copy as path, Properties (Alt+Enter). Cut/Rename/Delete are
// omitted (no in-place modification, like the classic menu).
// ---------------------------------------------------------------------------

struct CmdItem {
    std::wstring leaf;
    std::wstring full;  // in-archive full path with '/'
    bool isDir = false;
    uint64_t size = 0;
    PITEMID_CHILD pidl = nullptr;  // relative single-item copy
};

enum class CmdKind { Open, OpenNewWindow, Copy, CopyAsPath, Properties };

class CAsarCommand final : public IExplorerCommand, public IObjectWithSite {
public:
    CAsarCommand(CmdKind kind, const std::wstring& archivePath,
                 const std::wstring& subPath, PCIDLIST_ABSOLUTE parentAbs,
                 const std::vector<CmdItem>& items)
        : m_kind(kind),
          m_archivePath(archivePath),
          m_subPath(subPath),
          m_hasFiles(false),
          m_hasDirs(false) {
        m_parent = parentAbs ? ILClone(parentAbs) : nullptr;
        for (const CmdItem& s : items) {
            CmdItem c = s;
            c.pidl = nullptr;
            if (s.pidl) {
                size_t cb = s.pidl->mkid.cb + sizeof(USHORT);
                auto* copy = static_cast<PITEMID_CHILD>(CoTaskMemAlloc(cb));
                if (copy) {
                    memcpy(copy, s.pidl, cb);
                    c.pidl = copy;
                }
            }
            if (!c.isDir) m_hasFiles = true;
            else m_hasDirs = true;
            m_items.push_back(c);
        }
    }
    ~CAsarCommand() {
        for (auto& s : m_items)
            if (s.pidl) CoTaskMemFree(s.pidl);
        if (m_parent) ILFree(m_parent);
        if (m_site) m_site->Release();
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IExplorerCommand)
            *ppv = static_cast<IExplorerCommand*>(this);
        else if (riid == IID_IObjectWithSite)
            *ppv = static_cast<IObjectWithSite*>(this);
        else {
            *ppv = nullptr;
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

    STDMETHOD(SetSite)(IUnknown* punk) override {
        if (m_site) {
            m_site->Release();
            m_site = nullptr;
        }
        m_site = punk;
        if (m_site) m_site->AddRef();
        return S_OK;
    }
    STDMETHOD(GetSite)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (!m_site) {
            *ppv = nullptr;
            return E_FAIL;
        }
        return m_site->QueryInterface(riid, ppv);
    }

    STDMETHOD(GetTitle)(IShellItemArray*, LPWSTR* ppszName) override {
        if (!ppszName) return E_POINTER;
        const wchar_t* t = L"";
        switch (m_kind) {
            case CmdKind::Open: t = L"Open"; break;
            case CmdKind::OpenNewWindow: t = L"Open in new window"; break;
            case CmdKind::Copy: t = L"Copy"; break;
            case CmdKind::CopyAsPath: t = L"Copy as path"; break;
            case CmdKind::Properties: t = L"Properties"; break;
        }
        return SHStrDupW(t, ppszName);
    }
    STDMETHOD(GetIcon)(IShellItemArray*, LPWSTR*) override {
        // Let the shell resolve item/canonical glyphs.
        return E_NOTIMPL;
    }
    STDMETHOD(GetToolTip)(IShellItemArray*, LPWSTR* ppszTip) override {
        if (!ppszTip) return E_POINTER;
        const wchar_t* t = L"";
        switch (m_kind) {
            case CmdKind::Open: t = L"Open this item from the archive"; break;
            case CmdKind::OpenNewWindow: t = L"Open this folder in a new window"; break;
            case CmdKind::Copy: t = L"Copy this item to the clipboard"; break;
            case CmdKind::CopyAsPath: t = L"Copy the item path as text"; break;
            case CmdKind::Properties: t = L"Show properties for this item"; break;
        }
        return SHStrDupW(t, ppszTip);
    }
    STDMETHOD(GetCanonicalName)(GUID*) override { return E_NOTIMPL; }
    STDMETHOD(GetState)(IShellItemArray*, BOOL, EXPCMDSTATE* pState) override {
        if (!pState) return E_POINTER;
        switch (m_kind) {
            case CmdKind::Open:
                *pState = m_items.empty() ? ECS_HIDDEN : ECS_ENABLED;
                return S_OK;
            case CmdKind::OpenNewWindow:
                *pState = m_hasDirs ? ECS_ENABLED : ECS_HIDDEN;
                return S_OK;
            case CmdKind::Copy:
                *pState = m_hasFiles ? ECS_ENABLED : ECS_HIDDEN;
                return S_OK;
            case CmdKind::CopyAsPath:
            case CmdKind::Properties:
                *pState = m_items.empty() ? ECS_HIDDEN : ECS_ENABLED;
                return S_OK;
        }
        *pState = ECS_HIDDEN;
        return S_OK;
    }
    STDMETHOD(Invoke)(IShellItemArray*, IBindCtx*) override {
        switch (m_kind) {
            case CmdKind::Open: return DoOpen(false) ? S_OK : E_FAIL;
            case CmdKind::OpenNewWindow: return DoOpenNewWindow() ? S_OK : E_FAIL;
            case CmdKind::Copy: return DoCopy() ? S_OK : E_FAIL;
            case CmdKind::CopyAsPath: return DoCopyAsPath() ? S_OK : E_FAIL;
            case CmdKind::Properties:
                DoProperties(nullptr);
                return S_OK;
        }
        return E_FAIL;
    }
    STDMETHOD(GetFlags)(EXPCMDFLAGS* pFlags) override {
        if (!pFlags) return E_POINTER;
        *pFlags = (m_kind == CmdKind::Open)               ? (EXPCMDFLAGS)ECF_DEFAULT
                  : (m_kind == CmdKind::Properties)      ? (EXPCMDFLAGS)ECF_SEPARATORBEFORE
                                                         : (EXPCMDFLAGS)0;
        return S_OK;
    }
    STDMETHOD(EnumSubCommands)(IEnumExplorerCommand**) override { return E_NOTIMPL; }

private:
    bool BrowseSameTab(PCUIDLIST_RELATIVE rel) {
        PIDLIST_ABSOLUTE abs = nullptr;
        if (m_parent && rel) abs = ILCombine(m_parent, rel);
        if (!abs) return false;
        bool ok = false;
        if (m_site) {
            IServiceProvider* sp = nullptr;
            if (SUCCEEDED(m_site->QueryInterface(
                    IID_IServiceProvider, reinterpret_cast<void**>(&sp))) &&
                sp) {
                IShellBrowser* br = nullptr;
                if (SUCCEEDED(sp->QueryService(
                        SID_STopLevelBrowser, IID_IShellBrowser,
                        reinterpret_cast<void**>(&br))) &&
                    br) {
                    if (SUCCEEDED(br->BrowseObject(abs, SBSP_SAMEBROWSER))) ok = true;
                    br->Release();
                } else if (SUCCEEDED(sp->QueryService(
                               SID_SShellBrowser, IID_IShellBrowser,
                               reinterpret_cast<void**>(&br))) &&
                           br) {
                    if (SUCCEEDED(br->BrowseObject(abs, SBSP_SAMEBROWSER))) ok = true;
                    br->Release();
                }
                sp->Release();
            }
        }
        ILFree(abs);
        return ok;
    }

    bool DoOpen(bool newWindow) {
        if (m_items.empty()) return false;
        bool did = false;
        try {
            std::unique_ptr<asar::Archive> archive;
            if (m_hasFiles) archive = asar::Archive::Open(m_archivePath);
            wchar_t temp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, temp);
            std::wstring stem =
                std::filesystem::path(m_archivePath).stem().wstring();
            for (const CmdItem& s : m_items) {
                if (s.isDir) {
                    if (!s.pidl) continue;
                    if (!newWindow) {
                        if (BrowseSameTab(
                                reinterpret_cast<PCUIDLIST_RELATIVE>(s.pidl)))
                            did = true;
                        else if (m_parent) {
                            PIDLIST_ABSOLUTE abs = ILCombine(
                                m_parent,
                                reinterpret_cast<PCUIDLIST_RELATIVE>(s.pidl));
                            if (abs) {
                                if (SUCCEEDED(SHOpenFolderAndSelectItems(abs, 0, nullptr, 0)))
                                    did = true;
                                ILFree(abs);
                            }
                        }
                    } else if (m_parent) {
                        PIDLIST_ABSOLUTE abs = ILCombine(
                            m_parent,
                            reinterpret_cast<PCUIDLIST_RELATIVE>(s.pidl));
                        if (abs) {
                            if (SUCCEEDED(SHOpenFolderAndSelectItems(abs, 0, nullptr, 0)))
                                did = true;
                            ILFree(abs);
                        }
                    }
                    continue;
                }
                if (!archive) continue;
                std::vector<uint8_t> data = archive->ReadFile(s.full);
                std::filesystem::path out = std::filesystem::path(temp) /
                                            L"asarfolder" / stem /
                                            std::filesystem::path(s.full);
                std::error_code ec;
                std::filesystem::create_directories(out.parent_path(), ec);
                FILE* f = nullptr;
                if (_wfopen_s(&f, out.c_str(), L"wb") == 0 && f) {
                    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
                    fclose(f);
                    ShellExecuteW(nullptr, L"open", out.c_str(), nullptr,
                                  nullptr, SW_SHOWNORMAL);
                    did = true;
                }
            }
            return did;
        } catch (const std::exception&) {
            return did;
        }
    }

    bool DoOpenNewWindow() {
        if (!m_hasDirs) return false;
        bool did = false;
        for (const CmdItem& s : m_items) {
            if (!s.isDir || !s.pidl || !m_parent) continue;
            PIDLIST_ABSOLUTE abs = ILCombine(
                m_parent, reinterpret_cast<PCUIDLIST_RELATIVE>(s.pidl));
            if (!abs) continue;
            if (SUCCEEDED(SHOpenFolderAndSelectItems(abs, 0, nullptr, 0))) did = true;
            ILFree(abs);
        }
        return did;
    }

    bool DoCopy() {
        std::vector<PCUITEMID_CHILD> files;
        for (const CmdItem& s : m_items) {
            if (!s.isDir && s.pidl) files.push_back(s.pidl);
        }
        if (files.empty()) return false;
        IDataObject* data = nullptr;
        HRESULT hr = CreateItemDataObject(
            m_archivePath, m_subPath, (UINT)files.size(), files.data(),
            IID_IDataObject, reinterpret_cast<void**>(&data));
        if (FAILED(hr) || !data) return false;
        hr = OleSetClipboard(data);
        data->Release();
        return SUCCEEDED(hr);
    }

    bool DoCopyAsPath() {
        if (m_items.empty()) return false;
        std::wstring text;
        for (const CmdItem& s : m_items) {
            std::wstring rel = s.full;
            std::replace(rel.begin(), rel.end(), L'/', L'\\');
            if (!text.empty()) text += L"\r\n";
            text += m_archivePath + L"\\" + rel;
        }
        size_t cb = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, cb);
        if (!h) return false;
        void* p = GlobalLock(h);
        if (!p) {
            GlobalFree(h);
            return false;
        }
        memcpy(p, text.c_str(), cb);
        GlobalUnlock(h);
        if (!OpenClipboard(nullptr)) {
            GlobalFree(h);
            return false;
        }
        EmptyClipboard();
        bool ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        CloseClipboard();
        if (!ok) GlobalFree(h);
        return ok;
    }

    void DoProperties(HWND hwnd) {
        std::wstring text;
        if (m_items.size() == 1) {
            const CmdItem& s = m_items[0];
            wchar_t sz[64] = {};
            if (!s.isDir)
                swprintf_s(sz, L"%llu bytes", (unsigned long long)s.size);
            else
                wcscpy_s(sz, L"File folder");
            size_t dot = s.leaf.find_last_of(L'.');
            std::wstring type = s.isDir ? L"File folder"
                              : (dot == std::wstring::npos)
                                  ? L"File"
                                  : s.leaf.substr(dot + 1) + L" File";
            text = L"Name: " + s.leaf + L"\r\nType: " + type +
                   L"\r\nSize: " + sz + L"\r\nLocation: " + m_archivePath;
        } else {
            size_t nF = 0, nD = 0;
            for (const CmdItem& s : m_items) (s.isDir ? nD : nF)++;
            wchar_t buf[128] = {};
            swprintf_s(buf, L"%zu file(s), %zu folder(s) selected.", nF, nD);
            text = buf;
        }
        MessageBoxW(hwnd ? hwnd : nullptr, text.c_str(), L"ASAR Properties",
                    MB_OK | MB_ICONINFORMATION);
    }

    LONG m_refs = 1;
    CmdKind m_kind = CmdKind::Open;
    std::wstring m_archivePath;
    std::wstring m_subPath;
    PIDLIST_ABSOLUTE m_parent = nullptr;
    IUnknown* m_site = nullptr;
    std::vector<CmdItem> m_items;
    bool m_hasFiles = false;
    bool m_hasDirs = false;
};

class CAsarEnumCommands final : public IEnumExplorerCommand,
                                 public IObjectWithSite {
public:
    CAsarEnumCommands(const std::wstring& archivePath,
                      const std::wstring& subPath, PCIDLIST_ABSOLUTE parentAbs,
                      const std::vector<CmdItem>& items) {
        bool hasFiles = false, hasDirs = false;
        for (const CmdItem& s : items) {
            if (s.isDir) hasDirs = true;
            else hasFiles = true;
        }
        m_kinds.push_back(CmdKind::Open);
        if (hasDirs) m_kinds.push_back(CmdKind::OpenNewWindow);
        if (hasFiles) m_kinds.push_back(CmdKind::Copy);
        m_kinds.push_back(CmdKind::CopyAsPath);
        m_kinds.push_back(CmdKind::Properties);
        for (CmdKind k : m_kinds) {
            auto* cmd = new (std::nothrow)
                CAsarCommand(k, archivePath, subPath, parentAbs, items);
            if (cmd) m_cmds.push_back(cmd);
            else break;
        }
    }
    ~CAsarEnumCommands() {
        for (auto* c : m_cmds) c->Release();
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumExplorerCommand)
            *ppv = static_cast<IEnumExplorerCommand*>(this);
        else if (riid == IID_IObjectWithSite)
            *ppv = static_cast<IObjectWithSite*>(this);
        else {
            *ppv = nullptr;
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

    STDMETHOD(SetSite)(IUnknown* punk) override {
        for (auto* c : m_cmds) c->SetSite(punk);
        return S_OK;
    }
    STDMETHOD(GetSite)(REFIID, void**) override { return E_NOTIMPL; }

    STDMETHOD(Next)(ULONG celt, IExplorerCommand** rgelt,
                    ULONG* pceltFetched) override {
        if (pceltFetched) *pceltFetched = 0;
        ULONG fetched = 0;
        while (fetched < celt && m_index < m_cmds.size()) {
            rgelt[fetched] = m_cmds[m_index];
            rgelt[fetched]->AddRef();
            ++fetched;
            ++m_index;
        }
        if (pceltFetched) *pceltFetched = fetched;
        return fetched == celt ? S_OK : S_FALSE;
    }
    STDMETHOD(Skip)(ULONG celt) override {
        m_index += celt;
        if (m_index > m_cmds.size()) m_index = m_cmds.size();
        return S_OK;
    }
    STDMETHOD(Reset)() override {
        m_index = 0;
        return S_OK;
    }
    STDMETHOD(Clone)(IEnumExplorerCommand**) override { return E_NOTIMPL; }

private:
    LONG m_refs = 1;
    std::vector<CmdKind> m_kinds;
    std::vector<CAsarCommand*> m_cmds;
    size_t m_index = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

HRESULT CreateItemIcon(HWND, UINT cidl, PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                       void** ppvOut) {
    if (cidl < 1) return E_INVALIDARG;
    const AsarPidlItem* item = PidlAsItem(apidl[0]);
    if (!item) return E_INVALIDARG;
    auto* icon = new (std::nothrow) CItemIcon(PidlItemName(apidl[0]), item->isDir != 0);
    if (!icon) return E_OUTOFMEMORY;
    HRESULT hr = icon->QueryInterface(riid, ppvOut);
    icon->Release();
    return hr;
}

HRESULT CreateItemDataObject(const std::wstring& archivePath,
                             const std::wstring& subPath, UINT cidl,
                             PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                             void** ppvOut) {
    auto* data =
        new (std::nothrow) CAsarDataObject(archivePath, subPath, cidl, apidl);
    if (!data) return E_OUTOFMEMORY;
    HRESULT hr = data->QueryInterface(riid, ppvOut);
    data->Release();
    return hr;
}

HRESULT CreateItemMenu(const std::wstring& archivePath,
                       const std::wstring& subPath, PCIDLIST_ABSOLUTE parentPidl,
                       UINT cidl, PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                       void** ppvOut) {
    auto* menu =
        new (std::nothrow) CItemMenu(archivePath, subPath, parentPidl, cidl, apidl);
    if (!menu) return E_OUTOFMEMORY;
    HRESULT hr = menu->QueryInterface(riid, ppvOut);
    menu->Release();
    return hr;
}

HRESULT CreateItemPropertyStore(UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                REFIID riid, void** ppvOut) {
    if (cidl != 1 || !apidl) return E_INVALIDARG;
    const AsarPidlItem* item = PidlAsItem(apidl[0]);
    if (!item) return E_INVALIDARG;
    auto* store = new (std::nothrow) CItemPropStore(
        PidlItemName(apidl[0]), item->isDir != 0, item->size);
    if (!store) return E_OUTOFMEMORY;
    HRESULT hr = store->QueryInterface(riid, ppvOut);
    store->Release();
    return hr;
}

HRESULT CreateCompositeMenu(IContextMenu* custom, IContextMenu* def,
                            REFIID riid, void** ppvOut) {
    auto* merged = new (std::nothrow) CCompositeMenu(custom, def);
    if (!merged) return E_OUTOFMEMORY;
    HRESULT hr = merged->QueryInterface(riid, ppvOut);
    merged->Release();
    return hr;
}

HRESULT CreateItemExplorerCommands(const std::wstring& archivePath,
                                   const std::wstring& subPath,
                                   PCIDLIST_ABSOLUTE parentPidl, UINT cidl,
                                   PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                                   void** ppvOut) {
    std::vector<CmdItem> items;
    for (UINT i = 0; i < cidl; ++i) {
        const AsarPidlItem* item = PidlAsItem(apidl[i]);
        if (!item) continue;
        std::wstring leaf = PidlItemName(apidl[i]);
        if (leaf.empty()) continue;
        CmdItem c;
        c.leaf = leaf;
        c.full = subPath.empty() ? leaf : subPath + L"/" + leaf;
        c.isDir = item->isDir != 0;
        c.size = item->size;
        size_t cb = apidl[i]->mkid.cb + sizeof(USHORT);
        auto* copy = static_cast<PITEMID_CHILD>(CoTaskMemAlloc(cb));
        if (copy) {
            memcpy(copy, apidl[i], cb);
            c.pidl = copy;
        }
        items.push_back(c);
    }
    if (items.empty()) return E_INVALIDARG;
    auto* e = new (std::nothrow) CAsarEnumCommands(archivePath, subPath,
                                                   parentPidl, items);
    for (auto& c : items)
        if (c.pidl) CoTaskMemFree(c.pidl);
    if (!e) return E_OUTOFMEMORY;
    HRESULT hr = e->QueryInterface(riid, ppvOut);
    e->Release();
    return hr;
}
