// nuke-ai-fill / core / image_cache.h
//
// Simple binary float-image format for cache files. Chose this over
// OpenEXR for chunk C scope: no extra dependencies, trivial format,
// fast I/O, and the only consumer is the plugin itself (artists
// don't need to open these files in other tools).
//
// File layout (little-endian throughout):
//   bytes 0-7:   magic 'AIFILL\0\0'
//   bytes 8-11:  uint32_t version (current: 1)
//   bytes 12-15: uint32_t width
//   bytes 16-19: uint32_t height
//   bytes 20-23: uint32_t channels
//   bytes 24-27: uint32_t reserved (0)
//   bytes 28-..: width * height * channels float32 values, HWC layout
//
// Path conventions per NDK_NOTES 4: forward slashes, no trailing
// whitespace. Strict ASCII per NDK_NOTES 6.1.

#ifndef NUKE_AI_FILL_IMAGE_CACHE_H
#define NUKE_AI_FILL_IMAGE_CACHE_H

#include <cstdint>
#include <string>
#include <vector>

namespace nukeaifill {

constexpr uint32_t kImageCacheVersion = 1;

struct ImageCacheReadResult {
    bool               ok        = false;
    int                width     = 0;
    int                height    = 0;
    int                channels  = 0;
    std::vector<float> pixels;   // HWC interleaved
    std::string        error;    // populated on ok==false
};

// Write a HWC float32 image to disk. Returns true on success.
// On failure, leaves no partial file on disk (writes to a .tmp then
// renames atomically).
bool image_cache_write(const std::string& path,
                       const float*       hwc_pixels,
                       int                width,
                       int                height,
                       int                channels);

// Read an image written by image_cache_write. Returns a result struct
// with ok=true on success and ok=false with error populated otherwise.
ImageCacheReadResult image_cache_read(const std::string& path);

// Quick header inspection without loading pixel data. Used by the Op
// to verify cached dimensions match expected before issuing the full
// read. Returns false on any error.
bool image_cache_inspect(const std::string& path,
                         int*               out_width,
                         int*               out_height,
                         int*               out_channels);

} // namespace nukeaifill

#endif // NUKE_AI_FILL_IMAGE_CACHE_H
