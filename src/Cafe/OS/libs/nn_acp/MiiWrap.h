// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace nn::acp::MiiCrypto {
using Block = std::array<std::uint8_t, 16>;
using Key = Block;
constexpr std::size_t PlainSize = 96, WrappedSize = 112;

class Cipher {
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};
    bool ready{};
public:
    explicit Cipher(const Key& key) {
        ready = context && EVP_EncryptInit_ex(context.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr) == 1 &&
            EVP_CIPHER_CTX_set_padding(context.get(), 0) == 1;
    }
    bool Encrypt(const Block& input, Block& output) {
        int size{};
        return ready && EVP_EncryptUpdate(context.get(), output.data(), &size, input.data(), 16) == 1 && size == 16;
    }
};

inline Block Counter(std::span<const std::uint8_t, 8> nonce, std::uint8_t number) {
    Block result{};
    result[0] = 2; // Twelve-byte nonce and three-byte CCM length/counter.
    std::copy(nonce.begin(), nonce.end(), result.begin() + 1);
    result[15] = number;
    return result;
}

inline bool Transform(Cipher& cipher, std::span<const std::uint8_t, 8> nonce,
                      const std::array<std::uint8_t, 88>& input, std::array<std::uint8_t, 88>& output) {
    for (std::size_t offset = 0; offset < input.size(); offset += 16) {
        Block stream{};
        if (!cipher.Encrypt(Counter(nonce, static_cast<std::uint8_t>(offset / 16 + 1)), stream)) return false;
        for (std::size_t i = 0; i < std::min<std::size_t>(16, input.size() - offset); ++i)
            output[offset + i] = input[offset + i] ^ stream[i];
    }
    return true;
}

inline bool Authenticate(Cipher& cipher, std::span<const std::uint8_t, 8> nonce,
                         const std::array<std::uint8_t, 88>& plain, Block& tag) {
    auto initial = Counter(nonce, 96);
    initial[0] = 0x3A; // Sixteen-byte MAC; native CCM authenticates the padded 96-byte length.
    Block state{};
    if (!cipher.Encrypt(initial, state)) return false;
    for (std::size_t offset = 0; offset < 96; offset += 16) {
        Block input = state;
        for (std::size_t i = 0; i < 16 && offset + i < plain.size(); ++i) input[i] ^= plain[offset + i];
        if (!cipher.Encrypt(input, state)) return false;
    }
    Block mask{};
    if (!cipher.Encrypt(Counter(nonce, 0), mask)) return false;
    for (std::size_t i = 0; i < 16; ++i) tag[i] = state[i] ^ mask[i];
    return true;
}

inline bool Wrap(const Key& key, std::span<const std::uint8_t> input, std::span<std::uint8_t> output) {
    if (input.size() != PlainSize || output.size() != WrappedSize) return false;
    std::array<std::uint8_t, WrappedSize> result{};
    std::copy_n(input.begin() + 12, 8, result.begin());
    const std::span<const std::uint8_t, 8> nonce(result.data(), 8);
    std::array<std::uint8_t, 88> plain{}, encrypted{};
    std::copy_n(input.begin(), 12, plain.begin());
    std::copy(input.begin() + 20, input.end(), plain.begin() + 12);
    Cipher cipher(key);
    Block tag{};
    if (!Transform(cipher, nonce, plain, encrypted) || !Authenticate(cipher, nonce, plain, tag)) return false;
    std::copy(encrypted.begin(), encrypted.end(), result.begin() + 8);
    std::copy(tag.begin(), tag.end(), result.begin() + 96);
    std::copy(result.begin(), result.end(), output.begin());
    return true;
}

inline bool Unwrap(const Key& key, std::span<const std::uint8_t> input, std::span<std::uint8_t> output) {
    if (input.size() != WrappedSize || output.size() != PlainSize) return false;
    const std::span<const std::uint8_t, 8> nonce(input.data(), 8);
    std::array<std::uint8_t, 88> encrypted{}, plain{};
    std::copy_n(input.begin() + 8, 88, encrypted.begin());
    Cipher cipher(key);
    Block tag{};
    if (!Transform(cipher, nonce, encrypted, plain) || !Authenticate(cipher, nonce, plain, tag) ||
        CRYPTO_memcmp(tag.data(), input.data() + 96, tag.size()) != 0) return false;
    std::array<std::uint8_t, PlainSize> result{};
    std::copy_n(plain.begin(), 12, result.begin());
    std::copy(nonce.begin(), nonce.end(), result.begin() + 12);
    std::copy(plain.begin() + 12, plain.end(), result.begin() + 20);
    std::copy(result.begin(), result.end(), output.begin());
    return true;
}
}
