// asar.cpp - ASAR archive reading, extraction, packing and modification.
#include "asar/asar.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "json.h"
#include "sha256.h"

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#ifndef SYMBOLIC_LINK_FLAG_FILE
#define SYMBOLIC_LINK_FLAG_FILE 0x0
#endif
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

namespace asar {
namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) throw Error("invalid UTF-8 in archive header");
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct FileCloser {
    void operator()(FILE* f) const { if (f) fclose(f); }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

FilePtr OpenRead(const std::filesystem::path& p) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f)
        throw Error("cannot open for reading: " + WideToUtf8(p.wstring()));
    return FilePtr(f);
}

FilePtr OpenWrite(const std::filesystem::path& p) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f)
        throw Error("cannot open for writing: " + WideToUtf8(p.wstring()));
    return FilePtr(f);
}

void CopyBytes(FILE* src, FILE* dst, uint8_t* chunk, size_t chunkSize, uint64_t count) {
    while (count > 0) {
        size_t want = chunkSize < count ? chunkSize : static_cast<size_t>(count);
        size_t got = fread(chunk, 1, want, src);
        if (got == 0) throw Error("unexpected end of input while copying data");
        if (fwrite(chunk, 1, got, dst) != got)
            throw Error("write failed while copying data");
        count -= got;
    }
}

constexpr uint64_t kChunkSize = 1u << 20;      // 1 MiB copy buffer
constexpr uint64_t kBlockSize = 4194304;       // official asar integrity block size
constexpr uint32_t kMaxHeaderLen = 256u * 1024 * 1024;

std::string ReadAllBytes(const std::filesystem::path& p) {
    FilePtr f = OpenRead(p);
    _fseeki64(f.get(), 0, SEEK_END);
    long long sz = _ftelli64(f.get());
    _fseeki64(f.get(), 0, SEEK_SET);
    if (sz < 0) throw Error("cannot stat: " + WideToUtf8(p.wstring()));
    std::string out(static_cast<size_t>(sz), '\0');
    size_t got = sz ? fread(out.data(), 1, out.size(), f.get()) : 0;
    if (got != static_cast<size_t>(sz)) throw Error("read failed: " + WideToUtf8(p.wstring()));
    return out;
}

std::filesystem::path UnpackedDir(const std::filesystem::path& archive) {
    std::wstring s = archive.wstring();
    s += L".unpacked";
    return std::filesystem::path(s);
}

// ---------------------------------------------------------------------------
// JSON -> Node
// ---------------------------------------------------------------------------

uint64_t GetU64(const json::Value& obj, const wchar_t* key, uint64_t fallback = 0) {
    const json::Value* v = obj.Find(key);
    return v ? json::NumberToU64(*v, fallback) : fallback;
}

