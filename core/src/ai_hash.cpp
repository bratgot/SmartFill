// nuke-ai-fill / core / ai_hash.cpp
//
// SHA-256 implementation following FIPS 180-4.
// Reference: NIST FIPS PUB 180-4 (March 2012).
//
// Strict ASCII per NDK_NOTES section 6.1.

#include "ai_hash.h"

#include <cstring>

namespace nukeaifill {

namespace {

// SHA-256 round constants (first 32 bits of fractional parts of cube
// roots of the first 64 primes 2..311).
constexpr uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

// SHA-256 initial hash values (first 32 bits of fractional parts of
// square roots of the first 8 primes 2..19).
constexpr uint32_t H_INIT[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
};

inline uint32_t rotr(uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32u - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t ep0(uint32_t x) noexcept {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t ep1(uint32_t x) noexcept {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t sig0(uint32_t x) noexcept {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t sig1(uint32_t x) noexcept {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

} // anonymous namespace

// ----------------------------------------------------------------------

Sha256::Sha256() {
    reset();
}

void Sha256::reset() {
    std::memcpy(state_, H_INIT, sizeof(H_INIT));
    bit_length_ = 0;
    buffer_len_ = 0;
}

void Sha256::transform(const uint8_t* chunk) {
    uint32_t w[64];

    // Load 16 big-endian words from the chunk.
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(chunk[i * 4 + 0]) << 24)
             | (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16)
             | (static_cast<uint32_t>(chunk[i * 4 + 2]) <<  8)
             | (static_cast<uint32_t>(chunk[i * 4 + 3]));
    }

    // Extend to 64 words.
    for (int i = 16; i < 64; ++i) {
        w[i] = sig1(w[i - 2]) + w[i - 7] + sig0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + ep1(e) + ch(e, f, g) + K[i] + w[i];
        uint32_t t2 = ep0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);

    while (len > 0) {
        size_t take = 64 - buffer_len_;
        if (take > len) take = len;

        std::memcpy(buffer_ + buffer_len_, p, take);
        buffer_len_ += take;
        p           += take;
        len         -= take;
        bit_length_ += static_cast<uint64_t>(take) * 8;

        if (buffer_len_ == 64) {
            transform(buffer_);
            buffer_len_ = 0;
        }
    }
}

Sha256::Digest Sha256::finalize() {
    // Append 0x80, then zeros until length mod 64 == 56, then 8 bytes
    // of message length in bits big-endian.
    const uint64_t total_bits = bit_length_;

    buffer_[buffer_len_++] = 0x80;

    if (buffer_len_ > 56) {
        // Not enough room for length; pad to end and process, then
        // start a new chunk of zeros.
        while (buffer_len_ < 64) buffer_[buffer_len_++] = 0;
        transform(buffer_);
        buffer_len_ = 0;
    }
    while (buffer_len_ < 56) buffer_[buffer_len_++] = 0;

    // 8 bytes of total bit-length big-endian.
    for (int i = 7; i >= 0; --i) {
        buffer_[buffer_len_++] =
            static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFFu);
    }
    transform(buffer_);

    Digest out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = static_cast<uint8_t>((state_[i] >>  8) & 0xFFu);
        out[i * 4 + 3] = static_cast<uint8_t>( state_[i]        & 0xFFu);
    }

    reset();
    return out;
}

Sha256::Digest Sha256::digest(const void* data, size_t len) {
    Sha256 h;
    h.update(data, len);
    return h.finalize();
}

std::string Sha256::to_hex(const Digest& d) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < kDigestSize; ++i) {
        out[i * 2 + 0] = hex[(d[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[ d[i]       & 0x0F];
    }
    return out;
}

} // namespace nukeaifill
