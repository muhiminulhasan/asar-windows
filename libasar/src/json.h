// json.h - minimal JSON DOM used for ASAR headers.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asar::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    std::string number;                  // raw number token
    std::wstring str;                    // decoded string value
    std::vector<Value> array;
    std::vector<std::pair<std::wstring, Value>> members;

    const Value* Find(std::wstring_view key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : members)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

// Throws Error (asar::Error) with position info on malformed input.
Value Parse(std::string_view utf8);

// Escapes and quotes a wide string as a JSON string literal (UTF-8 output).
std::string WriteString(std::wstring_view s);

uint64_t NumberToU64(const Value& v, uint64_t fallback = 0);

}  // namespace asar::json
