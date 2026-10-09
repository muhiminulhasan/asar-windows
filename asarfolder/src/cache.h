// cache.h - process-wide parsed-header cache for .asar archives.
//
// Explorer queries IDataObject / EnumObjects / BindToObject speculatively on
// every selection, hover and right-click. Re-parsing a 30k-entry,
// multi-megabyte JSON header on each query stalls the UI thread for seconds
// ("hangs and works slow"). This cache parses once per (path, mtime, size)
// and shares the read-only Header across all folder instances and data
// objects in the process (thread-safe).

#pragma once

#include <asar/asar.h>

#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace asarcache {

struct Entry {
    std::shared_ptr<const asar::Header> header;
    uint64_t fileSize = 0;
    std::filesystem::file_time_type mtime{};
};

inline std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

inline std::map<std::wstring, Entry>& Map() {
    static std::map<std::wstring, Entry> map;
    return map;
}

// Returns the parsed header, using the cache on hit. Throws asar::Error
// when the archive cannot be opened/parsed (same as Archive::Open).
inline std::shared_ptr<const asar::Header> GetHeader(
    const std::wstring& archivePath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    uint64_t size = fs::file_size(archivePath, ec);
    if (ec) throw asar::Error("cannot stat archive");
    auto mtime = fs::last_write_time(archivePath, ec);
    if (ec) throw asar::Error("cannot stat archive");

    {
        std::lock_guard<std::mutex> lock(Mutex());
        auto it = Map().find(archivePath);
        if (it != Map().end() && it->second.fileSize == size &&
            it->second.mtime == mtime) {
            return it->second.header;
        }
    }
    // Miss: parse outside the lock. Concurrent misses may duplicate work
    // on cold open (see benchmark); correctness holds via last-writer-wins.
    std::unique_ptr<asar::Archive> archive =
        asar::Archive::Open(archivePath);
    auto header =
        std::make_shared<asar::Header>(archive->header());

    {
        std::lock_guard<std::mutex> lock(Mutex());
        Entry e;
        e.header = header;
        e.fileSize = size;
        e.mtime = mtime;
        Map()[archivePath] = std::move(e);
        // Bound growth: drop oldest when huge (archives rarely change).
        if (Map().size() > 32) Map().erase(Map().begin());
    }
    return header;
}

}  // namespace asarcache
