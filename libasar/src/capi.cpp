// capi.cpp - C API implementation on top of the libasar C++ core.
#include "asar/asar_c.h"

#include "asar/asar.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

namespace {

thread_local std::wstring t_lastError;

void SetLastErrorMsg(const std::wstring& msg) { t_lastError = msg; }

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

}  // namespace

extern "C" {

ASAR_CAPI asar_archive_t asar_open(const wchar_t* archive_path) {
    if (!archive_path) {
        SetLastErrorMsg(L"archive path is NULL");
        return nullptr;
    }
    SetLastErrorMsg(L"");
    try {
        return reinterpret_cast<asar_archive_t>(
            asar::Archive::Open(archive_path).release());
    } catch (const std::exception& e) {
        SetLastErrorMsg(Utf8ToWide(e.what()).c_str());
        return nullptr;
    }
}

ASAR_CAPI void asar_close(asar_archive_t h) {
    delete reinterpret_cast<asar::Archive*>(h);
}

ASAR_CAPI int asar_list(asar_archive_t h, asar_list_cb cb, void* user) {
    if (!h || !cb) {
        SetLastErrorMsg(L"invalid argument");
        return -1;
    }
    SetLastErrorMsg(L"");
    try {
        auto* arc = reinterpret_cast<asar::Archive*>(h);
        for (const asar::Entry& e : arc->List()) {
            if (cb(user, e.path.c_str(), e.isDir ? 1 : 0, e.isLink ? 1 : 0,
                   (unsigned long long)e.size))
                break;
        }
        return 0;
    } catch (const std::exception& e) {
        SetLastErrorMsg(Utf8ToWide(e.what()).c_str());
        return -1;
    }
}

ASAR_CAPI int asar_read_file(asar_archive_t h, const wchar_t* path_in_archive,
                             unsigned char** data, uint64_t* size) {
    if (!data || !size || !h || !path_in_archive) {
        SetLastErrorMsg(L"invalid argument");
        return -1;
    }
    *data = nullptr;
    *size = 0;
    SetLastErrorMsg(L"");
    try {
        auto* arc = reinterpret_cast<asar::Archive*>(h);
        std::vector<uint8_t> bytes = arc->ReadFile(path_in_archive);
        // Allocated with malloc; the supported release path is asar_free().
        auto* out = static_cast<unsigned char*>(malloc(bytes.empty() ? 1 : bytes.size()));
        if (!out) {
            SetLastErrorMsg(L"out of memory");
            return -1;
        }
        if (!bytes.empty()) memcpy(out, bytes.data(), bytes.size());
        *data = out;
        *size = bytes.size();
        return 0;
    } catch (const std::exception& e) {
        SetLastErrorMsg(Utf8ToWide(e.what()).c_str());
        return -1;
    }
}

ASAR_CAPI int asar_extract(asar_archive_t h, const wchar_t* dest_dir) {
    if (!h || !dest_dir) {
        SetLastErrorMsg(L"invalid argument");
        return -1;
    }
    SetLastErrorMsg(L"");
    try {
        auto* arc = reinterpret_cast<asar::Archive*>(h);
        arc->ExtractAll(dest_dir, nullptr, nullptr);
        return 0;
    } catch (const std::exception& e) {
        SetLastErrorMsg(Utf8ToWide(e.what()).c_str());
        return -1;
    }
}

ASAR_CAPI int asar_extract_file(asar_archive_t h, const wchar_t* path_in_archive,
                                const wchar_t* dest_file) {
    if (!h || !path_in_archive || !dest_file) {
        SetLastErrorMsg(L"invalid argument");
        return -1;
    }
    SetLastErrorMsg(L"");
    try {
        auto* arc = reinterpret_cast<asar::Archive*>(h);
        std::vector<uint8_t> data = arc->ReadFile(path_in_archive);
        FILE* f = nullptr;
        if (_wfopen_s(&f, dest_file, L"wb") != 0 || !f) {
            SetLastErrorMsg(L"cannot open destination file");
            return -1;
        }
        if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        return 0;
    } catch (const std::exception& e) {
        SetLastErrorMsg(Utf8ToWide(e.what()).c_str());
        return -1;
    }
}

ASAR_CAPI int asar_pack(const wchar_t* src_dir, const wchar_t* out_archive,
                        int integrity) {
    if (!src_dir || !out_archive) {
        SetLastErrorMsg(L"invalid argument");
        return -1;
    }
    SetLastErrorMsg(L"");
    try {
        asar::Pack(src_dir, out_archive, asar::PackOptions{integrity != 0},
                   nullptr);
        return 0;
    } catch (const std::exception& e) {
        SetLastErrorMsg(Utf8ToWide(e.what()).c_str());
        return -1;
    }
}

ASAR_CAPI int asar_add(const wchar_t* archive_path,
                       const wchar_t* const* sources, size_t source_count,
                       const wchar_t* dest_dir_in_archive, int integrity) {
    if (!archive_path || !sources || !source_count) {
        SetLastErrorMsg(L"invalid argument");
        return -1;
    }
    SetLastErrorMsg(L"");
    try {
        std::vector<std::filesystem::path> srcs;
        srcs.reserve(source_count);
        for (size_t i = 0; i < source_count; ++i)
            srcs.emplace_back(sources[i] ? sources[i] : L"");
        size_t n = asar::Add(archive_path, srcs,
                             dest_dir_in_archive ? dest_dir_in_archive : L"",
                             asar::PackOptions{integrity != 0}, nullptr);
        return static_cast<int>(n);
    } catch (const std::exception& e) {
        SetLastErrorMsg(Utf8ToWide(e.what()).c_str());
        return -1;
    }
}

ASAR_CAPI int asar_remove(const wchar_t* archive_path,
                          const wchar_t* const* paths, size_t path_count,
                          size_t* not_found_count) {
    if (!archive_path || !paths || !path_count) {
        SetLastErrorMsg(L"invalid argument");
        return -1;
    }
    SetLastErrorMsg(L"");
    try {
        std::vector<std::wstring> v;
        v.reserve(path_count);
        for (size_t i = 0; i < path_count; ++i)
            v.emplace_back(paths[i] ? paths[i] : L"");
        std::vector<std::wstring> missing;
        size_t n = asar::Remove(archive_path, v, &missing);
        if (not_found_count) *not_found_count = missing.size();
        return static_cast<int>(n);
    } catch (const std::exception& e) {
        SetLastErrorMsg(Utf8ToWide(e.what()).c_str());
        return -1;
    }
}

ASAR_CAPI void asar_free(void* p) { free(p); }

ASAR_CAPI const wchar_t* asar_last_error(void) {
    return t_lastError.c_str();
}

ASAR_CAPI const wchar_t* asar_version(void) { return L"0.1.0"; }

}  // extern "C"
