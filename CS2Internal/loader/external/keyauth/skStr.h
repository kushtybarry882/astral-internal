#pragma once

#include <string>
#include <array>
#include <cstddef>
#include <cstdint>

namespace skc {

// N-C-1: per-string key derived from the string's own content (FNV-1a of the
// plaintext), XORed byte-by-byte. No shared thread_local buffer, no constant
// key, no volatile-in-consteval theatre. Matches loader/skStr.h.
template<std::size_t N>
struct xor_string {
    std::array<char, N> data{};
    std::uint8_t key;

    consteval xor_string(const char* str) : key(0) {
        // Derive per-string key via FNV-1a of the plaintext.
        std::uint32_t hash = 2166136261u;
        for (std::size_t i = 0; i < N; ++i) {
            hash ^= static_cast<std::uint8_t>(str[i]);
            hash *= 16777619u;
        }
        key = static_cast<std::uint8_t>(hash & 0xFF) | 0x01; // ensure non-zero key
        for (std::size_t i = 0; i < N; ++i) {
            data[i] = static_cast<char>(str[i] ^ key);
        }
    }

    [[nodiscard]] std::string decrypt() const {
        std::string result;
        result.reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            result.push_back(static_cast<char>(data[i] ^ key));
        }
        return result;
    }
};

template<std::size_t N>
consteval xor_string<N> make_xor_string(const char (&str)[N]) {
    return xor_string<N>(str);
}

} // namespace skc

#define skCrypt(str) skc::make_xor_string(str)
#define skStr(str) skc::make_xor_string(str)
