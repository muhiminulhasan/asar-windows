// actions.cpp - the actual extract / open / pack operations.
#include "shellext.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "asar/asar.h"

namespace fs = std::filesystem;

std::wstring GetModuleDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hInstance, path, MAX_PATH);
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

void ShowError(HWND hwnd, const std::wstring& title, const std::wstring& msg) {
    MessageBoxW(hwnd, msg.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

namespace {

struct WaitCursor {
    HCURSOR m_old;
    explicit WaitCursor(HWND hwnd) {
        SetCapture(hwnd);
        m_old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    }
    ~WaitCursor() {
        SetCursor(m_old);
        ReleaseCapture();
    }
};

bool AnyTargetExists(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!ec) return true;
    }
    return false;
}

}  // namespace

bool ActionExtract(HWND hwnd, const std::wstring& asarPath, bool intoSubdir) {
    fs::path archive(asarPath);
    fs::path dest = archive.parent_path();
    if (intoSubdir) dest /= archive.stem();

    std::error_code ec;
    if (AnyTargetExists(dest)) {
        std::wstring msg = L"The destination folder already contains files:\n\n" +
                           dest.wstring() +
                           L"\n\nOverwrite existing files with matching names?";
        if (MessageBoxW(hwnd, msg.c_str(), L"ASAR Tools", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return true;  // cancelled by user, not an error
    }

    try {
        WaitCursor wait(hwnd);
        std::unique_ptr<asar::Archive> a = asar::Archive::Open(archive);
        std::vector<std::wstring> warnings;
        a->ExtractAll(dest, nullptr, &warnings);
        for (const std::wstring& w : warnings) {
            MessageBoxW(hwnd, w.c_str(), L"ASAR Tools warning", MB_OK | MB_ICONWARNING);
        }
    } catch (const std::exception& e) {
        std::wstring wide(e.what(), e.what() + strlen(e.what()));
        ShowError(hwnd, L"ASAR Tools - extraction failed", wide);
        return false;
    }
    return true;
}

bool ActionOpen(HWND hwnd, const std::wstring& asarPath) {
    std::wstring viewer = GetModuleDir() + L"\\asarview.exe";
    if (!PathFileExistsW(viewer.c_str())) {
        ShowError(hwnd, L"ASAR Tools",
                  L"asarview.exe was not found next to asarshell.dll.\n"
                  L"Reinstall ASAR Tools to fix this.");
        return false;
    }
    std::wstring params = L"\"" + asarPath + L"\"";
    HINSTANCE inst = ShellExecuteW(hwnd, L"open", viewer.c_str(), params.c_str(), nullptr,
                                   SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(inst) <= 32) {
        ShowError(hwnd, L"ASAR Tools", L"Failed to launch asarview.exe.");
        return false;
    }
    return true;
}

bool ActionPack(HWND hwnd, const std::wstring& dirPath) {
    fs::path dir(dirPath);
    fs::path out = dir.parent_path() / (dir.filename().wstring() + L".asar");

    std::error_code ec;
    if (fs::exists(out, ec)) {
        std::wstring msg = L"Archive already exists:\n\n" + out.wstring() +
                           L"\n\nOverwrite it?";
        if (MessageBoxW(hwnd, msg.c_str(), L"ASAR Tools", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return true;
    }

    try {
        WaitCursor wait(hwnd);
        asar::Pack(dir, out, asar::PackOptions{true}, nullptr);
    } catch (const std::exception& e) {
        std::wstring wide(e.what(), e.what() + strlen(e.what()));
        ShowError(hwnd, L"ASAR Tools - packing failed", wide);
        return false;
    }
    return true;
}
