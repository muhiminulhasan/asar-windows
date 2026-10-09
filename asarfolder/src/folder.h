// folder.h - asarfolder.dll: shell namespace extension that makes .asar
// files browsable inside Windows Explorer, exactly like .zip files.
//
// The mechanism mirrors Windows' own zipfldr.dll:
//   ProgID\ShellEx\StorageHandler = {our CLSID}     <- Explorer binds us
//   ProgID\CLSID                   = {our CLSID}
//   ProgID\Shell\Open\Command      = Explorer.exe /idlist,%I,%L
//         DelegateExecute           = {11dbb47c-a525-400b-9e80-a54615a090c0}
//   CLSID\{us}\ShellFolder\Attributes = SFGAO_FOLDER|...
//
// The object implements IShellFolder over the archive contents. Child item
// PIDLs use a private layout (AsarPidlItem) appended after the .asar file's
// own PIDL.

#pragma once

#include <windows.h>
#include <shlobj.h>

#include <asar/asar.h>

#include <memory>
#include <string>
#include <vector>

// {8AF8F867-E3EA-40B8-8A87-942ED2A3DF0D}
static const GUID CLSID_AsarFolder = {
    0x8AF8F867, 0xE3EA, 0x40B8, {0x8A, 0x87, 0x94, 0x2E, 0xD2, 0xA3, 0xDF, 0x0D}};

constexpr DWORD kPidlMagic = 0x52415341;  // 'ASAR'

// Private PIDL item layout (relative child PIDL of our folder objects).
struct AsarPidlItem {
    USHORT cb;          // total size, including name and terminator
    DWORD magic;        // kPidlMagic
    DWORD isDir;        // 0 = file, 1 = directory
    ULONGLONG size;     // payload size (files)
    WCHAR name[1];      // null-terminated; continues past the struct tail
};

inline const AsarPidlItem* PidlAsItem(PCUITEMID_CHILD pidl) {
    if (!pidl || pidl->mkid.cb < sizeof(AsarPidlItem)) return nullptr;
    auto* item = reinterpret_cast<const AsarPidlItem*>(pidl);
    if (item->magic != kPidlMagic) return nullptr;
    return item;
}

inline std::wstring PidlItemName(PCUITEMID_CHILD pidl) {
    const AsarPidlItem* item = PidlAsItem(pidl);
    if (!item) return {};
    return std::wstring(item->name);
}

// Allocates a child PIDL for an entry (caller frees with CoTaskMemFree).
// The PIDL stores the entry's FULL in-archive path; display names strip to
// the leaf. Keeping the full path lets streams/menus resolve any entry.
PIDLIST_RELATIVE MakeChildPidl(const asar::Entry& entry);

// Splits an absolute pidl into the .asar file path (prefix part) and the
// in-archive sub path (our trailing items). Returns false when the pidl has
// no filesystem prefix.
bool SplitArchivePidl(PCIDLIST_ABSOLUTE pidl, std::wstring& archivePath,
                      std::wstring& subPath);

// Registry bookkeeping (reg.cpp).
HRESULT RegisterAsarFolder(void);
HRESULT UnregisterAsarFolder(void);