Node NodeFromJson(const json::Value& v) {
    if (v.type != json::Value::Type::Object) throw Error("header node is not an object");
    Node n;
    if (const json::Value* files = v.Find(L"files")) {
        if (files->type != json::Value::Type::Object) throw Error("'files' is not an object");
        n.isDir = true;
        for (const auto& kv : files->members)
            n.files[kv.first] = NodeFromJson(kv.second);
        return n;
    }
    if (const json::Value* link = v.Find(L"link")) {
        if (link->type != json::Value::Type::String) throw Error("'link' is not a string");
        n.isLink = true;
        n.link = link->str;
        return n;
    }
    n.size = GetU64(v, L"size");
    if (const json::Value* off = v.Find(L"offset")) {
        if (off->type != json::Value::Type::String) throw Error("'offset' is not a string");
        n.offset = std::string(off->str.begin(), off->str.end());  // decimal digits only
    }
    if (const json::Value* un = v.Find(L"unpacked")) n.unpacked = un->boolean;
    if (const json::Value* ex = v.Find(L"executable")) n.executable = ex->boolean;
    if (const json::Value* ig = v.Find(L"integrity")) {
        Integrity i;
        if (const json::Value* a = ig->Find(L"algorithm"))
            i.algorithm = std::string(a->str.begin(), a->str.end());
        if (const json::Value* h = ig->Find(L"hash"))
            i.hash = std::string(h->str.begin(), h->str.end());
        i.blockSize = GetU64(*ig, L"blockSize", kBlockSize);
        if (const json::Value* blocks = ig->Find(L"blocks")) {
            for (const json::Value& b : blocks->array)
                i.blocks.push_back(std::string(b.str.begin(), b.str.end()));
        }
        n.integrity = std::move(i);
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

std::vector<std::wstring> SplitArchivePath(const std::wstring& path) {
    std::vector<std::wstring> comps;
    std::wstring cur;
    auto flush = [&]() {
        if (cur.empty() || cur == L"." || cur == L"..")
            throw Error("invalid in-archive path component: " + WideToUtf8(path));
        if (cur.size() >= 2 && cur[1] == L':')
            throw Error("drive-like path is not allowed: " + WideToUtf8(path));
        comps.push_back(cur);
        cur.clear();
    };
    for (wchar_t c : path) {
        if (c == L'/' || c == L'\\') flush();
        else cur.push_back(c);
    }
    if (!cur.empty()) flush();
    if (comps.empty()) throw Error("empty in-archive path");
    return comps;
}

const Node* Node::Find(std::wstring_view path) const {
    const Node* n = this;
    std::wstring cur;
    auto step = [&]() -> bool {
        if (!n || !n->isDir) return false;
        auto it = n->files.find(cur);
        if (it == n->files.end()) return false;
        n = &it->second;
        return true;
    };
    for (wchar_t c : path) {
        if (c == L'/' || c == L'\\') {
            if (!step()) return nullptr;
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && !step()) return nullptr;
    return n;
}

void InsertNode(Node& root, const std::vector<std::wstring>& path, Node node) {
    Node* n = &root;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        n->isDir = true;
        n = &n->files[path[i]];
        if (!n->isDir) n->files.clear();  // replace file with dir
        n->isDir = true;
    }
    (*n).files[path.back()] = std::move(node);
}

void Flatten(const Node& root, std::vector<Entry>& out) {
    struct Rec {
        static void Walk(const std::wstring& prefix, const std::wstring& name, const Node& n,
                         std::vector<Entry>& out) {
            std::wstring path = prefix.empty() ? name : prefix + L"/" + name;
            Entry e;
            e.path = path;
            e.isDir = n.isDir;
            e.isLink = n.isLink;
            e.unpacked = n.unpacked;
            e.executable = n.executable;
            e.size = n.size;
            e.offset = n.offset;
            e.link = n.link;
            out.push_back(std::move(e));
            if (n.isDir) {
                for (const auto& kv : n.files)
                    Walk(path, kv.first, kv.second, out);
            }
        }
    };
    if (root.isDir) {
        for (const auto& kv : root.files)
            Rec::Walk(L"", kv.first, kv.second, out);
    }
}

// ---------------------------------------------------------------------------
// Header (de)serialization
// ---------------------------------------------------------------------------

Header ParseHeaderJson(const std::string& headerJsonUtf8, uint64_t dataStart) {
    json::Value v = json::Parse(headerJsonUtf8);
    Header h;
    h.root = NodeFromJson(v);
    if (!h.root.isDir) h.root.isDir = true;
    h.dataStart = dataStart;
    return h;
}

std::string ParseHeaderBlock(const uint8_t* data, size_t len) {
    if (len < 8) throw Error("archive too small");
    auto rd32 = [&](size_t off) -> uint32_t {
        return uint32_t(data[off]) | (uint32_t(data[off + 1]) << 8) |
               (uint32_t(data[off + 2]) << 16) | (uint32_t(data[off + 3]) << 24);
    };
    uint32_t prefix = rd32(0);
    uint32_t headerLen = rd32(4);
    if (prefix != 4) throw Error("not an ASAR archive (bad pickle prefix)");
    if (headerLen < 8 || headerLen > kMaxHeaderLen || size_t(8) + headerLen > len)
        throw Error("corrupt ASAR header length");
    uint32_t payloadLen = rd32(8);
    uint32_t jsonLen = rd32(12);
    if (payloadLen < jsonLen || size_t(16) + jsonLen > size_t(8) + headerLen ||
        payloadLen > headerLen - 4)
        throw Error("corrupt ASAR pickle");
    if (jsonLen == 0) throw Error("empty ASAR header");
    return std::string(reinterpret_cast<const char*>(data) + 16, jsonLen);
}

namespace {

void WriteNodeJson(std::string& out, const std::wstring& name, const Node& n) {
    out += json::WriteString(name);
    out += ":{";
    if (n.isDir) {
        out += "\"files\":{";
        bool first = true;
        for (const auto& kv : n.files) {
            if (!first) out += ',';
            first = false;
            WriteNodeJson(out, kv.first, kv.second);
        }
        out += "}}";
        return;
    }
    if (n.isLink) {
        out += "\"link\":";
        out += json::WriteString(n.link);
        out += '}';
        return;
    }
    out += "\"size\":";
    out += std::to_string(n.size);
    out += ",\"offset\":\"";
    out += n.offset.c_str();
    out += '"';
    if (n.integrity) {
        out += ",\"integrity\":{\"algorithm\":\"SHA256\",\"hash\":\"";
        out += n.integrity->hash.c_str();
        out += "\",\"blockSize\":";
        out += std::to_string(n.integrity->blockSize ? n.integrity->blockSize : kBlockSize);
        out += ",\"blocks\":[";
        for (size_t i = 0; i < n.integrity->blocks.size(); ++i) {
            if (i) out += ',';
            out += '"';
            out += n.integrity->blocks[i].c_str();
            out += '"';
        }
        out += "]}";
    }
    if (n.unpacked) out += ",\"unpacked\":true";
    if (n.executable) out += ",\"executable\":true";
    out += '}';
}

// Reads `size` bytes of a file payload while hashing it and optionally
// writing the bytes to `sink`. Returns the integrity object.
Integrity HashAndCopy(FILE* src, FILE* sink, uint64_t size, bool withBlocks) {
    Integrity integ;
    Sha256 whole, block;
    uint64_t inBlock = 0;
    std::vector<uint8_t> buf(1u << 20);
    uint64_t remaining = size;
    while (remaining > 0) {
        size_t want = static_cast<size_t>((kChunkSize < remaining) ? kChunkSize : remaining);
        size_t got = fread(buf.data(), 1, want, src);
        if (got == 0) throw Error("unexpected end of input while reading payload");
        whole.Update(buf.data(), got);
        if (withBlocks) {
            const uint8_t* p = buf.data();
            size_t n = got;
            while (n > 0) {
                size_t take = static_cast<size_t>(
                    (kBlockSize - inBlock) < n ? (kBlockSize - inBlock) : n);
                block.Update(p, take);
                inBlock += take;
                p += take;
                n -= take;
                if (inBlock == kBlockSize) {
                    integ.blocks.push_back(ToHex(block.Finish().data(), 32));
                    block = Sha256{};
                    inBlock = 0;
                }
            }
        }
        if (sink && fwrite(buf.data(), 1, got, sink) != got)
            throw Error("write failed while streaming payload");
        remaining -= got;
    }
    if (withBlocks) {
        if (inBlock > 0 || integ.blocks.empty())
            integ.blocks.push_back(ToHex(block.Finish().data(), 32));
        std::array<uint8_t, 32> wholeDigest = whole.Finish();
        integ.hash = ToHex(wholeDigest.data(), 32);
    }
    integ.algorithm = "SHA256";
    integ.blockSize = kBlockSize;
    return integ;
}

// Walks a filesystem directory, building the archive tree and streaming all
// payload bytes into `dataFile` in deterministic order.
struct TreeBuilder {
    Node root;
    bool withIntegrity = true;
    std::vector<std::wstring>* warnings = nullptr;
    uint64_t offsetCursor = 0;

    void Warn(std::wstring msg) {
        if (warnings) warnings->push_back(std::move(msg));
    }

    void BuildDir(const std::filesystem::path& dir, Node& node) {
        node.isDir = true;
        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            // Skip symlink-like entries; asar stores plain files and dirs.
            std::error_code sec;
            if (e.is_symlink(sec) || sec) {
                Warn(L"skipped symlink: " + e.path().wstring());
                continue;
            }
            entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) {
                      return a.path().filename().wstring() < b.path().filename().wstring();
                  });
        for (const auto& ent : entries) {
            std::wstring name = ent.path().filename().wstring();
            std::error_code ec;
            if (ent.is_directory(ec) && !ec) {
                BuildDir(ent.path(), node.files[name]);
            } else if (ent.is_regular_file(ec) && !ec) {
                AddFile(ent.path(), node.files[name]);
            } else {
                Warn(L"skipped special file: " + ent.path().wstring());
            }
        }
    }

    void AddFile(const std::filesystem::path& p, Node& node) {
        unsigned long long sz = 0;
        std::error_code ec;
        sz = std::filesystem::file_size(p, ec);
        if (ec) throw Error("cannot stat file: " + WideToUtf8(p.wstring()));
        node.size = sz;
        node.offset = std::to_string(offsetCursor);
        offsetCursor += sz;
        // Payload bytes are appended by Pack()/Add() from the collected list;
        // the caller drives streaming through files list.
        fileList.push_back({p, &node});
    }

    std::vector<std::pair<std::filesystem::path, Node*>> fileList;

    void StreamPayloads(FILE* dataFile) {
        std::vector<uint8_t> buf(1u << 20);
        for (auto& [p, node] : fileList) {
            FilePtr f = OpenRead(p);
            Sha256 whole, block;
            uint64_t inBlock = 0;
            Integrity integ;
            uint64_t remaining = node->size;
            while (remaining > 0) {
                size_t want = static_cast<size_t>(
                    (static_cast<uint64_t>(buf.size()) < remaining) ? buf.size() : remaining);
                size_t got = fread(buf.data(), 1, want, f.get());
                if (got == 0) throw Error("read failed: " + WideToUtf8(p.wstring()));
                whole.Update(buf.data(), got);
                if (withIntegrity) {
                    const uint8_t* q = buf.data();
                    size_t n = got;
                    while (n > 0) {
                        size_t take = static_cast<size_t>(
                            (kBlockSize - inBlock) < n ? (kBlockSize - inBlock) : n);
                        block.Update(q, take);
                        inBlock += take;
                        q += take;
                        n -= take;
                        if (inBlock == kBlockSize) {
                            integ.blocks.push_back(ToHex(block.Finish().data(), 32));
                            block = Sha256{};
                            inBlock = 0;
                        }
                    }
                }
                if (fwrite(buf.data(), 1, got, dataFile) != got)
                    throw Error("write failed while packing payload");
                remaining -= got;
            }
            if (withIntegrity) {
                if (inBlock > 0 || integ.blocks.empty())
                    integ.blocks.push_back(ToHex(block.Finish().data(), 32));
                std::array<uint8_t, 32> digest = whole.Finish();
                integ.hash = ToHex(digest.data(), 32);
                integ.algorithm = "SHA256";
                integ.blockSize = kBlockSize;
                node->integrity = std::move(integ);
            }
        }
    }
};

}  // namespace

