// contextmenu.cpp - IShellExtInit/IContextMenu implementation.
#include "shellext.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <filesystem>
#include <new>

#include "asar/asar.h"

namespace fs = std::filesystem;

CAsarMenu::CAsarMenu() {
    InterlockedIncrement(&g_cDllRefs);
}

CAsarMenu::~CAsarMenu() {
    InterlockedDecrement(&g_cDllRefs);
}

STDMETHODIMP CAsarMenu::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(static_cast<IShellExtInit*>(this));
    } else if (riid == IID_IShellExtInit) {
        *ppv = static_cast<IShellExtInit*>(this);
    } else if (riid == IID_IContextMenu) {
        *ppv = static_cast<IContextMenu*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CAsarMenu::AddRef() {
    return InterlockedIncrement(&m_refs);
}

STDMETHODIMP_(ULONG) CAsarMenu::Release() {
    ULONG r = InterlockedDecrement(&m_refs);
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP CAsarMenu::Initialize(PCIDLIST_ABSOLUTE /*pidlFolder*/, IDataObject* pdtobj,
                                   HKEY /*hkeyProgID*/) {
    if (!pdtobj) return E_FAIL;
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg = {};
    if (FAILED(pdtobj->GetData(&fmt, &stg))) return E_FAIL;
    HDROP drop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
    if (!drop) {
        ReleaseStgMedium(&stg);
        return E_FAIL;
    }
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count >= 1) {
        UINT len = DragQueryFileW(drop, 0, nullptr, 0);
        std::wstring path(len + 1, L'\0');
        DragQueryFileW(drop, 0, path.data(), len + 1);
        path.resize(wcslen(path.c_str()));

        std::error_code ec;
        if (count == 1 && fs::is_directory(path, ec) && !ec) {
            m_mode = Mode::Directory;
            m_target = path;
        } else if (count == 1) {
            // Treat as ASAR file by extension (works even for files not yet
            // associated with our ProgID).
            std::wstring ext = path;
            size_t dot = ext.find_last_of(L'.');
            if (dot != std::wstring::npos) ext = ext.substr(dot);
            CharLowerBuffW(ext.data(), static_cast<DWORD>(ext.size()));
            if (ext == L".asar") {
                m_mode = Mode::AsarFile;
                m_target = path;
            }
        }
    }
    GlobalUnlock(stg.hGlobal);
    ReleaseStgMedium(&stg);
    return m_mode == Mode::None ? E_FAIL : S_OK;
}

STDMETHODIMP CAsarMenu::QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst,
                                         UINT /*idCmdLast*/, UINT uFlags) {
    if (m_mode == Mode::None || (uFlags & CMF_DEFAULTONLY)) {
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    }

    HMENU sub = CreatePopupMenu();
    UINT id = idCmdFirst;
    const wchar_t* label = L"ASAR";

    if (m_mode == Mode::AsarFile) {
        std::wstring stem = fs::path(m_target).stem().wstring();
        std::wstring extractTo = L"Extract to \"" + stem + L"\\\"";
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
        mii.dwTypeData = extractTo.data();
        mii.cch = static_cast<UINT>(extractTo.size());
        mii.wID = id++;
        mii.fState = MFS_ENABLED;
        InsertMenuItemW(sub, UINT_MAX, TRUE, &mii);

        mii.dwTypeData = const_cast<LPWSTR>(L"Extract here");
        mii.wID = id++;
        InsertMenuItemW(sub, UINT_MAX, TRUE, &mii);

        mii.fMask = MIIM_TYPE;
        mii.fType = MFT_SEPARATOR;
        InsertMenuItemW(sub, UINT_MAX, TRUE, &mii);

        mii.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
        mii.fType = 0;
        mii.dwTypeData = const_cast<LPWSTR>(L"Open in ASAR viewer");
        mii.wID = id++;
        InsertMenuItemW(sub, UINT_MAX, TRUE, &mii);
    } else {
        std::wstring name = fs::path(m_target).filename().wstring();
        std::wstring packLabel = L"Pack to \"" + name + L".asar\"";
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
        mii.dwTypeData = packLabel.data();
        mii.cch = static_cast<UINT>(packLabel.size());
        mii.wID = id++;
        mii.fState = MFS_ENABLED;
        InsertMenuItemW(sub, UINT_MAX, TRUE, &mii);
        label = L"ASAR archive";
    }

    MENUITEMINFOW miiSub = {};
    miiSub.cbSize = sizeof(miiSub);
    miiSub.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_ID;
    miiSub.hSubMenu = sub;
    miiSub.dwTypeData = const_cast<LPWSTR>(label);
    miiSub.cch = static_cast<UINT>(wcslen(label));
    miiSub.wID = id++;
    if (!InsertMenuItemW(hmenu, indexMenu++, TRUE, &miiSub)) {
        DestroyMenu(sub);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    }

    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, id - idCmdFirst);
}

