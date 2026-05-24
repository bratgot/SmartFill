// nuke-ai-fill / core / ai_cache.cpp
//
// Implementation of CacheKeyBuilder + AiCache. See ai_cache.h for
// design notes.
//
// Path conventions per NDK_NOTES section 4.2: store and return all
// paths with forward slashes, even on Windows.
//
// We use std::filesystem with error_code overloads exclusively, so
// no exception escapes from any cache call. The caller's worker
// wraps it in try/catch as a backstop.

#include "ai_cache.h"
#include "ai_hash.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace nukeaifill {

// ----------------------------------------------------------------------
// CacheKeyBuilder::Impl
// ----------------------------------------------------------------------

struct CacheKeyBuilder::Impl {
    Sha256 hasher;

    // Domain separators: we prefix each typed field with a short tag
    // so that add_string("123") and add(int 123) cannot collide. The
    // tag is one byte; integer keys collapse to little-endian binary.
    enum Tag : uint8_t {
        TAG_STRING  = 's',
        TAG_INT32   = 'i',
        TAG_INT64   = 'I',
        TAG_FLOAT   = 'f',
        TAG_DOUBLE  = 'd',
        TAG_BOOL    = 'b',
        TAG_OP_HASH = 'h',
    };

    void put_tag(Tag t) {
        uint8_t v = static_cast<uint8_t>(t);
        hasher.update(&v, 1);
    }

    void put_u32(uint32_t v) {
        uint8_t bytes[4] = {
            static_cast<uint8_t>( v        & 0xFFu),
            static_cast<uint8_t>((v >>  8) & 0xFFu),
            static_cast<uint8_t>((v >> 16) & 0xFFu),
            static_cast<uint8_t>((v >> 24) & 0xFFu),
        };
        hasher.update(bytes, 4);
    }
};

CacheKeyBuilder::CacheKeyBuilder() : impl_(std::make_unique<Impl>()) {}
CacheKeyBuilder::~CacheKeyBuilder() = default;

void CacheKeyBuilder::add(const std::string& s) {
    impl_->put_tag(Impl::TAG_STRING);
    impl_->put_u32(static_cast<uint32_t>(s.size()));
    impl_->hasher.update(s.data(), s.size());
}

void CacheKeyBuilder::add(const char* s) {
    add(std::string(s ? s : ""));
}

void CacheKeyBuilder::add(int32_t v) {
    impl_->put_tag(Impl::TAG_INT32);
    impl_->put_u32(static_cast<uint32_t>(v));
}