std::string SerializeHeaderJson(const Node& root) {
    std::string out = "{\"files\":{";
    bool first = true;
    for (const auto& kv : root.files) {
        if (!first) out += ',';
        first = false;
        WriteNodeJson(out, kv.first, kv.second);
    }
    out += "}}";
    return out;
}

std::vector<uint8_t> BuildHeaderBlock(const std::string& headerJsonUtf8) {
    uint32_t jsonLen = static_cast<uint32_t>(headerJsonUtf8.size());
    uint32_t payloadLen = (4 + jsonLen + 3) & ~3u;
    uint32_t pickleLen = 4 + payloadLen;  // header pickle buffer length
    std::vector<uint8_t> block(8 + pickleLen, 0);
    auto wr32 = [&](size_t off, uint32_t v) {
        block[off] = uint8_t(v);
        block[off + 1] = uint8_t(v >> 8);
        block[off + 2] = uint8_t(v >> 16);
        block[off + 3] = uint8_t(v >> 24);
    };
    wr32(0, 4);
    wr32(4, pickleLen);
    wr32(8, payloadLen);
    wr32(12, jsonLen);
    if (jsonLen) memcpy(block.data() + 16, headerJsonUtf8.data(), jsonLen);
    return block;
}

// ---------------------------------------------------------------------------
// Archive
// ---------------------------------------------------------------------------