STDMETHODIMP CAsarMenu::InvokeCommand(LPCMINVOKECOMMANDINFO pici) {
    if (!pici) return E_POINTER;

    int cmd = -1;
    if (IS_INTRESOURCE(pici->lpVerb)) {
        cmd = LOWORD(reinterpret_cast<UINT_PTR>(pici->lpVerb));
    } else {
        // Programmatic invocation by verb.
        const char* verb = pici->lpVerb;
        if (!verb) return E_FAIL;
        if (_stricmp(verb, "Asar.ExtractTo") == 0) cmd = CMD_EXTRACT_TO;
        else if (_stricmp(verb, "Asar.ExtractHere") == 0) cmd = CMD_EXTRACT_HERE;
        else if (_stricmp(verb, "Asar.Open") == 0) cmd = CMD_OPEN;
        else if (_stricmp(verb, "Asar.Pack") == 0) cmd = CMD_PACK;
    }
    if (cmd < 0) return E_FAIL;

    switch (m_mode) {
        case Mode::AsarFile:
            switch (cmd) {
                case CMD_EXTRACT_TO:
                    return ActionExtract(pici->hwnd, m_target, true) ? S_OK : E_FAIL;
                case CMD_EXTRACT_HERE:
                    return ActionExtract(pici->hwnd, m_target, false) ? S_OK : E_FAIL;
                case CMD_OPEN:
                    return ActionOpen(pici->hwnd, m_target) ? S_OK : E_FAIL;
                default:
                    return E_FAIL;
            }
        case Mode::Directory:
            if (cmd == CMD_PACK) return ActionPack(pici->hwnd, m_target) ? S_OK : E_FAIL;
            return E_FAIL;
        default:
            return E_FAIL;
    }
}

STDMETHODIMP CAsarMenu::GetCommandString(UINT_PTR idCmd, UINT uType, UINT* /*pwReserved*/,
                                         CHAR* pszName, UINT cchMax) {
    if (cchMax == 0) return E_FAIL;
    const wchar_t* verbW = nullptr;
    const char* verbA = nullptr;
    switch (idCmd) {
        case CMD_EXTRACT_TO: verbA = "Asar.ExtractTo"; verbW = L"Asar.ExtractTo"; break;
        case CMD_EXTRACT_HERE: verbA = "Asar.ExtractHere"; verbW = L"Asar.ExtractHere"; break;
        case CMD_OPEN: verbA = "Asar.Open"; verbW = L"Asar.Open"; break;
        case CMD_PACK: verbA = "Asar.Pack"; verbW = L"Asar.Pack"; break;
        default: return E_INVALIDARG;
    }
    const wchar_t* helpW = nullptr;
    switch (idCmd) {
        case CMD_EXTRACT_TO: helpW = L"Extract the ASAR archive into a subfolder"; break;
        case CMD_EXTRACT_HERE: helpW = L"Extract the ASAR archive here"; break;
        case CMD_OPEN: helpW = L"Open the ASAR archive in the ASAR viewer"; break;
        case CMD_PACK: helpW = L"Pack this folder into an ASAR archive"; break;
    }

    switch (uType) {
        case GCS_VERBA:
            if (!verbA) return E_FAIL;
            lstrcpynA(pszName, verbA, cchMax);
            return S_OK;
        case GCS_VERBW:
            WideCharToMultiByte(CP_ACP, 0, verbW, -1, pszName, cchMax, nullptr, nullptr);
            return S_OK;
        case GCS_HELPTEXTA: {
            int len = lstrlenW(helpW);
            WideCharToMultiByte(CP_ACP, 0, helpW, -1, pszName, cchMax, nullptr, nullptr);
            (void)len;
            return S_OK;
        }
        case GCS_HELPTEXTW:
            // pszName is really LPWSTR for the W variants.
            lstrcpynW(reinterpret_cast<LPWSTR>(pszName), helpW, cchMax);
            return S_OK;
        default:
            return E_NOTIMPL;
    }
}
