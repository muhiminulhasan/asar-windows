// asar.h - libasar public API
//
// A self-contained C++17 library for reading, extracting, packing and
// modifying Electron ASAR archives.
//
// Archive layout (little-endian):
//   [uint32 4]                          pickle size prefix (always 4)
//   [uint32 headerLen]                  length of the header pickle buffer
//   [uint32 payloadLen]                 pickle payload length (aligned to 4)
//   [uint32 jsonLen]                    byte length of the UTF-8 JSON header
//   [jsonLen bytes][zero padding]       JSON: {"files": {...}}
//   [file data ...]                     payloads referenced by "offset"
//
// "offset" values in the JSON are decimal strings relative to the start of
// the data section, i.e. absolute offset = 8 + headerLen + Number(offset).

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace asar {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Matches the "integrity" object written by the official asar tool.
struct Integrity {
    std::string algorithm;               // "SHA256"
    std::string hash;                    // hex digest of the whole file
    uint64_t blockSize = 0;              // 4194304
    std::vector<std::string> blocks;     // hex digests per 4 MiB block
};

// One node of the archive tree (file, directory or link).
struct Node {
    bool isDir = false;
    bool isLink = false;
    bool unpacked = false;               // payload lives in <archive>.unpacked/
    bool executable = false;
    uint64_t size = 0;                   // file payload size in bytes
    std::string offset;                  // decimal string, relative to data start
    std::wstring link;                   // link target, '/' separated, archive relative
    std::optional<Integrity> integrity;
    std::map<std::wstring, Node> files;  // children when isDir

    // '/' separated path lookup below this node; nullptr when absent.
    const Node* Find(std::wstring_view path) const;
};

struct Header {
    Node root;                 // root node; iterate root.files for top level
    uint64_t dataStart = 0;    // absolute file offset of the data section
};

// Flat listing entry.
struct Entry {
    std::wstring path;         // '/' separated, no leading '/'
    bool isDir = false;
    bool isLink = false;
    bool unpacked = false;
    bool executable = false;
    uint64_t size = 0;
    std::string offset;
    std::wstring link;
};

using ProgressFn = std::function<void(uint64_t filesDone, const std::wstring& currentPath)>;

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

class Archive {
public:
    // Opens and parses the archive header. Throws Error on malformed input.
    static std::unique_ptr<Archive> Open(const std::filesystem::path& filePath);

    ~Archive();

    const Header& header() const { return m_header; }
    const std::filesystem::path& filePath() const { return m_path; }

    // Flat listing, depth-first, parents before children.
    std::vector<Entry> List() const;

    // Reads one file payload fully. Resolves link nodes and unpacked files.
    // Throws Error when the path does not exist or is a directory.
    std::vector<uint8_t> ReadFile(const std::wstring& pathInArchive) const;

    // Extracts every entry below destDir. Directories and links are
    // recreated (links may be skipped with a warning when the process lacks
    // symlink privilege). Returns the number of file payloads extracted.
    size_t ExtractAll(const std::filesystem::path& destDir,
                      const ProgressFn& onProgress = {},
                      std::vector<std::wstring>* warnings = nullptr) const;

private:
    Archive() = default;
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    std::filesystem::path m_path;
    Header m_header;
    mutable void* m_file = nullptr;    // FILE*, opened lazily
};

// ---------------------------------------------------------------------------
// Packing / modification
// ---------------------------------------------------------------------------

struct PackOptions {
    bool computeIntegrity = true;   // SHA256 whole-file + 4 MiB block digests
};

// Packs a directory tree into a new archive (overwrites outArchive).
// Symbolic links are skipped (recorded in `warnings` when provided).
void Pack(const std::filesystem::path& srcDir,
          const std::filesystem::path& outArchive,
          const PackOptions& opts = {},
          std::vector<std::wstring>* warnings = nullptr);

// Adds files or directory trees into an existing archive. The archive is
// rewritten (header + old data + appended new payloads); existing entry
// offsets remain valid. Overwrites existing entries on name collision.
// Returns the number of file payloads added.
size_t Add(const std::filesystem::path& archivePath,
           const std::vector<std::filesystem::path>& sources,
           const std::wstring& destDirInArchive = L"",
           const PackOptions& opts = {},
           std::vector<std::wstring>* warnings = nullptr);

// Removes entries (files or whole subtrees) from an existing archive.
// Payload bytes of the remaining entries are untouched (removed payloads
// stay behind as unreferenced space). Paths not found are reported through
// `notFound` when provided. Returns the number of entries removed.
size_t Remove(const std::filesystem::path& archivePath,
              const std::vector<std::wstring>& pathsInArchive,
              std::vector<std::wstring>* notFound = nullptr);

// ---------------------------------------------------------------------------
// Low-level building blocks (shared with the 7-Zip plugin and tests)
// ---------------------------------------------------------------------------

// Serializes the tree to compact JSON: {"files":{...}}.
std::string SerializeHeaderJson(const Node& root);

// Builds the archive prefix (8-byte size block + pickle buffer) from UTF-8
// header JSON.
std::vector<uint8_t> BuildHeaderBlock(const std::string& headerJsonUtf8);

// Parses UTF-8 header JSON into a tree. Throws Error on malformed input.
Header ParseHeaderJson(const std::string& headerJsonUtf8, uint64_t dataStart);

// Reads and validates the archive prefix from a buffer laid out exactly like
// the start of an archive file (size block + pickle). Returns the JSON.
// Throws Error when malformed.
std::string ParseHeaderBlock(const uint8_t* data, size_t len);

// Depth-first flatten, parents before children, deterministic order.
void Flatten(const Node& root, std::vector<Entry>& out);

// Computes official-style integrity (SHA256 of the whole payload plus 4 MiB
// block digests).
Integrity ComputeIntegrity(const uint8_t* data, size_t len);

// Splits a '/' separated in-archive path into components; throws Error on
// empty, ".", ".." or drive-like components.
std::vector<std::wstring> SplitArchivePath(const std::wstring& path);

// Inserts (or replaces) a node at path, creating parent directories.
void InsertNode(Node& root, const std::vector<std::wstring>& path, Node node);

}  // namespace asar
