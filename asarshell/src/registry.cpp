// registry.cpp - per-user self registration (HKCU only, no admin required).
#include "shellext.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <olectl.h>

#include <filesystem>
#include <string>

namespace {

constexpr wchar_t kClsid[] = L"{8E1845A5-6803-4BED-87F1-BCB87BD610CA}";
constexpr wchar_t kProgId[] = L"asar.Archive";
constexpr wchar_t kHandlerName[] = L"AsarTools";
constexpr wchar_t kPrefsKey[] = L"Software\\AsarTools";

std::wstring RegGetStr(HKEY root, const std::wstring& subkey, const std::wstring& value) {
    std::wstring result;
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return result;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(h, value.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size > 0) {
        result.resize(size / sizeof(wchar_t));
        if (RegQueryValueExW(h, value.c_str(), nullptr, &type,
                             reinterpret_cast<BYTE*>(result.data()), &size) == ERROR_SUCCESS) {
            while (!result.empty() && result.back() == L'\0') result.pop_back();
        } else {
            result.clear();
        }
    }
    RegCloseKey(h);
    return result;
}

LSTATUS RegSetStr(HKEY root, const std::wstring& subkey, const std::wstring& value,
                  const std::wstring& data) {
    HKEY h = nullptr;
    DWORD disp = 0;
    LSTATUS err = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_WRITE, nullptr, &h, &disp);
    if (err != ERROR_SUCCESS) return err;
    err = RegSetValueExW(h, value.c_str(), 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(data.c_str()),
                         static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return err;
}

void RegDelTree(HKEY root, const std::wstring& subkey) {
    // SHDeleteKey removes a key with all its children.
    SHDeleteKeyW(root, subkey.c_str());
}

std::wstring ModulePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hInstance, path, MAX_PATH);
    return path;
}

}  // namespace

STDAPI DllRegisterServer(void) {
    const std::wstring dllPath = ModulePath();
    std::wstring dir = dllPath;
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir.resize(slash);
    const std::wstring icoPath = dir + L"\\asar.ico";
    const std::wstring viewerPath = dir + L"\\asarview.exe";

    const std::wstring classes = L"Software\\Classes";
    LSTATUS err = ERROR_SUCCESS;

    // 1. COM class.
    err = RegSetStr(HKEY_CURRENT_USER, classes + L"\\CLSID\\" + kClsid, L"",
                    L"ASAR Tools Context Menu");
    RegSetStr(HKEY_CURRENT_USER, classes + L"\\CLSID\\" + kClsid + L"\\InprocServer32", L"",
              dllPath);
    RegSetStr(HKEY_CURRENT_USER, classes + L"\\CLSID\\" + kClsid + L"\\InprocServer32",
              L"ThreadingModel", L"Apartment");

    // 2. File association: .asar -> asar.Archive (remember previous value).
    std::wstring prev = RegGetStr(HKEY_CURRENT_USER, classes + L"\\.asar", L"");
    if (!prev.empty() && prev != kProgId)
        RegSetStr(HKEY_CURRENT_USER, kPrefsKey, L"PrevAsarAssoc", prev);
    err = RegSetStr(HKEY_CURRENT_USER, classes + L"\\.asar", L"", kProgId);
    if (err != ERROR_SUCCESS) return SELFREG_E_CLASS;

    RegSetStr(HKEY_CURRENT_USER, classes + L"\\" + kProgId, L"", L"ASAR Archive");
    RegSetStr(HKEY_CURRENT_USER, classes + L"\\" + kProgId + L"\\DefaultIcon", L"", icoPath);
    // NOTE: the Open verb is owned by asarfolder.dll (double-click browses the
    // archive inside Explorer, like .zip). The viewer stays reachable through
    // the context menu entry added by contextmenu.cpp.

    // 3. Context menu handlers.
    RegSetStr(HKEY_CURRENT_USER,
              classes + L"\\SystemFileAssociations\\.asar\\shellex\\ContextMenuHandlers\\" +
                  kHandlerName,
              L"", kClsid);
    RegSetStr(HKEY_CURRENT_USER,
              classes + L"\\Directory\\shellex\\ContextMenuHandlers\\" + kHandlerName, L"",
              kClsid);

    // 4. Approved shell extension list (per user).
    RegSetStr(HKEY_CURRENT_USER,
              L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
              kClsid, L"ASAR Tools Context Menu");

    // Fail loudly if the association (the one registry value other tools
    // might also want to write) could not be written.
    if (err != ERROR_SUCCESS) return SELFREG_E_CLASS;

    // Make Explorer notice the changes.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer(void) {
    const std::wstring classes = L"Software\\Classes";

    RegDelTree(HKEY_CURRENT_USER, classes + L"\\CLSID\\" + kClsid);
    RegDelTree(HKEY_CURRENT_USER,
               classes + L"\\SystemFileAssociations\\.asar\\shellex\\ContextMenuHandlers\\" +
                   kHandlerName);
    RegDelTree(HKEY_CURRENT_USER,
               classes + L"\\Directory\\shellex\\ContextMenuHandlers\\" + kHandlerName);
    RegDelTree(HKEY_CURRENT_USER, classes + L"\\" + kProgId);
    RegDelTree(HKEY_CURRENT_USER,
               std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\"
                            L"Approved\\") + kClsid);

    // Restore the previous .asar association, if we recorded one.
    std::wstring prev = RegGetStr(HKEY_CURRENT_USER, kPrefsKey, L"PrevAsarAssoc");
    if (!prev.empty()) {
        RegSetStr(HKEY_CURRENT_USER, classes + L"\\.asar", L"", prev);
        RegDelTree(HKEY_CURRENT_USER, kPrefsKey);
    } else {
        RegDelTree(HKEY_CURRENT_USER, classes + L"\\.asar");
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
