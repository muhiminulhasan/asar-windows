// sha256.h - compact SHA-256 for ASAR integrity data.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace asar {

class Sha256 {
public:
    Sha256();
    void Update(const void* data, size_t len);
    std::array<uint8_t, 32> Finish();

private:
    void ProcessBlock(const uint8_t* block);
    uint32_t m_state[8];
    uint8_t m_buffer[64];
    uint64_t m_bytes;
    size_t m_bufferLen;
};

std::string ToHex(const uint8_t* data, size_t len);

}  // namespace asar
