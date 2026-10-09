// test_roundtrip.cpp - libasar pack/list/extract/add round-trip test.
#include <asar/asar.h>

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__);             \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static void WriteFileBytes(const fs::path& p, const std::vector<uint8_t>& data) {
    fs::create_directories(p.parent_path());
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) {
        std::printf("FAIL: cannot create %s\n", p.string().c_str());
        exit(1);
    }
    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static bool CompareFiles(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream ia(a, std::ios::binary), ib(b, std::ios::binary);
    char ba[4096], bb[4096];
    while (ia && ib) {
        ia.read(ba, sizeof(ba));
        ib.read(bb, sizeof(bb));
        auto ga = ia.gcount(), gb = ib.gcount();
        if (ga != gb) return false;
        if (memcmp(ba, bb, static_cast<size_t>(ga)) != 0) return false;
        if (ga == 0) break;
    }
    return true;
}

static void CompareTrees(const fs::path& a, const fs::path& b) {
    for (const auto& e : fs::recursive_directory_iterator(a)) {
        fs::path rel = fs::relative(e.path(), a);
        fs::path other = b / rel;
        std::error_code ec;
        CHECK(fs::exists(other, ec), "extracted tree is missing an entry");
        if (e.is_directory(ec)) continue;
        CHECK(CompareFiles(e.path(), other), "file content mismatch after round-trip");
    }
}

