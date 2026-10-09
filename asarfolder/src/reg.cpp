// reg.cpp - per-user registration mirroring Windows' own zipfldr mechanism.
//
// Keys written (all under HKCU\Software\Classes):
//   CLSID\{clsid}                     "ASAR Archive Folder"
//     \InProcServer32                 <dir>\asarfolder.dll (Apartment)
//     \DefaultIcon                    <dir>\asar.ico
//     \ProgID                         asar.Archive
//     \ShellFolder\Attributes         0x200001A0  (folder|browsable|...)
//     \ShellFolder\UseDropHandler     ""
//   asar.Archive\CLSID                {clsid}
//   asar.Archive\ShellEx\StorageHandler {clsid}       <- the file-as-folder hook
//   asar.Archive\Shell\Open           MultiSelectModel = Document
//   asar.Archive\Shell\Open\Command   Explorer.exe /idlist,%I,%L
//     DelegateExecute                 {11dbb47c-a525-400b-9e80-a54615a090c0}
#include "folder.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <olectl.h>

extern HINSTANCE g_hInstance;

#include <string>

namespace {

constexpr wchar_t kClsid[] = L"{8AF8F867-E3EA-40B8-8A87-942ED2A3DF0D}";
constexpr wchar_t kProgId[] = L"asar.Archive";
// System "execute folder" delegate shipped with every Windows version.
constexpr wchar_t kOpenDelegate[] = L"{11dbb47c-a525-400b-9e80-a54615a090c0}";

LSTATUS RegSetStr(HKEY root, const std::wstring& subkey, const std::wstring& value,
                  const std::wstring& data) {
    HKEY h = nullptr;
    DWORD disp = 0;
    LSTATUS err = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &h,
                                  &disp);
    if (err != ERROR_SUCCESS) return err;
    err = RegSetValueExW(h, value.c_str(), 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(data.c_str()),
                         static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return err;
}

LSTATUS RegSetDword(HKEY root, const std::wstring& subkey, const std::wstring& value,
                    DWORD data) {
    HKEY h = nullptr;
    DWORD disp = 0;
    LSTATUS err = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &h,
                                  &disp);
    if (err != ERROR_SUCCESS) return err;
    err = RegSetValueExW(h, value.c_str(), 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(h);
    return err;
}

LSTATUS RegSetEmpty(HKEY root, const std::wstring& subkey,
                    const std::wstring& value) {
    return RegSetStr(root, subkey, value, L"");
}

LSTATUS RegSetStrExpand(HKEY root, const std::wstring& subkey,
                        const std::wstring& value, const std::wstring& data) {
    HKEY h = nullptr;
    DWORD disp = 0;
    LSTATUS err = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &h,
                                  &disp);
    if (err != ERROR_SUCCESS) return err;
    err = RegSetValueExW(h, value.c_str(), 0, REG_EXPAND_SZ,
                         reinterpret_cast<const BYTE*>(data.c_str()),
                         static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return err;
}

void RegDelTree(HKEY root, const std::wstring& subkey) {
    SHDeleteKeyW(root, subkey.c_str());
}

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hInstance, path, MAX_PATH);
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

}  // namespace

HRESULT RegisterAsarFolder(void) {
    const std::wstring dir = ModuleDir();
    const std::wstring icoPath = dir + L"\\asar.ico";
    const std::wstring dllPath = dir + L"\\asarfolder.dll";
    const std::wstring classes = L"Software\\Classes";
    const std::wstring clsidKey = classes + L"\\CLSID\\" + kClsid;
    const std::wstring progIdKey = classes + L"\\" + kProgId;

    HRESULT result = S_OK;

    // 1. Folder class.
    if (FAILED(result = (RegSetStr(HKEY_CURRENT_USER, clsidKey, L"",
                                   L"ASAR Archive Folder") == ERROR_SUCCESS
                            ? S_OK
                            : SELFREG_E_CLASS)))
        return result;
    RegSetStr(HKEY_CURRENT_USER, clsidKey + L"\\InProcServer32", L"", dllPath);
    RegSetStr(HKEY_CURRENT_USER, clsidKey + L"\\InProcServer32", L"ThreadingModel",
              L"Apartment");
    RegSetStr(HKEY_CURRENT_USER, clsidKey + L"\\DefaultIcon", L"", icoPath);
    RegSetStr(HKEY_CURRENT_USER, clsidKey + L"\\ProgID", L"", kProgId);
    RegSetDword(HKEY_CURRENT_USER, clsidKey + L"\\ShellFolder", L"Attributes",
                0x200001A0);
    RegSetEmpty(HKEY_CURRENT_USER, clsidKey + L"\\ShellFolder", L"UseDropHandler");
    RegSetEmpty(HKEY_CURRENT_USER, clsidKey + L"\\ShellFolder",
                L"DisableContextualTabset");

    // 1b. Shell-folder category, present on Windows 11's own ArchiveFolder
    // registration ({00021490-...} = CATID_ShellFolder).
    RegSetEmpty(HKEY_CURRENT_USER,
                clsidKey + L"\\Implemented Categories\\"
                           L"{00021490-0000-0000-C000-000000000046}",
                L"");

    // 2. ProgID hooks: both mechanisms registered - the StorageHandler is
    // what Explorer actually binds when navigating into the archive, and the
    // ProgID CLSID carries the ShellFolder attributes.
    RegSetStr(HKEY_CURRENT_USER, progIdKey + L"\\CLSID", L"", kClsid);
    RegSetStr(HKEY_CURRENT_USER, progIdKey + L"\\ShellEx\\StorageHandler", L"",
              kClsid);
    RegSetStr(HKEY_CURRENT_USER, progIdKey + L"\\AppUserModelID", L"",
              L"Microsoft.Windows.Explorer");

    // 3. Open verb: navigate Explorer into the archive. The command value is
    // REG_EXPAND_SZ, matching zipfldr's own registration (%SystemRoot%).
    RegSetStr(HKEY_CURRENT_USER, progIdKey + L"\\Shell\\Open", L"MultiSelectModel",
              L"Document");
    RegSetStrExpand(HKEY_CURRENT_USER, progIdKey + L"\\Shell\\Open\\Command", L"",
                    L"%SystemRoot%\\Explorer.exe /idlist,%I,%L");
    RegSetStr(HKEY_CURRENT_USER, progIdKey + L"\\Shell\\Open\\Command",
              L"DelegateExecute", kOpenDelegate);

    // Make Explorer notice the changes.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

HRESULT UnregisterAsarFolder(void) {
    const std::wstring classes = L"Software\\Classes";
    const std::wstring clsidKey = classes + L"\\CLSID\\" + kClsid;
    const std::wstring progIdKey = classes + L"\\" + kProgId;

    RegDelTree(HKEY_CURRENT_USER, clsidKey);
    RegDelTree(HKEY_CURRENT_USER, progIdKey + L"\\ShellEx\\StorageHandler");
    RegDelTree(HKEY_CURRENT_USER, progIdKey + L"\\Shell\\Open");
    RegDelTree(HKEY_CURRENT_USER, progIdKey + L"\\CLSID");
    RegDelTree(HKEY_CURRENT_USER, progIdKey + L"\\AppUserModelID");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
