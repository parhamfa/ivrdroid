#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ivrdroid {

class Sha256 {
public:
    Sha256();
    void Update(const void* data, size_t size);
    std::array<uint8_t, 32> Finish();

private:
    void Transform(const uint8_t block[64]);

    std::array<uint32_t, 8> state_;
    std::array<uint8_t, 64> buffer_ {};
    uint64_t totalBytes_ = 0;
    size_t bufferBytes_ = 0;
    bool finished_ = false;
};

std::string Sha256Hex(const void* data, size_t size);
bool Sha256File(const char* path, off_t maximumBytes, std::string* hexDigest);
bool IsLowerHexSha256(const std::string& value);

}  // namespace ivrdroid
