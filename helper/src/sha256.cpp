#include "sha256.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace ivrdroid {
namespace {

constexpr std::array<uint32_t, 64> kRoundConstants {{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
}};

uint32_t RotateRight(uint32_t value, unsigned int amount) {
    return (value >> amount) | (value << (32U - amount));
}

uint32_t ReadBigEndian(const uint8_t* value) {
    return
        (static_cast<uint32_t>(value[0]) << 24U) |
        (static_cast<uint32_t>(value[1]) << 16U) |
        (static_cast<uint32_t>(value[2]) << 8U) |
        static_cast<uint32_t>(value[3]);
}

void WriteBigEndian(uint32_t value, uint8_t* output) {
    output[0] = static_cast<uint8_t>(value >> 24U);
    output[1] = static_cast<uint8_t>(value >> 16U);
    output[2] = static_cast<uint8_t>(value >> 8U);
    output[3] = static_cast<uint8_t>(value);
}

}  // namespace

Sha256::Sha256()
    : state_ {{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    }} {}

void Sha256::Transform(const uint8_t block[64]) {
    std::array<uint32_t, 64> words {};
    for (size_t index = 0; index < 16; ++index) {
        words[index] = ReadBigEndian(block + index * 4);
    }
    for (size_t index = 16; index < words.size(); ++index) {
        const uint32_t s0 =
            RotateRight(words[index - 15], 7) ^
            RotateRight(words[index - 15], 18) ^
            (words[index - 15] >> 3U);
        const uint32_t s1 =
            RotateRight(words[index - 2], 17) ^
            RotateRight(words[index - 2], 19) ^
            (words[index - 2] >> 10U);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];
    for (size_t index = 0; index < words.size(); ++index) {
        const uint32_t sum1 =
            RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const uint32_t choice = (e & f) ^ ((~e) & g);
        const uint32_t temporary1 =
            h + sum1 + choice + kRoundConstants[index] + words[index];
        const uint32_t sum0 =
            RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::Update(const void* data, size_t size) {
    if (finished_ || size == 0) return;
    const auto* bytes = static_cast<const uint8_t*>(data);
    totalBytes_ += size;
    while (size > 0) {
        const size_t available = buffer_.size() - bufferBytes_;
        const size_t count = size < available ? size : available;
        std::memcpy(buffer_.data() + bufferBytes_, bytes, count);
        bufferBytes_ += count;
        bytes += count;
        size -= count;
        if (bufferBytes_ == buffer_.size()) {
            Transform(buffer_.data());
            bufferBytes_ = 0;
        }
    }
}

std::array<uint8_t, 32> Sha256::Finish() {
    if (!finished_) {
        const uint64_t totalBits = totalBytes_ * 8U;
        buffer_[bufferBytes_++] = 0x80U;
        if (bufferBytes_ > 56) {
            while (bufferBytes_ < buffer_.size()) buffer_[bufferBytes_++] = 0;
            Transform(buffer_.data());
            bufferBytes_ = 0;
        }
        while (bufferBytes_ < 56) buffer_[bufferBytes_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[bufferBytes_++] = static_cast<uint8_t>(totalBits >> shift);
        }
        Transform(buffer_.data());
        bufferBytes_ = 0;
        finished_ = true;
    }
    std::array<uint8_t, 32> digest {};
    for (size_t index = 0; index < state_.size(); ++index) {
        WriteBigEndian(state_[index], digest.data() + index * 4);
    }
    return digest;
}

std::string Sha256Hex(const void* data, size_t size) {
    Sha256 hash;
    hash.Update(data, size);
    const auto digest = hash.Finish();
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const uint8_t byte : digest) {
        result.push_back(alphabet[byte >> 4U]);
        result.push_back(alphabet[byte & 0x0fU]);
    }
    return result;
}

bool Sha256File(const char* path, off_t maximumBytes, std::string* hexDigest) {
    if (path == nullptr || hexDigest == nullptr || maximumBytes <= 0) return false;
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    struct stat state {};
    bool valid =
        fstat(descriptor, &state) == 0 &&
        S_ISREG(state.st_mode) &&
        state.st_size >= 0 &&
        state.st_size <= maximumBytes;
    Sha256 hash;
    std::array<uint8_t, 16 * 1024> buffer {};
    off_t total = 0;
    while (valid) {
        ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            valid = false;
            break;
        }
        total += count;
        if (total > maximumBytes) {
            valid = false;
            break;
        }
        hash.Update(buffer.data(), static_cast<size_t>(count));
    }
    valid = valid && total == state.st_size;
    close(descriptor);
    if (!valid) return false;
    const auto digest = hash.Finish();
    constexpr char alphabet[] = "0123456789abcdef";
    hexDigest->clear();
    hexDigest->reserve(64);
    for (const uint8_t byte : digest) {
        hexDigest->push_back(alphabet[byte >> 4U]);
        hexDigest->push_back(alphabet[byte & 0x0fU]);
    }
    return true;
}

bool IsLowerHexSha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

}  // namespace ivrdroid
