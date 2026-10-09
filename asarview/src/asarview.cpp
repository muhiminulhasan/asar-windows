// asarview.cpp - 7-Zip-style ASAR archive viewer (Win32, no dependencies).
//
// Hierarchical browsing like the 7-Zip File Manager: the list shows the
// immediate children of the current folder, with aggregate sizes and
// folder/file counts for directories. Toolbar navigation (Up / Backspace),
// extract selected or all, add files, delete entries, verify integrity.

#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <asar/asar.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Control / command ids.
constexpr int IDC_LIST = 1000;
constexpr int IDC_TOOLBAR = 1001;
constexpr int IDC_PATHBAR = 1002;
constexpr int IDM_OPEN = 101;
constexpr int IDM_EXTRACT_ALL = 102;
constexpr int IDM_EXTRACT_SEL = 103;
constexpr int IDM_ADD = 104;
constexpr int IDM_REFRESH = 105;
constexpr int IDM_EXIT = 106;
constexpr int IDM_ABOUT = 107;
constexpr int IDM_UP = 108;
constexpr int IDM_TEST = 109;
constexpr int IDM_INFO = 110;
constexpr int IDM_DELETE = 111;
constexpr int IDM_VIEW = 112;

static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;
static HWND g_hList = nullptr;
static HWND g_hToolbar = nullptr;
static HWND g_hPath = nullptr;
static HWND g_hStatus = nullptr;
static HFONT g_hFont = nullptr;
static std::unique_ptr<asar::Archive> g_archive;
static FILETIME g_archiveTime{};   // shown in the Modified column (the
                                   // ASAR format stores no per-entry times,
                                   // so every entry shares the archive's)

// ---------------------------------------------------------------------------
// Tree model: every folder keeps its children and aggregate statistics.
// ---------------------------------------------------------------------------

struct NodeInfo {
    std::wstring name;              // leaf name ("" for the root)
    std::wstring path;              // full in-archive path ("" = root)
    bool isDir = true;
    bool unpacked = false;
    uint64_t size = 0;              // payload size (files) / aggregate (dirs)
    uint64_t subFiles = 0;          // descendant file count (dirs)
    uint64_t subFolders = 0;        // descendant folder count (dirs)
};

static std::map<std::wstring, NodeInfo> g_nodes;             // key: full path, "" = root
static std::map<std::wstring, std::vector<std::wstring>> g_children;
static std::wstring g_curPath;                               // current folder key

static std::wstring ParentOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L'/');
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

static std::wstring LeafOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L'/');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

static void BuildTree() {
    g_nodes.clear();
    g_children.clear();
    g_nodes[L""] = NodeInfo{};  // root

    for (const asar::Entry& e : g_archive->List()) {
        // Create the node (folders may appear implicitly through children).
        NodeInfo& n = g_nodes[e.path];
        n.name = LeafOf(e.path);
        n.path = e.path;
        n.isDir = e.isDir;
        n.unpacked = e.unpacked;
        n.size = e.isDir ? 0 : e.size;

        std::wstring parent = ParentOf(e.path);
        g_children[parent].push_back(e.path);
        // Make sure implicit ancestor folder nodes exist.
        std::wstring p = parent;
        while (!p.empty() && g_nodes.find(p) == g_nodes.end()) {
            NodeInfo& folder = g_nodes[p];
            folder.name = LeafOf(p);
            folder.path = p;
            folder.isDir = true;
            p = ParentOf(p);
        }
    }

    // Aggregate: every file adds size+1 file to all strict ancestors; every
    // folder adds 1 folder to all strict ancestors.
    for (const auto& kv : g_nodes) {
        const NodeInfo& n = kv.second;
        if (n.path.empty()) continue;
        std::wstring p = ParentOf(n.path);
        while (!p.empty()) {
            NodeInfo& anc = g_nodes[p];
            if (n.isDir) anc.subFolders += 1;
            else {
                anc.subFiles += 1;
                anc.size += n.size;
            }
            p = ParentOf(p);
        }
    }

    // Sort children: folders first, then by name (case-insensitive).
    for (auto& kv : g_children) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const std::wstring& a, const std::wstring& b) {
                      const NodeInfo& na = g_nodes[a];
                      const NodeInfo& nb = g_nodes[b];
                      if (na.isDir != nb.isDir) return na.isDir;
                      return _wcsicmp(na.name.c_str(), nb.name.c_str()) < 0;
                  });
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::wstring Utf8Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