// ---------------------------------------------------------------------------
// CAsarFolder - the folder object Explorer binds for a .asar file
// ---------------------------------------------------------------------------
class CAsarFolder final : public IShellFolder2,
                          public IPersistFolder3,
                          public IShellIcon,
                          public IObjectWithFolderEnumMode {
public:
    CAsarFolder() = default;

    // Direct initialization used for subfolder binds (no pidl round trip).
    HRESULT InitDirect(const std::wstring& archivePath, const std::wstring& subPath);

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IPersist
    STDMETHOD(GetClassID)(CLSID* clsid) override;

    // IPersistFolder
    STDMETHOD(Initialize)(PCIDLIST_ABSOLUTE pidl) override;

    // IPersistFolder2
    STDMETHOD(GetCurFolder)(PIDLIST_ABSOLUTE* ppidl) override;

    // IPersistFolder3
    STDMETHOD(InitializeEx)(IBindCtx* pbc, PCIDLIST_ABSOLUTE pidlRoot,
                            const PERSIST_FOLDER_TARGET_INFO* pfti) override;
    STDMETHOD(GetFolderTargetInfo)(PERSIST_FOLDER_TARGET_INFO* pfti) override;

    // IShellFolder
    STDMETHOD(ParseDisplayName)(HWND hwnd, IBindCtx* pbc, PWSTR pszDisplayName,
                                ULONG* pchEaten, PIDLIST_RELATIVE* ppidl,
                                ULONG* pdwAttributes) override;
    STDMETHOD(EnumObjects)(HWND hwnd, SHCONTF grfFlags,
                           IEnumIDList** ppeid) override;
    STDMETHOD(BindToObject)(PCUIDLIST_RELATIVE pidl, IBindCtx* pbc, REFIID riid,
                            void** ppvOut) override;
    STDMETHOD(BindToStorage)(PCUIDLIST_RELATIVE pidl, IBindCtx* pbc, REFIID riid,
                             void** ppvObj) override;
    STDMETHOD(CompareIDs)(LPARAM lParam, PCUIDLIST_RELATIVE pidl1,
                          PCUIDLIST_RELATIVE pidl2) override;
    STDMETHOD(CreateViewObject)(HWND hwnd, REFIID riid, void** ppvOut) override;
    STDMETHOD(GetAttributesOf)(UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                               SFGAOF* rgfInOut) override;
    STDMETHOD(GetUIObjectOf)(HWND hwnd, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                             REFIID riid, UINT* prgfInOut, void** ppvOut) override;
    STDMETHOD(GetDisplayNameOf)(PCUITEMID_CHILD pidl, SHGDNF uFlags,
                                STRRET* lpName) override;
    STDMETHOD(SetNameOf)(HWND hwnd, PCUITEMID_CHILD pidl, LPCWSTR pszName,
                         SHGDNF uFlags, PITEMID_CHILD* ppidlOut) override;

    // IShellFolder2
    STDMETHOD(GetDefaultSearchGUID)(GUID* pguid) override;
    STDMETHOD(EnumSearches)(IEnumExtraSearch** ppenum) override;
    STDMETHOD(GetDefaultColumn)(DWORD dwRes, ULONG* pSort, ULONG* pDisplay) override;
    STDMETHOD(GetDefaultColumnState)(UINT iColumn, SHCOLSTATEF* pcsFlags) override;
    STDMETHOD(GetDetailsEx)(PCUITEMID_CHILD pidl, const SHCOLUMNID* pscid,
                            VARIANT* pv) override;
    STDMETHOD(GetDetailsOf)(PCUITEMID_CHILD pidl, UINT iColumn,
                            SHELLDETAILS* psd) override;
    STDMETHOD(MapColumnToSCID)(UINT iColumn, SHCOLUMNID* pscid) override;

    // IObjectWithFolderEnumMode
    STDMETHOD(SetMode)(FOLDER_ENUM_MODE feMode) override;
    STDMETHOD(GetMode)(FOLDER_ENUM_MODE* pfeMode) override;

    // IShellIcon - S_FALSE routes the shell to our IExtractIconW
    STDMETHOD(GetIconOf)(PCUITEMID_CHILD pidl, UINT flags, PINT lpIconLoc) override;

private:
    ~CAsarFolder();

    friend class CAsarItemMenu;

    LONG m_refs = 1;
    std::wstring m_archivePath;
    std::wstring m_subPath;  // in-archive path of THIS folder, "" = root
    PIDLIST_ABSOLUTE m_pidl = nullptr;  // full pidl, for GetCurFolder
    FOLDER_ENUM_MODE m_enumMode = FEM_VIEWRESULT;
    bool m_loaded = false;
    // Shared, immutable parse result owned by asarcache; m_node points into
    // it (this folder's subtree). No per-instance copies.
    std::shared_ptr<const asar::Header> m_header;
    const asar::Node* m_node = nullptr;

    HRESULT EnsureLoaded();
    const asar::Node* FindNode(const std::wstring& name) const;
    std::wstring ChildPath(const std::wstring& name) const;
};
