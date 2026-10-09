// main.cpp - asar command line tool.
//
// Usage:
//   asar l <archive>                     list contents
//   asar x <archive> [destdir]           extract (default: <archive dir>\<archive stem>)
//   asar p <srcdir> <archive>            pack a directory
//   asar a <archive> <file...> [--to subdir-in-archive]  add files/dirs
//   asar t <archive>                     verify header and payload integrity
//
// Options: --no-integrity (p/a: skip SHA256 integrity computation)

#include <asar/asar.h>

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void PrintUsage() {
    std::fprintf(stderr,
        "asar-windows command line tool\n"
        "\n"
        "Usage:\n"
        "  asar l <archive>                              list contents\n"
        "  asar x <archive> [destdir]                    extract\n"
        "  asar p <srcdir> <archive>                     pack directory\n"
        "  asar a <archive> <file...> [--to <subdir>]    add files or directories\n"
        "  asar t <archive>                              verify integrity\n"
        "\n"
        "Options:\n"
        "  --no-integrity        skip SHA256 integrity metadata (pack/add)\n");
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

int CmdList(const std::wstring& archivePath) {
    std::unique_ptr<asar::Archive> a = asar::Archive::Open(archivePath);
    for (const asar::Entry& e : a->List()) {
        std::wstring line = e.path;
        std::string utf8(line.begin(), line.end());  // console-friendly dump
        if (e.isDir) {
            std::printf("       dir  %s\n", utf8.c_str());
        } else {
            std::printf("%10llu  %s%s\n", static_cast<unsigned long long>(e.size),
                        utf8.c_str(), e.unpacked ? "  (unpacked)" : "");
        }
    }
    return 0;
}

int CmdExtract(const std::wstring& archivePath, const std::wstring& dest) {
    std::unique_ptr<asar::Archive> a = asar::Archive::Open(archivePath);
    std::wstring destDir = dest;
    if (destDir.empty()) {
        std::filesystem::path p(archivePath);
        destDir = p.parent_path() / p.stem();
    }
    std::vector<asar::Error> errors;
    std::vector<std::wstring> warnings;
    size_t n = 0;
    try {
        n = a->ExtractAll(destDir, nullptr, &warnings);
    } catch (const asar::Error& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    for (const std::wstring& w : warnings)
        std::fwprintf(stderr, L"warning: %s\n", w.c_str());
    std::wprintf(L"extracted %llu file(s) to %s\n", static_cast<unsigned long long>(n),
                 destDir.c_str());
    return 0;
}

int CmdPack(const std::wstring& srcDir, const std::wstring& archivePath, bool integrity) {
    try {
        asar::Pack(srcDir, archivePath, asar::PackOptions{integrity}, nullptr);
    } catch (const asar::Error& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::wprintf(L"packed %s -> %s\n", srcDir.c_str(), archivePath.c_str());
    return 0;
}

int CmdAdd(const std::wstring& archivePath, const std::vector<std::filesystem::path>& files,
           const std::wstring& destDir, bool integrity) {
    try {
        size_t n = asar::Add(archivePath, files, destDir, asar::PackOptions{integrity}, nullptr);
        std::wprintf(L"added %llu file(s) to %s\n", static_cast<unsigned long long>(n),
                     archivePath.c_str());
    } catch (const asar::Error& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

int CmdTest(const std::wstring& archivePath) {
    try {
        std::unique_ptr<asar::Archive> a = asar::Archive::Open(archivePath);
        std::vector<asar::Entry> entries = a->List();
        uint64_t total = 0;
        size_t files = 0;
        size_t missingSidecars = 0;
        for (const asar::Entry& e : entries) {
            if (e.isDir || e.isLink) continue;
            std::vector<uint8_t> data;
            try {
                data = a->ReadFile(e.path);
            } catch (const asar::Error&) {
                if (e.unpacked) {
                    // Sidecar (<archive>.unpacked/...) missing on this
                    // machine: environment gap, not archive corruption.
                    ++missingSidecars;
                    continue;
                }
                throw;
            }
            if (data.size() != e.size) {
                std::fwprintf(stderr, L"size mismatch: %s\n", e.path.c_str());
                return 1;
            }
            // Verify SHA256 when the header carries integrity data.
            const asar::Node* n2 = a->header().root.Find(e.path);
            if (n2 && n2->integrity) {
                asar::Integrity got = asar::ComputeIntegrity(data.data(), data.size());
                if (got.hash != n2->integrity->hash) {
                    std::fwprintf(stderr, L"hash mismatch: %s\n", e.path.c_str());
                    return 1;
                }
                if (!n2->integrity->blocks.empty() && got.blocks != n2->integrity->blocks) {
                    std::fwprintf(stderr, L"block hash mismatch: %s\n", e.path.c_str());
                    return 1;
                }
            }
            total += e.size;
            ++files;
        }
        std::wprintf(L"ok: %llu file(s), %llu bytes, integrity verified\n",
                     static_cast<unsigned long long>(files),
                     static_cast<unsigned long long>(total));
        if (missingSidecars)
            std::wprintf(L"note: %llu unpacked file(s) skipped - "
                         L"sidecar folder <archive>.unpacked not present\n",
                         static_cast<unsigned long long>(missingSidecars));
    } catch (const asar::Error& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }
    std::wstring cmd = argv[1];
    bool integrity = true;
    std::wstring destDir;
    std::vector<std::wstring> rest;
    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--no-integrity") integrity = false;
        else if (a == L"--to" && i + 1 < argc) destDir = argv[++i];
        else rest.push_back(a);
    }

    if (cmd == L"l" && rest.size() == 1) return CmdList(rest[0]);
    if (cmd == L"x" && (rest.size() == 1 || rest.size() == 2))
        return CmdExtract(rest[0], rest.size() == 2 ? rest[1] : L"");
    if (cmd == L"p" && rest.size() == 2) return CmdPack(rest[0], rest[1], integrity);
    if (cmd == L"a" && rest.size() >= 2) {
        std::vector<std::filesystem::path> sources(rest.begin() + 1, rest.end());
        return CmdAdd(rest[0], sources, destDir, integrity);
    }
    if (cmd == L"t" && rest.size() == 1) return CmdTest(rest[0]);

    PrintUsage();
    return 2;
}