static void ShowError(HWND hwnd, const wchar_t* title, const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
    va_end(args);
    MessageBoxW(hwnd, msg, title, MB_OK | MB_ICONERROR);
}

static void ShowErrorA(HWND hwnd, const wchar_t* title, const std::exception& e) {
    std::wstring what = Utf8Wide(e.what());
    MessageBoxW(hwnd, what.c_str(), title, MB_OK | MB_ICONERROR);
}

static std::wstring FormatBytes(uint64_t n) {
    wchar_t buf[64];
    if (n >= 1u << 30)
        swprintf_s(buf, L"%.2f GiB", static_cast<double>(n) / (1u << 30));
    else if (n >= 1u << 20)
        swprintf_s(buf, L"%.2f MiB", static_cast<double>(n) / (1u << 20));
    else if (n >= 1u << 10)
        swprintf_s(buf, L"%.2f KiB", static_cast<double>(n) / (1u << 10));
    else
        swprintf_s(buf, L"%u B", static_cast<unsigned>(n));
    return buf;
}

static std::wstring ArchiveTimeString() {
    SYSTEMTIME st = {};
    FileTimeToSystemTime(&g_archiveTime, &st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute);
    return buf;
}

static void CleanupOldTemp() {
    wchar_t temp[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, temp)) return;
    std::wstring pattern = std::wstring(temp) + L"AsarView-*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::error_code ec;
            fs::remove_all(fs::path(temp) / fd.cFileName, ec);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static fs::path TempSessionDir() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    wchar_t name[64];
    swprintf_s(name, L"AsarView-%u", GetCurrentProcessId());
    return fs::path(temp) / name;
}

// ---------------------------------------------------------------------------
// File dialogs (IFileDialog)
// ---------------------------------------------------------------------------

static bool PickOpenAsar(HWND hwnd, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC,
                                IID_PPV_ARGS(&dlg))))
        return false;
    bool ok = false;
    const COMDLG_FILTERSPEC filters[] = {
        {L"ASAR archives", L"*.asar"},
        {L"All files", L"*.*"},
    };
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(L"Open ASAR archive");
    if (SUCCEEDED(dlg->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out = path;
                ok = true;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

static bool PickFolder(HWND hwnd, const wchar_t* title, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC,
                                IID_PPV_ARGS(&dlg))))
        return false;
    bool ok = false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out = path;
                ok = true;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

static bool PickFiles(HWND hwnd, std::vector<std::wstring>& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC,
                                IID_PPV_ARGS(&dlg))))
        return false;
    bool ok = false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Add files to archive");
    if (SUCCEEDED(dlg->Show(hwnd))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items)) && items) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        out.emplace_back(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            items->Release();
            ok = !out.empty();
        }
    }
    dlg->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// List view
// ---------------------------------------------------------------------------

static void UpdateStatusBar() {
    if (!g_archive || !g_hStatus) return;
    const NodeInfo& cur = g_nodes[g_curPath];
    wchar_t buf[512];
    swprintf_s(buf,
               L"%llu file(s), %llu folder(s), %s in this folder  |  %s%s",
               (unsigned long long)cur.subFiles, (unsigned long long)cur.subFolders,
               FormatBytes(cur.size).c_str(), g_archive->filePath().c_str(),
               g_curPath.empty() ? L"" : (L"  \\  " + g_curPath).c_str());
    SendMessageW(g_hStatus, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(buf));
}

static void SetPathBar() {
    wchar_t display[1024];
    swprintf_s(display, L"\\%s", g_curPath.c_str());
    SetWindowTextW(g_hPath, display);
}

