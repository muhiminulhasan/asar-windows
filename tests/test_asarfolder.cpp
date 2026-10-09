// test_asarfolder.cpp - headless end-to-end test of the Explorer namespace
// extension: binds an .asar file as a shell folder (exactly what Explorer
// does via the ProgID StorageHandler), enumerates it, checks display names,
// and pulls file contents out through the drag-and-drop data object.
//
// Usage: test_asarfolder.exe [path-to-built-sample.asar]

#include <windows.h>
#include <shlobj.h>

#include <asar/asar.h>
#include "asarfolder/src/folder.h"

#include <cstdio>
#include <filesystem>
#include <memory>
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

// Must match CLSID_AsarFolder in asarfolder/src/folder.h.
// {8AF8F867-E3EA-40B8-8A87-942ED2A3DF0D}
static const GUID kAsarFolderId = {
    0x8AF8F867, 0xE3EA, 0x40B8, {0x8A, 0x87, 0x94, 0x2E, 0xD2, 0xA3, 0xDF, 0x0D}};

// --- Self-registration -----------------------------------------------------
// The shell only routes .asar binds to our StorageHandler when
// asarfolder.dll is registered under HKCU and the .asar extension points at
// the ProgID. The test registers the DLL next to itself (CMake copies it
// there) and adds the association, then restores both at the end.
static HMODULE g_hExtDll = nullptr;
static bool g_assocTouched = false;
static bool g_assocExisted = false;
static std::wstring g_assocOld;
static std::wstring g_prevDllPath;  // asarfolder.dll registered before the test

typedef HRESULT(STDAPICALLTYPE* DllRegFn)(void);

static bool RegisterExtensionForTest(void) {
    // If a copy is already registered (dev machine), remember which DLL so we
    // can re-register it afterwards; DllUnregisterServer would otherwise wipe
    // the user's installed Explorer integration.
    HKEY hPrev = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Classes\\CLSID\\{8AF8F867-E3EA-40B8-8A87-"
                      L"942ED2A3DF0D}\\InProcServer32",
                      0, KEY_READ, &hPrev) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH] = {};
        DWORD cb = sizeof(buf), type = 0;
        if (RegQueryValueExW(hPrev, nullptr, nullptr, &type,
                             reinterpret_cast<BYTE*>(buf),
                             &cb) == ERROR_SUCCESS &&
            type == REG_SZ)
            g_prevDllPath = buf;
        RegCloseKey(hPrev);
    }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path dll = fs::path(exePath).parent_path() / L"asarfolder.dll";
    g_hExtDll = LoadLibraryW(dll.c_str());
    if (!g_hExtDll) {
        std::printf("note: LoadLibrary(%ls) failed %lu\n", dll.c_str(),
                    GetLastError());
        return false;
    }
    auto reg = reinterpret_cast<DllRegFn>(GetProcAddress(g_hExtDll,
                                                         "DllRegisterServer"));
    if (!reg || FAILED(reg())) {
        std::printf("note: DllRegisterServer failed\n");
        return false;
    }

    // Associate .asar with the ProgID, saving any previous default value.
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.asar", 0,
                        nullptr, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                        nullptr, &h, nullptr) == ERROR_SUCCESS) {
        wchar_t old[256] = {};
        DWORD cb = sizeof(old), type = 0;
        g_assocExisted = (RegQueryValueExW(h, nullptr, nullptr, &type,
                                           reinterpret_cast<BYTE*>(old),
                                           &cb) == ERROR_SUCCESS &&
                          type == REG_SZ);
        if (g_assocExisted) g_assocOld = old;
        const wchar_t* progid = L"asar.Archive";
        RegSetValueExW(h, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(progid),
                       static_cast<DWORD>((wcslen(progid) + 1) *
                                          sizeof(wchar_t)));
        RegCloseKey(h);
        g_assocTouched = true;
    }
    return true;
}