std::unique_ptr<Archive> Archive::Open(const std::filesystem::path& filePath) {
    std::string json;
    uint64_t dataStart = 0;
    {
        FilePtr f = OpenRead(filePath);
        uint8_t buf[8];
        if (fread(buf, 1, 8, f.get()) != 8) throw Error("file too small to be an ASAR archive");
        if (buf[0] != 4 || buf[1] != 0 || buf[2] != 0 || buf[3] != 0)
            throw Error("not an ASAR archive");
        uint32_t headerLen = uint32_t(buf[4]) | (uint32_t(buf[5]) << 8) |
                             (uint32_t(buf[6]) << 16) | (uint32_t(buf[7]) << 24);
        if (headerLen < 8 || headerLen > kMaxHeaderLen)
            throw Error("not an ASAR archive (bad header length)");
        // Re-read as one contiguous buffer: size block + header pickle.
        std::vector<uint8_t> all(8 + headerLen);
        memcpy(all.data(), buf, 8);
        if (fread(all.data() + 8, 1, headerLen, f.get()) != headerLen)
            throw Error("truncated ASAR header");
        json = ParseHeaderBlock(all.data(), all.size());
        dataStart = 8 + headerLen;
    }
    auto a = std::unique_ptr<Archive>(new Archive());
    a->m_path = filePath;
    a->m_header = ParseHeaderJson(json, dataStart);
    a->m_file = nullptr;
    return a;
}

Archive::~Archive() {
    if (m_file) fclose(static_cast<FILE*>(m_file));
}

std::vector<Entry> Archive::List() const {
    std::vector<Entry> out;
    Flatten(m_header.root, out);
    return out;
}