static void PopulateList() {
    ListView_DeleteAllItems(g_hList);
    if (!g_archive) return;

    wchar_t modified[64];
    wcscpy_s(modified, ArchiveTimeString().c_str());

    int i = 0;
    auto it = g_children.find(g_curPath);
    if (it != g_children.end()) {
        for (const std::wstring& childPath : it->second) {
            const NodeInfo& n = g_nodes[childPath];

            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.iSubItem = 0;
            item.pszText = const_cast<LPWSTR>(n.name.c_str());
            int idx = ListView_InsertItem(g_hList, &item);

            wchar_t sizeBuf[64] = L"";
            if (!n.isDir)
                swprintf_s(sizeBuf, L"%llu", (unsigned long long)n.size);
            else if (n.size > 0)
                swprintf_s(sizeBuf, L"%s", FormatBytes(n.size).c_str());
            ListView_SetItemText(g_hList, idx, 1, sizeBuf);
            ListView_SetItemText(g_hList, idx, 2, modified);

            wchar_t foldersBuf[32] = L"";
            wchar_t filesBuf[32] = L"";
            if (n.isDir) {
                swprintf_s(foldersBuf, L"%llu", (unsigned long long)n.subFolders);
                swprintf_s(filesBuf, L"%llu", (unsigned long long)n.subFiles);
            }
            ListView_SetItemText(g_hList, idx, 3, foldersBuf);
            ListView_SetItemText(g_hList, idx, 4, filesBuf);
            ListView_SetItemText(g_hList, idx, 5,
                                 const_cast<LPWSTR>(n.unpacked ? L"yes" : L""));
            ++i;
        }
    }
    UpdateStatusBar();
}