static void UnregisterExtensionForTest(void) {
    if (g_assocTouched) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.asar", 0,
                          KEY_WRITE, &h) == ERROR_SUCCESS) {
            if (g_assocExisted) {
                RegSetValueExW(h, nullptr, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(g_assocOld.c_str()),
                               static_cast<DWORD>((g_assocOld.size() + 1) *
                                                  sizeof(wchar_t)));
            } else {
                RegDeleteValueW(h, nullptr);
            }
            RegCloseKey(h);
        }
        g_assocTouched = false;
    }
    if (g_hExtDll) {
        auto unreg = reinterpret_cast<DllRegFn>(GetProcAddress(
            g_hExtDll, "DllUnregisterServer"));
        if (unreg) unreg();
        FreeLibrary(g_hExtDll);
        g_hExtDll = nullptr;
    }
    // Restore a pre-existing registration (overwritten above).
    if (!g_prevDllPath.empty()) {
        HMODULE hPrev = LoadLibraryW(g_prevDllPath.c_str());
        if (hPrev) {
            auto reReg = reinterpret_cast<DllRegFn>(GetProcAddress(
                hPrev, "DllRegisterServer"));
            if (reReg) reReg();
            FreeLibrary(hPrev);
        }
        g_prevDllPath.clear();
    }
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(CoInitializeEx(nullptr,
                              COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        std::printf("FAIL: CoInitializeEx\n");
        return 1;
    }
    CHECK(RegisterExtensionForTest(), "register extension for test");

    // --- Build a sample archive with libasar. ---
    wchar_t tempRoot[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempRoot);
    fs::path root = fs::path(tempRoot) / L"asar_folder_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"tree" / L"docs");

    const std::vector<uint8_t> hello = {'H', 'e', 'l', 'l', 'o', ' ', 'E', 'x', 'p', 'l', 'o', 'r', 'e', 'r', '!'};
    const std::vector<uint8_t> payload(200000, 0x5A);
    const std::vector<uint8_t> readme = {'r', 'e', 'a', 'd', ' ', 'm', 'e'};
    {
        FILE* f = nullptr;
        _wfopen_s(&f, (root / L"tree" / L"hello.txt").c_str(), L"wb");
        fwrite(hello.data(), 1, hello.size(), f);
        fclose(f);
        _wfopen_s(&f, (root / L"tree" / L"docs" / L"readme.md").c_str(), L"wb");
        fwrite(readme.data(), 1, readme.size(), f);
        fclose(f);
        _wfopen_s(&f, (root / L"tree" / L"big.bin").c_str(), L"wb");
        fwrite(payload.data(), 1, payload.size(), f);
        fclose(f);
    }
    fs::path sample = root / L"sample.asar";
    asar::Pack(root / L"tree", sample, asar::PackOptions{true}, nullptr);
    std::unique_ptr<asar::Archive> ref = asar::Archive::Open(sample);
    std::vector<asar::Entry> refEntries = ref->List();
    size_t refDirs = 0, refFiles = 0;
    for (const asar::Entry& e : refEntries) (e.isDir ? refDirs : refFiles)++;

    // --- Bind the .asar file as a shell folder, like Explorer does. ---
    PIDLIST_ABSOLUTE filePidl = nullptr;
    SFGAOF flags = SFGAO_FOLDER;
    HRESULT hr = SHParseDisplayName(sample.c_str(), nullptr, &filePidl, flags, &flags);
    CHECK(hr == S_OK && filePidl, "SHParseDisplayName(asar)");

    IShellFolder* desktop = nullptr;
    CHECK(SHGetDesktopFolder(&desktop) == S_OK && desktop, "SHGetDesktopFolder");

    IShellFolder* folder = nullptr;
    hr = desktop->BindToObject(filePidl, nullptr, IID_IShellFolder,
                               reinterpret_cast<void**>(&folder));
    if (FAILED(hr)) {
        std::printf("note: StorageHandler bind returned 0x%08lX, trying direct creation\n",
                    static_cast<unsigned long>(hr));
        // Fallback: instantiate the class directly and initialize from pidl.
        IPersistFolder* persist = nullptr;
        CHECK(CoCreateInstance(kAsarFolderId, nullptr, CLSCTX_INPROC_SERVER,
                               IID_IPersistFolder,
                               reinterpret_cast<void**>(&persist)) == S_OK,
              "create folder class");
        if (persist) {
            CHECK(persist->Initialize(filePidl) == S_OK, "IPersistFolder::Initialize");
            hr = persist->QueryInterface(IID_IShellFolder, reinterpret_cast<void**>(&folder));
            persist->Release();
        }
    }
    CHECK(hr == S_OK && folder, "bind .asar as shell folder");
    if (!folder) {
        UnregisterExtensionForTest();
        return 1;
    }

    // --- Enumerate. ---
    IEnumIDList* enumIds = nullptr;
    CHECK(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                              &enumIds) == S_OK,
          "EnumObjects");
    size_t dirs = 0, files = 0;
    std::vector<PITEMID_CHILD> items;
    if (enumIds) {
        for (;;) {
            PITEMID_CHILD one = nullptr;
            ULONG got = 0;
            if (enumIds->Next(1, &one, &got) != S_OK || got == 0) break;
            items.push_back(one);
        }
        enumIds->Release();
    }
    for (PITEMID_CHILD p : items) {
        SFGAOF a = SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_CANCOPY | SFGAO_STREAM;
        PCUITEMID_CHILD cp = p;
        folder->GetAttributesOf(1, &cp, &a);
        std::printf("test-attrs: name via pidl, a=%08llx hr=%08lx\n",
                    (unsigned long long)a,
                    (unsigned long long)0);
        if (a & SFGAO_FOLDER) ++dirs;
        else ++files;
    }
    for (PITEMID_CHILD p : items) {
        auto* raw = reinterpret_cast<const AsarPidlItem*>(p);
        std::printf("pidl: cb=%u magic=%08X isDir=%u size=%llu name=%ls\n",
                    raw->cb, raw->magic, raw->isDir,
                    (unsigned long long)raw->size, raw->name);
    }
    std::printf("counts: dirs=%zu files=%zu (expected immediate children)\n", dirs,
                files);
    // Root must list immediate children only (zip-like), not all descendants.
    size_t expDirs = 0, expFiles = 0;
    for (const asar::Entry& e : refEntries) {
        if (e.path.find(L'/') != std::wstring::npos) continue;
        if (e.isDir) ++expDirs;
        else ++expFiles;
    }
    CHECK(dirs == expDirs, "directory count matches immediate children");
    CHECK(files == expFiles, "file count matches immediate children");
    // PIDLs must store leaf names (no slashes) for correct nested navigation.
    for (PITEMID_CHILD p : items) {
        std::wstring nm = PidlItemName(p);
        CHECK(!nm.empty() && nm.find(L'/') == std::wstring::npos,
              "child pidl is leaf name");
    }

    // --- Display names are leaf names. ---
    if (!items.empty()) {
        STRRET str = {};
        CHECK(folder->GetDisplayNameOf(items[0], SHGDN_NORMAL, &str) == S_OK,
              "GetDisplayNameOf");
        wchar_t name[MAX_PATH] = {};
        if (str.uType == STRRET_WSTR && str.pOleStr) {
            wcscpy_s(name, str.pOleStr);
            CoTaskMemFree(str.pOleStr);
        }
        std::wstring pidlLeaf = PidlItemName(items[0]);
        CHECK(name == pidlLeaf, "display name is the leaf name");
        CHECK(wcslen(name) > 0 && wcschr(name, L'/') == nullptr,
              "display name has no slash");
    }

    // --- Drag/copy data object: descriptor sizes + streamed contents. ---
    // Items are immediate children with leaf PIDLs; resolve ref entries by
    // full path (leaf for top-level files).
    for (size_t i = 0; i < items.size(); ++i) {
        SFGAOF a = SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_CANCOPY | SFGAO_STREAM;
        PCUITEMID_CHILD cp = items[i];
        folder->GetAttributesOf(1, &cp, &a);
        if (a & SFGAO_FOLDER) continue;
        std::wstring leaf = PidlItemName(cp);
        const asar::Entry* found = nullptr;
        for (const asar::Entry& re : refEntries) {
            if (re.path == leaf) {
                found = &re;
                break;
            }
        }
        CHECK(found != nullptr, "file maps to libasar entry");
        if (!found) continue;
        const asar::Entry& e = *found;

        IDataObject* data = nullptr;
        CHECK(folder->GetUIObjectOf(nullptr, 1, &cp, IID_IDataObject, nullptr,
                                    reinterpret_cast<void**>(&data)) == S_OK,
              "GetUIObjectOf(IDataObject)");
        if (!data) continue;

        CLIPFORMAT cfDesc = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
        CLIPFORMAT cfContents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
        FORMATETC fmt = {cfDesc, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM med = {};
        CHECK(data->GetData(&fmt, &med) == S_OK, "GetData(FILEDESCRIPTOR)");
        if (med.hGlobal) {
            auto* grp = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(med.hGlobal));
            CHECK(grp && grp->cItems == 1, "one file descriptor");
            if (grp) {
                CHECK(wcscmp(grp->fgd[0].cFileName, e.path.substr(e.path.find_last_of(L'/') + 1).c_str()) == 0,
                      "descriptor name");
                ULONGLONG size = (ULONGLONG(grp->fgd[0].nFileSizeHigh) << 32) |
                                 grp->fgd[0].nFileSizeLow;
                CHECK(size == e.size, "descriptor size");
            }
            GlobalUnlock(med.hGlobal);
            ReleaseStgMedium(&med);
        }

        fmt.cfFormat = cfContents;
        fmt.lindex = 0;
        fmt.tymed = TYMED_ISTREAM;
        med = {};
        CHECK(data->GetData(&fmt, &med) == S_OK && med.pstm, "GetData(FILECONTENTS)");
        if (med.pstm) {
            std::vector<uint8_t> got(e.size);
            ULONG readTotal = 0;
            while (readTotal < e.size) {
                ULONG gotN = 0;
                HRESULT hrRead =
                    med.pstm->Read(got.data() + readTotal, ULONG(e.size - readTotal), &gotN);
                if (FAILED(hrRead) || gotN == 0) break;
                readTotal += gotN;
            }
            std::vector<uint8_t> want = ref->ReadFile(e.path);
            CHECK(readTotal == want.size(), "stream length matches");
            if (readTotal == want.size())
                CHECK(memcmp(got.data(), want.data(), want.size()) == 0,
                      "stream bytes match payload");
            ReleaseStgMedium(&med);
        }
        data->Release();
    }

    // --- Icons available. ---
    if (!items.empty()) {
        IExtractIconW* icon = nullptr;
        PCUITEMID_CHILD cp = items[0];
        CHECK(folder->GetUIObjectOf(nullptr, 1, &cp, IID_IExtractIconW, nullptr,
                                    reinterpret_cast<void**>(&icon)) == S_OK,
              "GetUIObjectOf(IExtractIconW)");
        if (icon) icon->Release();
    }

    // --- Nested folder bind. ---
    for (size_t i = 0; i < items.size(); ++i) {
        SFGAOF a = SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_CANCOPY | SFGAO_STREAM;
        PCUITEMID_CHILD cp = items[i];
        folder->GetAttributesOf(1, &cp, &a);
        if (!(a & SFGAO_FOLDER)) continue;
        IShellFolder* sub = nullptr;
        CHECK(folder->BindToObject(items[i], nullptr, IID_IShellFolder,
                                   reinterpret_cast<void**>(&sub)) == S_OK,
              "BindToObject(subfolder)");
        if (sub) {
            IEnumIDList* subEnum = nullptr;
            CHECK(sub->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                                   &subEnum) == S_OK,
                  "subfolder EnumObjects");
            if (subEnum) {
                PITEMID_CHILD one = nullptr;
                ULONG got = 0;
                CHECK(subEnum->Next(1, &one, &got) == S_OK && got == 1,
                      "subfolder has its file");
                if (one) CoTaskMemFree(one);
                subEnum->Release();
            }
            sub->Release();
        }
        break;
    }

    for (PITEMID_CHILD p : items) CoTaskMemFree(p);
    folder->Release();
    desktop->Release();
    if (filePidl) ILFree(filePidl);
    fs::remove_all(root, ec);
    UnregisterExtensionForTest();
    CoUninitialize();

    if (g_failures) {
        std::printf("test_asarfolder: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("test_asarfolder: PASS\n");
    return 0;
}