std::vector<uint8_t> Archive::ReadFile(const std::wstring& pathInArchive) const {
    const Node* n = m_header.root.Find(pathInArchive);
    if (!n) throw Error("not found in archive: " + WideToUtf8(pathInArchive));
    int depth = 0;
    while (n->isLink) {
        if (++depth > 16) throw Error("link loop at: " + WideToUtf8(pathInArchive));
        n = m_header.root.Find(n->link);
        if (!n) throw Error("broken link at: " + WideToUtf8(pathInArchive));
    }
    if (n->isDir) throw Error("path is a directory: " + WideToUtf8(pathInArchive));
    if (n->unpacked) {
        std::filesystem::path p = UnpackedDir(m_path) / std::filesystem::path(pathInArchive);
        std::string bytes = ReadAllBytes(p);
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    }
    if (!m_file) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, m_path.c_str(), L"rb") != 0 || !f)
            throw Error("cannot reopen archive file");
        m_file = f;
    }
    FILE* f = static_cast<FILE*>(m_file);
    uint64_t off = 0;
    for (char c : n->offset) off = off * 10 + (c - '0');
    if (_fseeki64(f, static_cast<long long>(m_header.dataStart + off), SEEK_SET) != 0)
        throw Error("seek failed");
    std::vector<uint8_t> out(static_cast<size_t>(n->size));
    size_t got = n->size ? fread(out.data(), 1, out.size(), f) : 0;
    if (got != out.size()) throw Error("truncated payload for: " + WideToUtf8(pathInArchive));
    return out;
}

size_t Archive::ExtractAll(const std::filesystem::path& destDir, const ProgressFn& onProgress,
                           std::vector<std::wstring>* warnings) const {
    std::filesystem::create_directories(destDir);
    std::vector<Entry> entries = List();
    uint64_t done = 0;
    FILE* f = nullptr;  // opened lazily for streaming payloads
    auto openArchive = [&]() -> FILE* {
        if (!f) {
            FILE* g = nullptr;
            if (_wfopen_s(&g, m_path.c_str(), L"rb") != 0 || !g)
                throw Error("cannot reopen archive file");
            f = g;
        }
        return f;
    };
    struct FileGuard {
        FILE** p;
        ~FileGuard() { if (*p) fclose(*p); }
    } guard{&f};

    std::vector<uint8_t> buf(1u << 20);
    for (const Entry& e : entries) {
        std::filesystem::path dest = destDir / std::filesystem::path(e.path);
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (e.isDir) {
            std::filesystem::create_directories(dest, ec);
        } else if (e.isLink) {
            std::wstring target = L"\\\\?\\" + (destDir / std::filesystem::path(e.link)).wstring();
            DWORD flags = SYMBOLIC_LINK_FLAG_FILE;
            const Node* n = m_header.root.Find(e.path);
            if (n && n->isDir) flags = SYMBOLIC_LINK_FLAG_DIRECTORY;
            BOOL ok = CreateSymbolicLinkW(dest.c_str(), target.c_str(),
                                          flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
            if (!ok)
                ok = CreateSymbolicLinkW(dest.c_str(), target.c_str(), flags);
            if (!ok && warnings)
                warnings->push_back(L"could not create symlink: " + e.path);
        } else if (e.unpacked) {
            std::filesystem::path src = UnpackedDir(m_path) / std::filesystem::path(e.path);
            std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec && warnings) warnings->push_back(L"missing unpacked file: " + e.path);
        } else {
            FILE* af = openArchive();
            uint64_t off = 0;
            for (char c : e.offset) off = off * 10 + (c - '0');
            if (_fseeki64(af, static_cast<long long>(m_header.dataStart + off), SEEK_SET) != 0)
                throw Error("seek failed while extracting");
            FilePtr out = OpenWrite(dest);
            uint64_t remaining = e.size;
            while (remaining > 0) {
                size_t want = static_cast<size_t>(
                    (static_cast<uint64_t>(buf.size()) < remaining) ? buf.size() : remaining);
                size_t got = fread(buf.data(), 1, want, af);
                if (got == 0) throw Error("truncated payload while extracting " + WideToUtf8(e.path));
                if (fwrite(buf.data(), 1, got, out.get()) != got)
                    throw Error("write failed while extracting " + WideToUtf8(e.path));
                remaining -= got;
            }
            ++done;
        }
        if (onProgress) onProgress(done, e.path);
    }
    return static_cast<size_t>(done);
}

// ---------------------------------------------------------------------------
// Pack / Add / Remove
// ---------------------------------------------------------------------------