void CacheKeyBuilder::add(int64_t v) {
    impl_->put_tag(Impl::TAG_INT64);
    impl_->put_u32(static_cast<uint32_t>( v        & 0xFFFFFFFFull));
    impl_->put_u32(static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
}

void CacheKeyBuilder::add(float v) {
    impl_->put_tag(Impl::TAG_FLOAT);
    static_assert(sizeof(float) == 4, "float must be 4 bytes");
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    impl_->put_u32(bits);
}

void CacheKeyBuilder::add(double v) {
    impl_->put_tag(Impl::TAG_DOUBLE);
    static_assert(sizeof(double) == 8, "double must be 8 bytes");
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    impl_->put_u32(static_cast<uint32_t>( bits        & 0xFFFFFFFFull));
    impl_->put_u32(static_cast<uint32_t>((bits >> 32) & 0xFFFFFFFFull));
}

void CacheKeyBuilder::add(bool v) {
    impl_->put_tag(Impl::TAG_BOOL);
    uint8_t b = v ? 1u : 0u;
    impl_->hasher.update(&b, 1);
}

void CacheKeyBuilder::add_op_hash(uint64_t h) {
    impl_->put_tag(Impl::TAG_OP_HASH);
    impl_->put_u32(static_cast<uint32_t>( h        & 0xFFFFFFFFull));
    impl_->put_u32(static_cast<uint32_t>((h >> 32) & 0xFFFFFFFFull));
}

std::string CacheKeyBuilder::finalize() {
    auto digest = impl_->hasher.finalize();
    return Sha256::to_hex(digest);
}

// ----------------------------------------------------------------------
// AiCache
// ----------------------------------------------------------------------

namespace {

// Normalize a path string: forward slashes, no trailing whitespace,
// no trailing slash.
std::string normalize_dir(std::string s) {
    // Strip trailing whitespace and newlines (File_knob may append).
    while (!s.empty() && (s.back() == ' '  || s.back() == '\t' ||
                          s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    // Backslashes to forward.
    for (auto& c : s) {
        if (c == '\\') c = '/';
    }
    // Strip trailing slash.
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

bool ensure_dir(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    if (ec) return false;
    return fs::is_directory(fs::u8path(dir), ec);
}

bool is_hex_key(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

} // anonymous namespace

AiCache::AiCache(std::string cache_dir)
    : cache_dir_(normalize_dir(std::move(cache_dir))) {
    ensure_dir(cache_dir_);
}

std::string AiCache::exr_path_for(const std::string& key) const {
    return cache_dir_ + "/" + key + ".exr";
}

std::string AiCache::json_path_for(const std::string& key) const {
    return cache_dir_ + "/" + key + ".json";
}

bool AiCache::has(const std::string& key) const {
    if (!is_hex_key(key)) return false;
    std::error_code ec;
    return fs::exists(fs::u8path(exr_path_for(key)), ec) && !ec;
}

CachedResult AiCache::lookup(const std::string& key) const {
    CachedResult r;
    if (!has(key)) return r;
    r.hit      = true;
    r.exr_path = exr_path_for(key);
    // width/height left at 0; caller already knows expected dimensions
    // from its knob state. If they ever need to verify, OpenEXR's
    // header read is cheap (a few KiB).
    return r;
}

bool AiCache::write_sidecar(const std::string& key,
                            const std::string& json_blob) {
    if (!is_hex_key(key)) return false;
    if (!ensure_dir(cache_dir_)) return false;

    const std::string path = json_path_for(key);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(json_blob.data(), static_cast<std::streamsize>(json_blob.size()));
    return out.good();
}

int AiCache::clear() {
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(cache_dir_), ec);
    if (ec) return 0;

    int removed = 0;
    for (const auto& entry : it) {
        const auto& p = entry.path();
        const auto stem = p.stem().u8string();
        // u8string returns std::string in C++17, std::u8string in C++20.
        // Both are byte-compatible with our ASCII-hex stem check.
        std::string stem_s(reinterpret_cast<const char*>(stem.data()),
                           stem.size());
        if (!is_hex_key(stem_s)) continue;

        const auto ext = p.extension().u8string();
        std::string ext_s(reinterpret_cast<const char*>(ext.data()),
                          ext.size());
        if (ext_s != ".exr" && ext_s != ".json") continue;

        std::error_code rm_ec;
        if (fs::remove(p, rm_ec) && !rm_ec) {
            ++removed;
        }
    }
    return removed;
}

int AiCache::prune(int64_t max_age_seconds, int max_entries) {
    using clock = std::chrono::system_clock;
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(cache_dir_), ec);
    if (ec) return 0;

    struct Entry {
        fs::path                              path;
        std::chrono::system_clock::time_point mtime;
    };
    std::vector<Entry> exr_entries;
    exr_entries.reserve(64);

    for (const auto& entry : it) {
        const auto& p = entry.path();
        const auto ext = p.extension().u8string();
        std::string ext_s(reinterpret_cast<const char*>(ext.data()),
                          ext.size());
        if (ext_s != ".exr") continue;

        const auto stem = p.stem().u8string();
        std::string stem_s(reinterpret_cast<const char*>(stem.data()),
                           stem.size());
        if (!is_hex_key(stem_s)) continue;

        std::error_code ts_ec;
        auto ft = fs::last_write_time(p, ts_ec);
        if (ts_ec) continue;
        // Convert file_time_type to system_clock::time_point. C++17
        // does not provide a direct cast, but a duration_cast through
        // both clocks' epochs is close enough for pruning purposes.
        auto sctp = std::chrono::time_point_cast<clock::duration>(
            ft - decltype(ft)::clock::now() + clock::now());
        exr_entries.push_back({p, sctp});
    }

    const auto now = clock::now();
    int removed = 0;

    // Delete entries older than max_age_seconds.
    if (max_age_seconds > 0) {
        for (auto it2 = exr_entries.begin(); it2 != exr_entries.end(); ) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - it2->mtime).count();
            if (age > max_age_seconds) {
                std::error_code rm_ec;
                fs::remove(it2->path, rm_ec);
                // Also remove the sidecar if present.
                fs::path side = it2->path;
                side.replace_extension(".json");
                std::error_code rm_ec2;
                fs::remove(side, rm_ec2);
                ++removed;
                it2 = exr_entries.erase(it2);
            } else {
                ++it2;
            }
        }
    }

    // Enforce max_entries: keep newest, drop oldest.
    if (max_entries > 0 && static_cast<int>(exr_entries.size()) > max_entries) {
        std::sort(exr_entries.begin(), exr_entries.end(),
                  [](const Entry& a, const Entry& b) {
                      return a.mtime > b.mtime;
                  });
        for (size_t i = static_cast<size_t>(max_entries);
             i < exr_entries.size(); ++i) {
            std::error_code rm_ec;
            fs::remove(exr_entries[i].path, rm_ec);
            fs::path side = exr_entries[i].path;
            side.replace_extension(".json");
            std::error_code rm_ec2;
            fs::remove(side, rm_ec2);
            ++removed;
        }
    }

    return removed;
}

} // namespace nukeaifill
