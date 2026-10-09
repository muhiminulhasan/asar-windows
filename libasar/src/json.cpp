// json.cpp - minimal JSON parser/writer for ASAR headers.
#include "json.h"

#include <cstdio>

#include "asar/asar.h"

namespace asar::json {
namespace {

struct Parser {
    std::string_view in;
    size_t pos = 0;

    [[noreturn]] void Fail(const char* msg) {
        throw Error(std::string("JSON error at offset ") + std::to_string(pos) + ": " + msg);
    }

    void SkipWs() {
        while (pos < in.size()) {
            char c = in[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
            else break;
        }
    }

    char Peek() {
        if (pos >= in.size()) Fail("unexpected end of input");
        return in[pos];
    }

    void Expect(char c) {
        if (pos >= in.size() || in[pos] != c)
            Fail(std::string("expected '").append(1, c).append("'").c_str());
        ++pos;
    }

    static void AppendCodePoint(std::wstring& out, uint32_t cp) {
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
    }

    uint32_t ReadHex4() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = Peek();
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else Fail("invalid \\u escape");
            ++pos;
        }
        return v;
    }

    std::wstring ParseStringBody() {
        std::wstring out;
        for (;;) {
            if (pos >= in.size()) Fail("unterminated string");
            char c = in[pos++];
            if (c == '"') break;
            if (c == '\\') {
                char e = Peek();
                ++pos;
                switch (e) {
                    case '"': out.push_back(L'"'); break;
                    case '\\': out.push_back(L'\\'); break;
                    case '/': out.push_back(L'/'); break;
                    case 'b': out.push_back(L'\b'); break;
                    case 'f': out.push_back(L'\f'); break;
                    case 'n': out.push_back(L'\n'); break;
                    case 'r': out.push_back(L'\r'); break;
                    case 't': out.push_back(L'\t'); break;
                    case 'u': {
                        uint32_t cp = ReadHex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < in.size() &&
                            in[pos] == '\\' && in[pos + 1] == 'u') {
                            pos += 2;
                            uint32_t lo = ReadHex4();
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else
                                cp = 0xFFFD;  // lone surrogate
                        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                            cp = 0xFFFD;
                        }
                        AppendCodePoint(out, cp);
                        break;
                    }
                    default: Fail("bad escape character");
                }
            } else {
                // Copy one UTF-8 sequence through, validating lead byte.
                unsigned char b = static_cast<unsigned char>(c);
                if (b < 0x80) {
                    if (b < 0x20) Fail("control character in string");
                    out.push_back(static_cast<wchar_t>(b));
                } else {
                    int extra = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
                    if (extra < 0 || pos + extra > in.size()) Fail("bad UTF-8 sequence");
                    uint32_t cp = b & (0x3F >> extra);
                    bool ok = true;
                    for (int i = 0; i < extra; ++i) {
                        unsigned char cb = static_cast<unsigned char>(in[pos++]);
                        if ((cb & 0xC0) != 0x80) { ok = false; break; }
                        cp = (cp << 6) | (cb & 0x3F);
                    }
                    if (!ok) Fail("bad UTF-8 continuation");
                    AppendCodePoint(out, cp);
                }
            }
        }
        return out;
    }

    Value ParseValue() {
        SkipWs();
        char c = Peek();
        Value v;
        if (c == '{') {
            ++pos;
            v.type = Value::Type::Object;
            SkipWs();
            if (Peek() == '}') { ++pos; return v; }
            for (;;) {
                SkipWs();
                Expect('"');
                std::wstring key = ParseStringBody();
                SkipWs();
                Expect(':');
                v.members.emplace_back(std::move(key), ParseValue());
                SkipWs();
                if (Peek() == ',') { ++pos; continue; }
                Expect('}');
                break;
            }
        } else if (c == '[') {
            ++pos;
            v.type = Value::Type::Array;
            SkipWs();
            if (Peek() == ']') { ++pos; return v; }
            for (;;) {
                v.array.push_back(ParseValue());
                SkipWs();
                if (Peek() == ',') { ++pos; continue; }
                Expect(']');
                break;
            }
        } else if (c == '"') {
            ++pos;
            v.type = Value::Type::String;
            v.str = ParseStringBody();
        } else if (c == 't') {
            if (in.compare(pos, 4, "true") != 0) Fail("bad literal");
            pos += 4;
            v.type = Value::Type::Bool;
            v.boolean = true;
        } else if (c == 'f') {
            if (in.compare(pos, 5, "false") != 0) Fail("bad literal");
            pos += 5;
            v.type = Value::Type::Bool;
            v.boolean = false;
        } else if (c == 'n') {
            if (in.compare(pos, 4, "null") != 0) Fail("bad literal");
            pos += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            v.type = Value::Type::Number;
            size_t start = pos;
            if (c == '-') ++pos;
            while (pos < in.size() &&
                   ((in[pos] >= '0' && in[pos] <= '9') || in[pos] == '.' ||
                    in[pos] == 'e' || in[pos] == 'E' || in[pos] == '+' || in[pos] == '-'))
                ++pos;
            v.number = std::string(in.substr(start, pos - start));
        } else {
            Fail("unexpected character");
        }
        return v;
    }
};

}  // namespace

Value Parse(std::string_view utf8) {
    Parser p{utf8, 0};
    Value v = p.ParseValue();
    p.SkipWs();
    if (p.pos != utf8.size()) p.Fail("trailing data after value");
    return v;
}

std::string WriteString(std::wstring_view s) {
    // Wide -> UTF-8 bytes via manual encoding to avoid locale dependence.
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    auto pushUtf8 = [&out](uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = static_cast<unsigned short>(s[i]);  // wchar_t is 16-bit on Windows
        char c = static_cast<char>(cp);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
            uint32_t lo = static_cast<unsigned short>(s[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        switch (cp) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\b': out += "\\b"; continue;
            case '\f': out += "\\f"; continue;
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
        }
        if (cp < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", cp);
            out += buf;
        } else {
            pushUtf8(cp);
        }
    }
    out.push_back('"');
    return out;
}

uint64_t NumberToU64(const Value& v, uint64_t fallback) {
    if (v.type != Value::Type::Number) return fallback;
    try {
        return std::stoull(v.number);
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace asar::json