size_t Remove(const std::filesystem::path& archivePath,
              const std::vector<std::wstring>& pathsInArchive,
              std::vector<std::wstring>* notFound) {
    std::unique_ptr<Archive> arc = Archive::Open(archivePath);
    Node root = arc->header().root;
    uint64_t dataStart = arc->header().dataStart;

    size_t removed = 0;
    for (const std::wstring& path : pathsInArchive) {
        std::vector<std::wstring> comps;
        try {
            comps = SplitArchivePath(path);
        } catch (const Error&) {
            if (notFound) notFound->push_back(path);
            continue;
        }
        Node* parent = &root;
        bool ok = true;
        for (size_t i = 0; i + 1 < comps.size(); ++i) {
            auto it = parent->files.find(comps[i]);
            if (it == parent->files.end() || !it->second.isDir) {
                ok = false;
                break;
            }
            parent = &it->second;
        }
        auto it = ok ? parent->files.find(comps.back()) : parent->files.end();
        if (!ok || it == parent->files.end()) {
            if (notFound) notFound->push_back(path);
            continue;
        }
        parent->files.erase(it);
        ++removed;
    }

    // Same rewrite as Add: new header, then the untouched old data section
    // (offsets of surviving entries remain valid).
    std::error_code ec;
    uint64_t total = std::filesystem::file_size(archivePath, ec);
    if (ec) throw Error("cannot stat archive");
    if (total < dataStart) throw Error("corrupt archive (size)");
    uint64_t oldDataSize = total - dataStart;

    std::string json = SerializeHeaderJson(root);
    std::vector<uint8_t> block = BuildHeaderBlock(json);
    std::filesystem::path newPath = archivePath.wstring() + L".tmpnew";
    {
        FilePtr out = OpenWrite(newPath);
        if (fwrite(block.data(), 1, block.size(), out.get()) != block.size())
            throw Error("failed writing archive header");
        {
            FILE* af = nullptr;
            if (_wfopen_s(&af, archivePath.c_str(), L"rb") != 0 || !af)
                throw Error("cannot reopen archive");
            FilePtr afGuard(af);
            if (_fseeki64(af, static_cast<long long>(dataStart), SEEK_SET) != 0)
                throw Error("seek failed in archive");
            std::vector<uint8_t> buf(1u << 20);
            uint64_t remaining = oldDataSize;
            while (remaining > 0) {
                size_t want = static_cast<size_t>(
                    (static_cast<uint64_t>(buf.size()) < remaining) ? buf.size()
                                                                    : remaining);
                size_t got = fread(buf.data(), 1, want, af);
                if (got == 0) throw Error("truncated archive while rewriting");
                if (fwrite(buf.data(), 1, got, out.get()) != got)
                    throw Error("write failed while rewriting archive");
                remaining -= got;
            }
        }
    }

    std::filesystem::path bak = archivePath.wstring() + L".bak";
    std::filesystem::remove(bak, ec);
    std::filesystem::rename(archivePath, bak, ec);
    std::filesystem::rename(newPath, archivePath, ec);
    if (ec) {
        std::filesystem::rename(bak, archivePath, ec);
        throw Error("failed to replace archive file");
    }
    std::filesystem::remove(bak, ec);
    return removed;
}

// ---------------------------------------------------------------------------
// Pack / Add
// ---------------------------------------------------------------------------

void Pack(const std::filesystem::path& srcDir, const std::filesystem::path& outArchive,
          const PackOptions& opts, std::vector<std::wstring>* warnings) {
    std::error_code ec;
    if (!std::filesystem::is_directory(srcDir, ec) || ec)
        throw Error("source is not a directory: " + WideToUtf8(srcDir.wstring()));

    TreeBuilder tb;
    tb.withIntegrity = opts.computeIntegrity;
    tb.warnings = warnings;
    tb.BuildDir(srcDir, tb.root);

    std::filesystem::path dataPath = outArchive.wstring() + L".tmpdata";
    {
        FilePtr dataFile = OpenWrite(dataPath);
        tb.StreamPayloads(dataFile.get());
    }

    std::string json = SerializeHeaderJson(tb.root);
    std::vector<uint8_t> block = BuildHeaderBlock(json);
    {
        FilePtr out = OpenWrite(outArchive);
        if (fwrite(block.data(), 1, block.size(), out.get()) != block.size())
            throw Error("failed writing archive header");
        FilePtr dataFile = OpenRead(dataPath);
        std::vector<uint8_t> buf(1u << 20);
        for (;;) {
            size_t got = fread(buf.data(), 1, buf.size(), dataFile.get());
            if (got == 0) break;
            if (fwrite(buf.data(), 1, got, out.get()) != got)
                throw Error("failed writing archive payload");
        }
    }
    std::filesystem::remove(dataPath, ec);
}

