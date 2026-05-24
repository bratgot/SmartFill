// nuke-ai-fill / core / image_cache.cpp
//
// See image_cache.h. Strict ASCII per NDK_NOTES 6.1.

#include "image_cache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace nukeaifill {

namespace {

constexpr char kMagic[8] = { 'A', 'I', 'F', 'I', 'L', 'L', '\0', '\0' };
constexpr size_t kHeaderSize = 28;

// Defensive sanity caps. A 16K x 16K RGBA image is ~4 GB which is
// already past what makes sense for an inpainting cache; we cap
// generously and reject anything weirder.
constexpr int kMaxDim      = 32768;
constexpr int kMaxChannels = 8;

struct Header {
    char     magic[8];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t reserved;
};
static_assert(sizeof(Header) == kHeaderSize, "Header packing must be 28 bytes");

void write_u32_le(std::ostream& out, uint32_t v) {
    char b[4] = {
        static_cast<char>( v        & 0xFFu),
        static_cast<char>((v >>  8) & 0xFFu),
        static_cast<char>((v >> 16) & 0xFFu),
        static_cast<char>((v >> 24) & 0xFFu),
    };
    out.write(b, 4);
}

bool read_u32_le(std::istream& in, uint32_t& v) {
    unsigned char b[4];
    in.read(reinterpret_cast<char*>(b), 4);
    if (!in.good()) return false;
    v = static_cast<uint32_t>(b[0])
      | (static_cast<uint32_t>(b[1]) <<  8)
      | (static_cast<uint32_t>(b[2]) << 16)
      | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool read_header(std::istream& in, Header& h, std::string& err) {
    in.read(h.magic, 8);
    if (!in.good()) { err = "short read on magic"; return false; }
    if (std::memcmp(h.magic, kMagic, 8) != 0) {
        err = "bad magic (file is not .aifill format)";
        return false;
    }
    if (!read_u32_le(in, h.version) ||
        !read_u32_le(in, h.width) ||
        !read_u32_le(in, h.height) ||
        !read_u32_le(in, h.channels) ||
        !read_u32_le(in, h.reserved)) {
        err = "short read on header fields";
        return false;
    }
    if (h.version != kImageCacheVersion) {
        err = "unsupported file version " + std::to_string(h.version);
        return false;
    }
    if (h.width == 0 || h.height == 0 || h.channels == 0 ||
        h.width  > static_cast<uint32_t>(kMaxDim) ||
        h.height > static_cast<uint32_t>(kMaxDim) ||
        h.channels > static_cast<uint32_t>(kMaxChannels)) {
        err = "dimensions out of range";
        return false;
    }
    return true;
}

} // anonymous namespace

bool image_cache_write(const std::string& path,
                       const float*       hwc_pixels,
                       int                width,
                       int                height,
                       int                channels)
{
    if (!hwc_pixels) return false;
    if (width <= 0 || width > kMaxDim) return false;
    if (height <= 0 || height > kMaxDim) return false;
    if (channels <= 0 || channels > kMaxChannels) return false;

    // Write to a temp file first, then atomically rename. Two reasons:
    // (1) crash or process kill mid-write leaves no corrupted cache file
    // (2) a concurrent reader either sees the old file or the new one,
    //     never a half-written one.
    const std::string tmp_path = path + ".tmp";

    std::error_code ec;
    fs::path tmp_fs = fs::u8path(tmp_path);
    fs::create_directories(tmp_fs.parent_path(), ec);
    // ec ignored - parent may already exist, which is fine. fail-fast
    // on the open below if not.

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        out.write(kMagic, 8);
        write_u32_le(out, kImageCacheVersion);
        write_u32_le(out, static_cast<uint32_t>(width));
        write_u32_le(out, static_cast<uint32_t>(height));
        write_u32_le(out, static_cast<uint32_t>(channels));
        write_u32_le(out, 0);

        const std::streamsize bytes =
            static_cast<std::streamsize>(width) *
            static_cast<std::streamsize>(height) *
            static_cast<std::streamsize>(channels) *
            sizeof(float);
        out.write(reinterpret_cast<const char*>(hwc_pixels), bytes);

        if (!out.good()) {
            // Clean up temp on failure.
            out.close();
            std::error_code rm_ec;
            fs::remove(tmp_fs, rm_ec);
            return false;
        }
    }  // close ofstream before rename

    // Atomic-replace. std::filesystem::rename overwrites on POSIX;
    // on Windows it does not, so we remove the destination first.
    fs::path dst_fs = fs::u8path(path);
    std::error_code rm_ec;
    fs::remove(dst_fs, rm_ec);
    // ec ignored - file may not exist yet

    std::error_code rn_ec;
    fs::rename(tmp_fs, dst_fs, rn_ec);
    if (rn_ec) {
        std::error_code rm2_ec;
        fs::remove(tmp_fs, rm2_ec);
        return false;
    }
    return true;
}

ImageCacheReadResult image_cache_read(const std::string& path)
{
    ImageCacheReadResult r;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        r.error = "cannot open file";
        return r;
    }

    Header h{};
    if (!read_header(in, h, r.error)) {
        return r;
    }

    const size_t n = static_cast<size_t>(h.width) *
                     static_cast<size_t>(h.height) *
                     static_cast<size_t>(h.channels);
    r.pixels.resize(n);

    const std::streamsize bytes =
        static_cast<std::streamsize>(n) * sizeof(float);
    in.read(reinterpret_cast<char*>(r.pixels.data()), bytes);
    if (in.gcount() != bytes) {
        r.error = "short read on pixel data";
        r.pixels.clear();
        return r;
    }

    r.width    = static_cast<int>(h.width);
    r.height   = static_cast<int>(h.height);
    r.channels = static_cast<int>(h.channels);
    r.ok       = true;
    return r;
}

bool image_cache_inspect(const std::string& path,
                         int*               out_width,
                         int*               out_height,
                         int*               out_channels)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    Header h{};
    std::string err;
    if (!read_header(in, h, err)) return false;
    if (out_width)    *out_width    = static_cast<int>(h.width);
    if (out_height)   *out_height   = static_cast<int>(h.height);
    if (out_channels) *out_channels = static_cast<int>(h.channels);
    return true;
}

} // namespace nukeaifill
