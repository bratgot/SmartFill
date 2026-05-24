// nuke-ai-fill / core / ai_hash.h
//
// Small embedded SHA-256 implementation. FIPS 180-4 reference.
// No third-party crypto dependency, no Windows BCrypt link.
//
// Used to derive content-addressed cache keys from input state.
// Crypto strength is not load-bearing for security here - we just
// need a wide hash with no realistic accidental collisions for our
// caching purposes. SHA-256 is overkill but cheap and well-known.
//
// Strict ASCII per NDK_NOTES section 6.1 (no Greek letter symbols
// in the code - we use EP0/EP1/SIG0/SIG1 not Sigma/sigma).

#ifndef NUKE_AI_FILL_AI_HASH_H
#define NUKE_AI_FILL_AI_HASH_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nukeaifill {

// ----------------------------------------------------------------------
// Streaming SHA-256
// ----------------------------------------------------------------------
//
// Usage:
//   Sha256 h;
//   h.update(buf, len);
//   h.update(more, more_len);
//   auto bytes = h.finalize();         // std::array<uint8_t, 32>
//   std::string hex = Sha256::to_hex(bytes);
//
// After finalize() the object is reset and ready for a new digest.

class Sha256 {
public:
    static constexpr size_t kDigestSize = 32;
    using Digest = std::array<uint8_t, kDigestSize>;

    Sha256();

    void reset();

    void update(const void* data, size_t len);
    void update(const std::string& s) { update(s.data(), s.size()); }

    // Finalize, return digest, and reset for reuse.
    Digest finalize();

    // Convenience one-shot.
    static Digest digest(const void* data, size_t len);

    static std::string to_hex(const Digest& d);

private:
    void transform(const uint8_t* chunk);

    uint32_t state_[8];     // H0..H7
    uint64_t bit_length_;   // total message length in bits
    uint8_t  buffer_[64];   // current chunk being filled
    size_t   buffer_len_;   // bytes filled in buffer_
};

} // namespace nukeaifill

#endif // NUKE_AI_FILL_AI_HASH_H
