// test_capi.cpp - exercises the asar.dll C API end to end:
// pack -> open -> list -> read -> extract -> add -> verify bytes.
#include <windows.h>

#include "../libasar/include/asar/asar_c.h"

#include <cstdio>
#include <filesystem>
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

static void WriteFileBytes(const std::wstring& p, const char* data, size_t n) {
    fs::create_directories(fs::path(p).parent_path());
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) {
        std::printf("FAIL: cannot create %ls\n", p.c_str());
        exit(1);
    }
    if (n) fwrite(data, 1, n, f);
    fclose(f);
}

struct ListCtx {
    int files = 0, dirs = 0;
    bool sawHello = false;
};

static int LISTCB(void* user, const wchar_t* path, int is_dir, int,
                  unsigned long long) {
    auto* ctx = static_cast<ListCtx*>(user);
    if (is_dir) ++ctx->dirs;
    else ++ctx->files;
    if (wcscmp(path, L"hello.txt") == 0) ctx->sawHello = true;
    return 0;
}

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    fs::path root = fs::path(temp) / L"asar_capi_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"tree" / L"sub");

    const char hello[] = "Hello from the C API!";
    const char deep[] = "deep";
    WriteFileBytes((root / L"tree" / L"hello.txt").wstring(), hello,
                   sizeof(hello) - 1);
    WriteFileBytes((root / L"tree" / L"sub" / L"deep.txt").wstring(), deep,
                   sizeof(deep) - 1);

    fs::path archive = root / L"out.asar";
    fs::path srcDir = root / L"tree";
    CHECK(asar_pack(srcDir.c_str(), archive.c_str(), 1) == 0, "asar_pack");

    asar_archive_t a = asar_open(archive.c_str());
    CHECK(a != nullptr, "asar_open");
    if (!a) return 1;

    ListCtx lc;
    CHECK(asar_list(a, LISTCB, &lc) == 0, "asar_list");
    CHECK(lc.files == 2 && lc.dirs == 1, "list counts");
    CHECK(lc.sawHello, "hello.txt listed");

    unsigned char* data = nullptr;
    uint64_t size = 0;
    CHECK(asar_read_file(a, L"hello.txt", &data, &size) == 0, "asar_read_file");
    CHECK(size == sizeof(hello) - 1, "read size");
    CHECK(memcmp(data, hello, size) == 0, "read bytes");
    asar_free(data);
    data = nullptr;

    fs::path dest = root / L"extracted";
    CHECK(asar_extract(a, dest.c_str()) == 0, "asar_extract");
    HANDLE f = CreateFileW((dest / L"hello.txt").c_str(), GENERIC_READ, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(f != INVALID_HANDLE_VALUE, "extracted file exists");
    char buf[64] = {};
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf), &got, nullptr);
    CloseHandle(f);
    CHECK(got == sizeof(hello) - 1 && memcmp(buf, hello, got) == 0,
          "extracted bytes match");

    asar_close(a);

    // Add a file to the archive via the C API.
    const char extra[] = "extra!";
    fs::path extraSrc = root / L"extra.txt";
    WriteFileBytes(extraSrc.wstring(), extra, sizeof(extra) - 1);
    const wchar_t* sources[] = {extraSrc.c_str()};
    int addRc = asar_add(archive.c_str(), sources, 1, L"added", 1);
    if (addRc != 1) {
        const wchar_t* le = asar_last_error();
        char narrow[512] = {};
        WideCharToMultiByte(CP_ACP, 0, le ? le : L"(no error)", -1,
                            narrow, sizeof(narrow), nullptr, nullptr);
        std::printf("asar_add rc=%d lastError='%s'\n", addRc, narrow);
    }
    CHECK(addRc == 1, "asar_add count");

    // Reopen and verify the added entry.
    asar_archive_t a2 = asar_open(archive.c_str());
    CHECK(a2 != nullptr, "reopen");
    unsigned char* rd = nullptr;
    uint64_t rdSize = 0;
    CHECK(asar_read_file(a2, L"added/extra.txt", &rd, &rdSize) == 0,
          "read added");
    CHECK(rdSize == sizeof(extra) - 1 && memcmp(rd, extra, rdSize) == 0,
          "added bytes match");
    asar_free(rd);
    asar_close(a2);

    // Round-trip: extract via the C API and byte-compare the added file.
    fs::path out2 = root / L"ex2";
    asar_archive_t a3 = asar_open(archive.c_str());
    CHECK(a3 != nullptr, "reopen 2");
    CHECK(asar_extract(a3, out2.c_str()) == 0, "extract 2");
    asar_close(a3);
    HANDLE f2 = CreateFileW((out2 / L"added" / L"extra.txt").c_str(),
                            GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(f2 != INVALID_HANDLE_VALUE, "added extracted");
    char b2[32] = {};
    DWORD got2 = 0;
    ReadFile(f2, b2, sizeof(b2), &got2, nullptr);
    CloseHandle(f2);
    CHECK(got2 == sizeof(extra) - 1 && memcmp(b2, extra, got2) == 0,
          "added extracted bytes match");

    fs::remove_all(root, ec);
    if (g_failures) {
        std::printf("test_capi: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("test_capi: PASS\n");
    return 0;
}
