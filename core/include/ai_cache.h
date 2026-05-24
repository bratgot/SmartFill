// nuke-ai-fill / core / ai_cache.h
//
// Content-addressed on-disk cache for inference results.
//
// Key (string) is a hex SHA-256 derived from the full input state:
//   - sampled hash of source image pixels (64 evenly-spaced samples)
//   - sampled hash of mask pixels
//   - serialized knob state (prompt, steps, seed, model path, etc.)
//   - model file path and mtime
//
// Value is a half-float EXR at <cache_dir>/<key>.exr plus a JSON
// sidecar <cache_dir>/<key>.json recording the knob state at bake
// time (for forensics).
//
// Strict ASCII per NDK_NOTES section 6.1.
//
// Path conventions: all paths stored with forward slashes (NDK_NOTES
// section 4.2). Convert from std::filesystem::path via
// .generic_string(), not .string(). Strip trailing whitespace from
// any user-supplied File_knob value before passing in.

#ifndef NUKE_AI_FILL_AI_CACHE_H
#define NUKE_AI_FILL_AI_CACHE_H

#include <cstdint>
#include <memory>
#include <string>

namespace nukeaifill {

// ----------------------------------------------------------------------
// Cache key construction
// ----------------------------------------------------------------------
//
// CacheKeyBuilder accumulates state into an internal SHA-256. Order
// matters: the same fields fed in the same order produce the same key.
//
// Usage from an Op's _validate / hash_op:
//
//   CacheKeyBuilder kb;
//   kb.add("AIGenerate.v1");                // schema tag
//   kb.add_image_sampled(src_pixels, w, h); // 64-sample digest
//   kb.add_image_sampled(mask_pixels, w, h);
//   kb.add(prompt);
//   kb.add(neg_prompt);
//   kb.add(static_cast<int32_t>(steps));
//   kb.add(cfg_scale);
//   kb.add(static_cast<int64_t>(seed));
//   kb.add(model_path);
//   kb.add(model_mtime_unix);
//   std::string key = kb.finalize();

class CacheKeyBuilder {
public:
    CacheKeyBuilder();
    ~CacheKeyBuilder();

    CacheKeyBuilder(const CacheKeyBuilder&)            = delete;
    CacheKeyBuilder& operator=(const CacheKeyBuilder&) = delete;

    void add(const std::string& s);
    void add(const char* s);
    void add(int32_t v);
    void add(int64_t v);
    void add(float v);
    void add(double v);
    void add(bool v);

    // Add a Nuke upstream hash. Pass the value of
    // input(n)->hash_op(hash_context).getHash() (or equivalent on
    // the DD::Image::Op API). This is THE correct way to fingerprint
    // input image state in a Nuke pipeline: Nuke's hash already
    // incorporates all upstream node parameters, time, and so on.
    //
    // Do NOT hash pixel contents directly - it is slow (4K image is
    // ~130 MB to SHA-256, ~400 ms per validate call) and redundant
    // with the upstream hash.
    void add_op_hash(uint64_t h);

    // Produce the hex string. Builder may be reused after this; finalize
    // resets internal state.
    std::string finalize();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ----------------------------------------------------------------------
// Cache
// ----------------------------------------------------------------------

struct CachedResult {
    std::string exr_path;   // forward-slash absolute path, "" if miss
    int         width  = 0;
    int         height = 0;
    bool        hit    = false;
};

class AiCache {
public:
    // cache_dir is created if it does not exist. Pass an absolute path
    // with forward slashes.
    explicit AiCache(std::string cache_dir);

    bool has(const std::string& key) const;

    // Returns hit=false if missing. Does NOT read the file; that
    // happens later in renderStripe via the standard EXR reader.
    CachedResult lookup(const std::string& key) const;

    // Store a result. The caller has already written the EXR; this
    // function writes the JSON sidecar and returns the canonical EXR
    // path the caller should have written to.
    //
    // To know where to write the EXR before storing, call
    // exr_path_for(key) first.
    std::string exr_path_for(const std::string& key) const;
    std::string json_path_for(const std::string& key) const;

    bool write_sidecar(const std::string& key,
                       const std::string& json_blob);

    // Delete all cached entries. Returns the number deleted.
    int clear();

    // Delete entries older than max_age_seconds, capped at max_entries.
    // Cheap LRU. Returns the number deleted.
    int prune(int64_t max_age_seconds, int max_entries);

    const std::string& cache_dir() const { return cache_dir_; }

private:
    std::string cache_dir_;
};

} // namespace nukeaifill

#endif // NUKE_AI_FILL_AI_CACHE_H