static void SetColumns() {
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<LPWSTR>(L"Name");
    col.cx = 420;
    ListView_InsertColumn(g_hList, 0, &col);
    col.pszText = const_cast<LPWSTR>(L"Size");
    col.cx = 110;
    ListView_InsertColumn(g_hList, 1, &col);
    col.pszText = const_cast<LPWSTR>(L"Modified");
    col.cx = 130;
    ListView_InsertColumn(g_hList, 2, &col);
    col.pszText = const_cast<LPWSTR>(L"Folders");
    col.cx = 80;
    ListView_InsertColumn(g_hList, 3, &col);
    col.pszText = const_cast<LPWSTR>(L"Files");
    col.cx = 80;
    ListView_InsertColumn(g_hList, 4, &col);
    col.pszText = const_cast<LPWSTR>(L"Unpacked");
    col.cx = 80;
    ListView_InsertColumn(g_hList, 5, &col);
    ListView_SetExtendedListViewStyle(g_hList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
}

static void NavigateTo(const std::wstring& path) {
    g_curPath = path;
    SetPathBar();
    PopulateList();
}

static void NavigateUp() {
    if (g_curPath.empty()) return;
    NavigateTo(ParentOf(g_curPath));
}

static void LoadArchive(const std::wstring& path) {
    try {
        std::unique_ptr<asar::Archive> a = asar::Archive::Open(path);
        g_archive = std::move(a);
    } catch (const std::exception& e) {
        ShowErrorA(g_hWnd, L"Cannot open archive", e);
        return;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        GetFileTime(h, nullptr, nullptr, &g_archiveTime);
        CloseHandle(h);
    }
    BuildTree();
    NavigateTo(L"");
    std::wstring title = L"ASAR Viewer - " + path;
    SetWindowTextW(g_hWnd, title.c_str());
}

// ---------------------------------------------------------------------------
// Selection / actions
// ---------------------------------------------------------------------------

struct Selected {
    std::vector<const NodeInfo*> nodes;
    std::vector<int> indices;
};

static Selected GetSelected() {
    Selected sel;
    int i = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    while (i >= 0) {
        wchar_t name[MAX_PATH] = {};
        ListView_GetItemText(g_hList, i, 0, name, MAX_PATH);
        std::wstring full = g_curPath.empty() ? name : g_curPath + L"/" + name;
        auto it = g_nodes.find(full);
        if (it != g_nodes.end()) {
            sel.nodes.push_back(&it->second);
            sel.indices.push_back(i);
        }
        i = ListView_GetNextItem(g_hList, i, LVNI_SELECTED);
    }
    return sel;
}

static void ExtractSelected() {
    if (!g_archive) return;
    Selected sel = GetSelected();
    if (sel.nodes.empty()) {
        MessageBoxW(g_hWnd, L"Select entries to extract first.", L"ASAR Viewer", MB_OK);
        return;
    }
    std::wstring dest;
    if (!PickFolder(g_hWnd, L"Extract selected entries to", dest)) return;

    try {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        size_t done = 0;
        for (const NodeInfo* n : sel.nodes) {
            if (n->isDir) {
                // extract the whole subtree of this folder
                for (const asar::Entry& e : g_archive->List()) {
                    if (e.path != n->path &&
                        e.path.compare(0, n->path.size() + 1, n->path + L"/") != 0)
                        continue;
                    if (e.isDir || e.isLink) continue;
                    std::vector<uint8_t> bytes = g_archive->ReadFile(e.path);
                    fs::path out = fs::path(dest) / fs::path(e.path);
                    std::error_code ec;
                    fs::create_directories(out.parent_path(), ec);
                    FILE* f = nullptr;
                    if (_wfopen_s(&f, out.c_str(), L"wb") == 0 && f) {
                        if (!bytes.empty()) fwrite(bytes.data(), 1, bytes.size(), f);
                        fclose(f);
                        ++done;
                    }
                }
                continue;
            }
            std::vector<uint8_t> data = g_archive->ReadFile(n->path);
            fs::path out = fs::path(dest) / fs::path(n->name);
            FILE* f = nullptr;
            if (_wfopen_s(&f, out.c_str(), L"wb") == 0 && f) {
                if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
                fclose(f);
                ++done;
            }
        }
        SetCursor(old);
        wchar_t buf[160];
        swprintf_s(buf, L"Extracted %u file(s) to %s",
                   static_cast<unsigned>(done), dest.c_str());
        MessageBoxW(g_hWnd, buf, L"ASAR Viewer", MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception& e) {
        ShowErrorA(g_hWnd, L"Extraction failed", e);
    }
}

static void ExtractAll() {
    if (!g_archive) return;
    std::wstring dest;
    if (!PickFolder(g_hWnd, L"Extract archive to", dest)) return;
    try {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        std::vector<std::wstring> warnings;
        size_t n = g_archive->ExtractAll(dest, nullptr, &warnings);
        SetCursor(old);
        wchar_t buf[160];
        swprintf_s(buf, L"Extracted %u file(s) to %s",
                   static_cast<unsigned>(n), dest.c_str());
        MessageBoxW(g_hWnd, buf, L"ASAR Viewer", MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception& e) {
        ShowErrorA(g_hWnd, L"Extraction failed", e);
    }
}

static void AddFiles() {
    if (!g_archive) return;
    std::vector<std::wstring> files;
    if (!PickFiles(g_hWnd, files)) return;
    try {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        std::vector<fs::path> paths(files.begin(), files.end());
        // Files land in the folder currently being browsed.
        asar::Add(g_archive->filePath(), paths, g_curPath, asar::PackOptions{true},
                  nullptr);
        SetCursor(old);
        LoadArchive(g_archive->filePath().wstring());
    } catch (const std::exception& e) {
        ShowErrorA(g_hWnd, L"Adding files failed", e);
    }
}

static void DeleteSelected() {
    if (!g_archive) return;
    Selected sel = GetSelected();
    if (sel.nodes.empty()) {
        MessageBoxW(g_hWnd, L"Select entries to delete first.", L"ASAR Viewer", MB_OK);
        return;
    }
    wchar_t buf[256];
    swprintf_s(buf,
               L"Delete %u entr%s from the archive?\n\n"
               L"The archive is rewritten; deleted payloads cannot be recovered.",
               static_cast<unsigned>(sel.nodes.size()),
               sel.nodes.size() == 1 ? L"y" : L"ies");
    if (MessageBoxW(g_hWnd, buf, L"Delete", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    try {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        std::vector<std::wstring> paths;
        for (const NodeInfo* n : sel.nodes) paths.push_back(n->path);
        std::vector<std::wstring> missing;
        size_t n = asar::Remove(g_archive->filePath(), paths, &missing);
        SetCursor(old);
        LoadArchive(g_archive->filePath().wstring());
        wchar_t msg[160];
        swprintf_s(msg, L"Deleted %u entr%s.", static_cast<unsigned>(n),
                   n == 1 ? L"y" : L"ies");
        MessageBoxW(g_hWnd, msg, L"ASAR Viewer", MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception& e) {
        ShowErrorA(g_hWnd, L"Delete failed", e);
    }
}

static void PreviewEntry(int index) {
    if (!g_archive) return;
    wchar_t name[MAX_PATH] = {};
    ListView_GetItemText(g_hList, index, 0, name, MAX_PATH);
    std::wstring full = g_curPath.empty() ? name : g_curPath + L"/" + name;
    try {
        std::vector<uint8_t> data = g_archive->ReadFile(full);
        fs::path out = TempSessionDir() /
                       fs::path(g_archive->filePath()).stem() / fs::path(full);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        FILE* f = nullptr;
        if (_wfopen_s(&f, out.c_str(), L"wb") != 0 || !f) {
            ShowError(g_hWnd, L"ASAR Viewer", L"Cannot write temporary file.");
            return;
        }
        if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        ShellExecuteW(g_hWnd, L"open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } catch (const std::exception& ex) {
        ShowErrorA(g_hWnd, L"Cannot preview file", ex);
    }
}

static void TestIntegrity() {
    if (!g_archive) return;
    try {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        size_t files = 0, failures = 0, noIntegrity = 0;
        for (const asar::Entry& e : g_archive->List()) {
            if (e.isDir || e.isLink) continue;
            std::vector<uint8_t> data = g_archive->ReadFile(e.path);
            const asar::Node* n = g_archive->header().root.Find(e.path);
            if (n && n->integrity) {
                asar::Integrity got =
                    asar::ComputeIntegrity(data.data(), data.size());
                if (got.hash != n->integrity->hash) {
                    ++failures;
                    continue;
                }
                if (!n->integrity->blocks.empty() &&
                    got.blocks != n->integrity->blocks)
                    ++failures;
            } else {
                ++noIntegrity;
            }
            ++files;
        }
        SetCursor(old);
        wchar_t msg[256];
        if (failures == 0)
            swprintf_s(msg, L"Integrity OK: %u file(s) verified, %u without integrity data.",
                       static_cast<unsigned>(files), static_cast<unsigned>(noIntegrity));
        else
            swprintf_s(msg, L"INTEGRITY FAILURES: %u of %u file(s) are corrupted!",
                       static_cast<unsigned>(failures), static_cast<unsigned>(files));
        MessageBoxW(g_hWnd, msg, L"Test archive",
                    failures ? MB_OK | MB_ICONERROR : MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception& e) {
        ShowErrorA(g_hWnd, L"Test failed", e);
    }
}

static void ArchiveInfo() {
    if (!g_archive) return;
    std::error_code ec;
    uint64_t total = fs::file_size(g_archive->filePath(), ec);
    const asar::Header& h = g_archive->header();
    uint64_t dataStart = h.dataStart;
    size_t files = 0, dirs = 0;
    uint64_t payload = 0;
    for (const asar::Entry& e : g_archive->List()) {
        if (e.isDir) ++dirs;
        else {
            ++files;
            payload += e.size;
        }
    }
    wchar_t msg[1024];
    swprintf_s(
        msg,
        L"Archive:      %s\n"
        L"Archive size: %s\n"
        L"Header size:  %s\n"
        L"Payload size: %s (%llu file(s), %llu folder(s))\n"
        L"Slack space:  %s (unreferenced payload from earlier deletes)",
        g_archive->filePath().c_str(), FormatBytes(total).c_str(),
        FormatBytes(dataStart).c_str(), FormatBytes(payload).c_str(),
        (unsigned long long)files, (unsigned long long)dirs,
        FormatBytes(total > dataStart + payload ? total - dataStart - payload : 0)
            .c_str());
    MessageBoxW(g_hWnd, msg, L"Archive info", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// Window plumbing
// ---------------------------------------------------------------------------

static HMENU BuildMainMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXTRACT_ALL, L"Extract &all...\tCtrl+A");
    AppendMenuW(file, MF_STRING, IDM_EXTRACT_SEL, L"Extract &selected...\tCtrl+E");
    AppendMenuW(file, MF_STRING, IDM_ADD, L"&Add files...\tCtrl+D");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_UP, L"&Up one level\tBackspace");
    AppendMenuW(view, MF_STRING, IDM_REFRESH, L"&Refresh\tF5");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_TEST, L"&Test integrity\tCtrl+T");
    AppendMenuW(tools, MF_STRING, IDM_DELETE, L"&Delete selected\tDel");
    AppendMenuW(tools, MF_STRING, IDM_INFO, L"Archive &info\tCtrl+I");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About ASAR Viewer");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return bar;
}

static HWND BuildToolbar(HWND hwnd) {
    HWND tb = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                              WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST,
                              0, 0, 0, 0, hwnd,
                              reinterpret_cast<HMENU>(IDC_TOOLBAR), g_hInst, nullptr);
    SendMessageW(tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    struct Def {
        int id;
        const wchar_t* text;
    };
    static const Def defs[] = {
        {IDM_UP, L"Up"},
        {IDM_ADD, L"Add"},
        {IDM_EXTRACT_ALL, L"Extract"},
        {IDM_EXTRACT_SEL, L"Extract selected"},
        {IDM_TEST, L"Test"},
        {IDM_VIEW, L"View"},
        {IDM_DELETE, L"Delete"},
        {IDM_INFO, L"Info"},
    };
    for (const Def& d : defs) {
        TBBUTTON btn = {};
        btn.iBitmap = I_IMAGENONE;
        btn.idCommand = d.id;
        btn.fsState = TBSTATE_ENABLED;
        btn.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
        btn.iString = reinterpret_cast<INT_PTR>(d.text);
        SendMessageW(tb, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&btn));
    }
    SendMessageW(tb, TB_AUTOSIZE, 0, 0);
    return tb;
}

static void LayoutChildren() {
    RECT rc;
    GetClientRect(g_hWnd, &rc);
    SendMessageW(g_hToolbar, TB_AUTOSIZE, 0, 0);
    RECT tb;
    GetWindowRect(g_hToolbar, &tb);
    int tbHeight = tb.bottom - tb.top;
    SendMessageW(g_hPath, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
    MoveWindow(g_hPath, 2, tbHeight + 2, rc.right - 4, 24, TRUE);
    int listTop = tbHeight + 28;
    SendMessageW(g_hStatus, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(g_hStatus, &sb);
    int sbHeight = sb.bottom - sb.top;
    MoveWindow(g_hList, 0, listTop, rc.right, rc.bottom - listTop - sbHeight, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hToolbar = BuildToolbar(hwnd);
            g_hPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
                                          ES_READONLY,
                                      0, 0, 0, 0, hwnd,
                                      reinterpret_cast<HMENU>(IDC_PATHBAR), g_hInst,
                                      nullptr);
            g_hList = CreateWindowExW(
                0, WC_LISTVIEWW, nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST), g_hInst, nullptr);
            g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                                        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0,
                                        0, 0, hwnd, nullptr, g_hInst, nullptr);
            NONCLIENTMETRICSW ncm = {};
            ncm.cbSize = sizeof(ncm);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
            if (g_hFont) {
                SendMessageW(g_hList, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont),
                             TRUE);
                SendMessageW(g_hStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont),
                             TRUE);
                SendMessageW(g_hToolbar, WM_SETFONT,
                             reinterpret_cast<WPARAM>(g_hFont), TRUE);
                SendMessageW(g_hPath, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont),
                             TRUE);
            }
            SetColumns();
            LayoutChildren();
            DragAcceptFiles(hwnd, TRUE);
            return 0;
        }
        case WM_SIZE:
            LayoutChildren();
            return 0;
        case WM_SETFOCUS:
            SetFocus(g_hList);
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t path[MAX_PATH] = {};
            if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
                std::wstring p = path;
                int len = lstrlenW(path);
                CharLowerBuffW(path, len);
                if (wcsstr(path, L".asar") == path + len - 5) LoadArchive(p);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_NOTIFY: {
            const NMHDR* nm = reinterpret_cast<const NMHDR*>(lp);
            if (nm->idFrom == IDC_LIST && nm->code == NM_DBLCLK) {
                const NMITEMACTIVATE* ia = reinterpret_cast<const NMITEMACTIVATE*>(lp);
                if (ia->iItem >= 0 && !g_children.empty()) {
                    // Enter folders; preview files.
                    wchar_t name[MAX_PATH] = {};
                    ListView_GetItemText(g_hList, ia->iItem, 0, name, MAX_PATH);
                    std::wstring full =
                        g_curPath.empty() ? name : g_curPath + L"/" + name;
                    auto it = g_nodes.find(full);
                    if (it != g_nodes.end()) {
                        if (it->second.isDir) NavigateTo(full);
                        else PreviewEntry(ia->iItem);
                    }
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDM_OPEN) {
                std::wstring path;
                if (PickOpenAsar(hwnd, path)) LoadArchive(path);
            } else if (id == IDM_EXTRACT_ALL) {
                ExtractAll();
            } else if (id == IDM_EXTRACT_SEL) {
                ExtractSelected();
            } else if (id == IDM_ADD) {
                AddFiles();
            } else if (id == IDM_DELETE) {
                DeleteSelected();
            } else if (id == IDM_TEST) {
                TestIntegrity();
            } else if (id == IDM_INFO) {
                ArchiveInfo();
            } else if (id == IDM_UP) {
                NavigateUp();
            } else if (id == IDM_REFRESH) {
                if (g_archive) LoadArchive(g_archive->filePath().wstring());
            } else if (id == IDM_VIEW) {
                Selected sel = GetSelected();
                if (sel.nodes.size() == 1 && !sel.nodes[0]->isDir) {
                    int idx = sel.indices[0];
                    PreviewEntry(idx);
                }
            } else if (id == IDM_EXIT) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            } else if (id == IDM_ABOUT) {
                MessageBoxW(hwnd,
                            L"ASAR Viewer - part of asar-windows.\n"
                            L"Browse ASAR archives like the 7-Zip File Manager:\n"
                            L"folder navigation, drag-out, extract, add, delete,\n"
                            L"integrity testing.\n\nOpen source, MIT licensed.",
                            L"About", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static HACCEL BuildAccelerators() {
    ACCEL acc[] = {
        {FVIRTKEY | FCONTROL, 'O', IDM_OPEN},
        {FVIRTKEY | FCONTROL, 'A', IDM_EXTRACT_ALL},
        {FVIRTKEY | FCONTROL, 'E', IDM_EXTRACT_SEL},
        {FVIRTKEY | FCONTROL, 'D', IDM_ADD},
        {FVIRTKEY | FCONTROL, 'T', IDM_TEST},
        {FVIRTKEY | FCONTROL, 'I', IDM_INFO},
        {FVIRTKEY, VK_BACK, IDM_UP},
        {FVIRTKEY, VK_F5, IDM_REFRESH},
        {FVIRTKEY, VK_DELETE, IDM_DELETE},
    };
    return CreateAcceleratorTableW(acc, 9);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInst;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        return 1;
    INITCOMMONCONTROLSEX icc = {sizeof(icc),
                                ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    CleanupOldTemp();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = L"AsarViewWindow";
    if (!RegisterClassExW(&wc)) return 1;

    g_hWnd = CreateWindowExW(0, L"AsarViewWindow", L"ASAR Viewer",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900,
                             620, nullptr, BuildMainMenu(), hInst, nullptr);
    if (!g_hWnd) return 1;

    int nArgs = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (args && nArgs >= 2) LoadArchive(args[1]);
    if (args) LocalFree(args);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    HACCEL hAccel = BuildAccelerators();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!TranslateAcceleratorW(g_hWnd, hAccel, &msg) &&
            !IsDialogMessage(g_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_hFont) DeleteObject(g_hFont);
    DestroyAcceleratorTable(hAccel);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