size_t Add(const std::filesystem::path& archivePath,
           const std::vector<std::filesystem::path>& sources,
           const std::wstring& destDirInArchive, const PackOptions& opts,
           std::vector<std::wstring>* warnings) {
    // Parse the existing archive.
    uint64_t oldDataSize = 0;
    Header header;
    {
        std::unique_ptr<Archive> arc = Archive::Open(archivePath);
        header = arc->header();
        std::error_code ec;
        uint64_t total = std::filesystem::file_size(archivePath, ec);
        if (ec) throw Error("cannot stat archive");
        if (total < header.dataStart) throw Error("corrupt archive (size)");
        oldDataSize = total - header.dataStart;
    }

    // Collect new entries.
    struct Pending {
        std::vector<std::wstring> path;  // in-archive components
        bool isDir = false;
        std::filesystem::path src;       // empty for plain dirs
        uint64_t size = 0;
    };
    std::vector<Pending> pending;
    std::vector<std::wstring> destBase;
    if (!destDirInArchive.empty()) destBase = SplitArchivePath(destDirInArchive);

    std::vector<std::pair<std::filesystem::path, Node*>> streamList;
    uint64_t offsetCursor = oldDataSize;

    for (const auto& src : sources) {
        std::error_code ec;
        bool isDir = std::filesystem::is_directory(src, ec);
        if (ec) throw Error("cannot access: " + WideToUtf8(src.wstring()));
        std::vector<std::wstring> base = destBase;
        base.push_back(src.filename().wstring());
        if (isDir) {
            if (std::filesystem::is_symlink(src, ec)) {
                if (warnings) warnings->push_back(L"skipped symlink: " + src.wstring());
                continue;
            }
            pending.push_back({base, true, {}, 0});
            // Recurse.
            struct Stack {
                static void Walk(const std::filesystem::path& dir,
                                 const std::vector<std::wstring>& base,
                                 std::vector<std::wstring>* warnings,
                                 std::vector<Pending>& pending,
                                 std::vector<std::filesystem::path>& streamPaths) {
                    std::vector<std::filesystem::directory_entry> entries;
                    for (const auto& e : std::filesystem::directory_iterator(dir)) {
                        std::error_code sec;
                        if (e.is_symlink(sec) || sec) {
                            if (warnings)
                                warnings->push_back(L"skipped symlink: " + e.path().wstring());
                            continue;
                        }
                        entries.push_back(e);
                    }
                    std::sort(entries.begin(), entries.end(),
                              [](const auto& a, const auto& b) {
                                  return a.path().filename().wstring() <
                                         b.path().filename().wstring();
                              });
                    for (const auto& ent : entries) {
                        std::vector<std::wstring> child = base;
                        child.push_back(ent.path().filename().wstring());
                        std::error_code ec2;
                        if (ent.is_directory(ec2) && !ec2) {
                            pending.push_back({child, true, {}, 0});
                            Walk(ent.path(), child, warnings, pending, streamPaths);
                        } else if (ent.is_regular_file(ec2) && !ec2) {
                            Pending f;
                            f.path = child;
                            f.src = ent.path();
                            f.size = ent.file_size(ec2);
                            if (ec2) throw Error("cannot stat: " + WideToUtf8(ent.path().wstring()));
                            pending.push_back(f);
                            streamPaths.push_back(ent.path());
                        }
                    }
                }
            };
            std::vector<std::filesystem::path> childPaths;
            Stack::Walk(src, base, warnings, pending, childPaths);
            for (const auto& p : childPaths) streamList.push_back({p, nullptr});
        } else {
            if (std::filesystem::is_symlink(src, ec)) {
                if (warnings) warnings->push_back(L"skipped symlink: " + src.wstring());
                continue;
            }
            Pending f;
            f.path = base;
            f.src = src;
            f.size = std::filesystem::file_size(src, ec);
            if (ec) throw Error("cannot stat: " + WideToUtf8(src.wstring()));
            pending.push_back(f);
            streamList.push_back({src, nullptr});
        }
    }

    // Stream all new payloads into a temp data file, computing integrity.
    std::filesystem::path dataPath = archivePath.wstring() + L".tmpdata";
    std::vector<Integrity> integrities(streamList.size());
    {
        FilePtr dataFile = OpenWrite(dataPath);
        for (size_t i = 0; i < streamList.size(); ++i) {
            FilePtr f = OpenRead(streamList[i].first);
            Integrity integ = HashAndCopy(f.get(), dataFile.get(),
                                          std::filesystem::file_size(streamList[i].first),
                                          opts.computeIntegrity);
            integrities[i] = std::move(integ);
        }
    }

    // Build the new tree.
    size_t streamIdx = 0;
    Node newRoot;  // fresh root: only entries carried via old-data plan items survive
    // Start from the old tree so untouched entries stay.
    newRoot = header.root;

    for (Pending& p : pending) {
        if (p.isDir) {
            Node d;
            d.isDir = true;
            InsertNode(newRoot, p.path, std::move(d));
        } else {
            Node n;
            n.size = p.size;
            n.offset = std::to_string(offsetCursor);
            offsetCursor += p.size;
            n.integrity = integrities[streamIdx];
            ++streamIdx;
            InsertNode(newRoot, p.path, std::move(n));
        }
    }

    // Serialize and write the new archive to a temp file, then swap.
    std::string json = SerializeHeaderJson(newRoot);
    std::vector<uint8_t> block = BuildHeaderBlock(json);
    std::filesystem::path newPath = archivePath.wstring() + L".tmpnew";
    {
        FilePtr out = OpenWrite(newPath);
        if (fwrite(block.data(), 1, block.size(), out.get()) != block.size())
            throw Error("failed writing archive header");
        std::vector<uint8_t> buf(1u << 20);
        // 1) old payloads, unchanged
        {
            std::unique_ptr<Archive> arc = Archive::Open(archivePath);
            FILE* af = nullptr;
            if (_wfopen_s(&af, arc->filePath().c_str(), L"rb") != 0 || !af)
                throw Error("cannot reopen archive");
            FilePtr afGuard(af);
            if (_fseeki64(af, static_cast<long long>(header.dataStart), SEEK_SET) != 0)
                throw Error("seek failed in archive");
            uint64_t remaining = oldDataSize;
            while (remaining > 0) {
                size_t want = static_cast<size_t>(
                    (static_cast<uint64_t>(buf.size()) < remaining) ? buf.size() : remaining);
                size_t got = fread(buf.data(), 1, want, af);
                if (got == 0) throw Error("truncated archive while rewriting");
                if (fwrite(buf.data(), 1, got, out.get()) != got)
                    throw Error("write failed while rewriting archive");
                remaining -= got;
            }
        }
        // 2) new payloads
        {
            FilePtr dataFile = OpenRead(dataPath);
            uint64_t remaining = offsetCursor - oldDataSize;
            while (remaining > 0) {
                size_t want = static_cast<size_t>(
                    (static_cast<uint64_t>(buf.size()) < remaining) ? buf.size() : remaining);
                size_t got = fread(buf.data(), 1, want, dataFile.get());
                if (got == 0) throw Error("truncated temp data while rewriting");
                if (fwrite(buf.data(), 1, got, out.get()) != got)
                    throw Error("write failed while appending payloads");
                remaining -= got;
            }
        }
    }
    std::error_code ec;
    std::filesystem::remove(dataPath, ec);
    std::filesystem::path bak = archivePath.wstring() + L".bak";
    std::filesystem::remove(bak, ec);
    std::filesystem::rename(archivePath, bak, ec);
    std::filesystem::rename(newPath, archivePath, ec);
    if (ec) {
        std::filesystem::rename(bak, archivePath, ec);
        throw Error("failed to replace archive file");
    }
    std::filesystem::remove(bak, ec);

    size_t added = 0;
    for (const Pending& p : pending)
        if (!p.isDir) ++added;
    return added;
}

Integrity ComputeIntegrity(const uint8_t* data, size_t len) {
    Integrity integ;
    Sha256 whole, block;
    uint64_t inBlock = 0;
    whole.Update(data, len);
    const uint8_t* p = data;
    size_t n = len;
    while (n > 0) {
        size_t take = static_cast<size_t>(
            (kBlockSize - inBlock) < static_cast<uint64_t>(n) ? (kBlockSize - inBlock) : n);
        block.Update(p, take);
        inBlock += take;
        p += take;
        n -= take;
        if (inBlock == kBlockSize) {
            integ.blocks.push_back(ToHex(block.Finish().data(), 32));
            block = Sha256{};
            inBlock = 0;
        }
    }
    if (inBlock > 0 || integ.blocks.empty())
        integ.blocks.push_back(ToHex(block.Finish().data(), 32));
    std::array<uint8_t, 32> digest = whole.Finish();
    integ.hash = ToHex(digest.data(), 32);
    integ.algorithm = "SHA256";
    integ.blockSize = kBlockSize;
    return integ;
}

}  // namespace asar