int wmain() {
    wchar_t tempRoot[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempRoot);
    fs::path root = fs::path(tempRoot) / L"asar_roundtrip_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    // --- Build a sample tree. ---
    fs::path tree = root / L"tree";
    std::vector<uint8_t> emptyBytes;
    std::vector<uint8_t> helloBytes = {'H', 'e', 'l', 'l', 'o', ',', ' ', 'A', 'S', 'A', 'R', '!',
                                       '\n'};
    std::vector<uint8_t> binBytes(5 * 1024 * 1024 + 123);  // spans integrity blocks
    for (size_t i = 0; i < binBytes.size(); ++i)
        binBytes[i] = static_cast<uint8_t>(i * 131 + (i >> 9));
    std::vector<uint8_t> uniBytes = {'u', 'n', 'i', 'c', 'o', 'd', 'e'};
    std::vector<uint8_t> addedBytes = {'a', 'd', 'd', 'e', 'd', '-', 'f', 'i', 'l', 'e'};

    WriteFileBytes(tree / L"empty.txt", emptyBytes);
    WriteFileBytes(tree / L"hello.txt", helloBytes);
    WriteFileBytes(tree / L"data" / L"bin.dat", binBytes);
    WriteFileBytes(tree / L"a" / L"b" / L"deep.txt", {'d', 'e', 'e', 'p'});
    WriteFileBytes(tree / L"emptydir" / L".keepme", {});  // placeholder removed below
    fs::path uni = tree / L"caf\xc3\xa9 \x65e5\x672c\x8a9e";  // UTF-8 in source is fine on disk
    WriteFileBytes(uni / L"test.txt", uniBytes);
    WriteFileBytes(tree / L"added-later.txt", addedBytes);

    // Make one truly empty directory.
    fs::path emptyDir = tree / L"emptydir2";
    fs::create_directories(emptyDir);

    // Remove the placeholder so "emptydir" is a real empty dir? Keep one empty
    // dir with no children:
    {
        fs::path realEmpty = tree / L"emptydir";
        fs::remove(realEmpty / L".keepme", ec);
    }

    // --- Pack. ---
    fs::path archive = root / L"out.asar";
    try {
        asar::Pack(tree, archive, asar::PackOptions{true}, nullptr);
    } catch (const std::exception& e) {
        std::printf("FAIL: pack threw: %s\n", e.what());
        return 1;
    }
    CHECK(fs::exists(archive), "archive file exists after pack");

    // --- Open + list. ---
    try {
        std::unique_ptr<asar::Archive> a = asar::Archive::Open(archive);
        std::vector<asar::Entry> entries = a->List();
        size_t files = 0, dirs = 0;
        const asar::Entry* binEntry = nullptr;
        for (const asar::Entry& e : entries) {
            if (e.isDir) ++dirs;
            else ++files;
            if (e.path == L"data/bin.dat") binEntry = &e;
        }
        CHECK(files == 6, "expected 6 files after pack");
        CHECK(dirs == 6, "expected 6 dirs after pack");  // data, a, a/b, cafe-日本語, emptydir2
        CHECK(binEntry != nullptr, "bin.dat listed");
        {
            const asar::Node* binNode = a->header().root.Find(L"data/bin.dat");
            CHECK(binNode != nullptr, "bin.dat node found");
            if (binNode) {
                CHECK(binNode->integrity.has_value(), "integrity computed");
                if (binNode->integrity) {
                    CHECK(binNode->integrity->blocks.size() == 2,
                          "5 MiB file spans exactly 2 integrity blocks");
                    CHECK(binNode->integrity->algorithm == "SHA256", "integrity algorithm");
                }
            }
        }

        // --- ReadFile spot checks. ---
        std::vector<uint8_t> hello = a->ReadFile(L"hello.txt");
        CHECK(hello == helloBytes, "ReadFile(hello.txt)");
        std::vector<uint8_t> bin = a->ReadFile(L"data/bin.dat");
        CHECK(bin == binBytes, "ReadFile(bin.dat)");

        // --- Extract all and compare. ---
        fs::path extracted = root / L"extracted";
        a->ExtractAll(extracted);
        CompareTrees(tree, extracted);
        CHECK(fs::exists(extracted / L"emptydir2", ec), "empty directory extracted");
    } catch (const std::exception& e) {
        std::printf("FAIL: exception: %s\n", e.what());
        return 1;
    }

    // --- Add a file. ---
    try {
        std::vector<fs::path> sources = {tree / L"hello.txt", tree / L"a" / L"b" / L"deep.txt"};
        size_t added = asar::Add(archive, sources, L"sub/dir", asar::PackOptions{true}, nullptr);
        CHECK(added == 2, "Add returned 2");
        std::unique_ptr<asar::Archive> a = asar::Archive::Open(archive);
        std::vector<uint8_t> got = a->ReadFile(L"sub/dir/hello.txt");
        CHECK(got == helloBytes, "added file readable");
        std::vector<uint8_t> got2 = a->ReadFile(L"sub/dir/deep.txt");
        CHECK(got2.size() == 4 && got2[0] == 'd', "second added file readable");
        // Original entries still intact after rewrite.
        std::vector<uint8_t> bin = a->ReadFile(L"data/bin.dat");
        CHECK(bin == binBytes, "original data intact after Add");
    } catch (const std::exception& e) {
        std::printf("FAIL: add threw: %s\n", e.what());
        return 1;
    }

    // --- Remove entries (file + whole folder subtree). ---
    try {
        size_t n = asar::Remove(archive, {L"hello.txt", L"a"},
                                nullptr);
        CHECK(n == 2, "Remove returned 2");
        std::unique_ptr<asar::Archive> a = asar::Archive::Open(archive);
        std::vector<asar::Entry> entries = a->List();
        bool helloGone = true, aGone = true, binKept = false;
        for (const asar::Entry& e : entries) {
            if (e.path == L"hello.txt" || e.path == L"a" ||
                e.path == L"a/b" || e.path == L"a/b/deep.txt")
                helloGone = false;
            if (e.path == L"data/bin.dat") binKept = true;
        }
        CHECK(helloGone, "removed entries absent from listing");
        CHECK(aGone, "removed folder subtree absent");
        CHECK(binKept, "surviving entry still listed");
        std::vector<uint8_t> bin = a->ReadFile(L"data/bin.dat");
        CHECK(bin == binBytes, "surviving payload intact after Remove");
    } catch (const std::exception& e) {
        std::printf("FAIL: remove threw: %s\n", e.what());
        return 1;
    }

    fs::remove_all(root, ec);
    if (g_failures) {
        std::printf("test_roundtrip: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("test_roundtrip: PASS\n");
    return 0;
}
