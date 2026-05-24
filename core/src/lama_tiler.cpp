// nuke-ai-fill / core / lama_tiler.cpp
//
// See lama_tiler.h. Strict ASCII per NDK_NOTES 6.1.

#include "lama_tiler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace nukeaifill {

namespace {

constexpr int kN = LamaSession::kImageSize;  // 512

// Helper: clamp + integer cast for bilinear sampling.
inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void report_progress(ProgressFn fn, void* user, float f) {
    if (!fn) return;
    if (!fn(f, user)) {
        throw std::runtime_error("cancelled");
    }
}

} // anonymous namespace

// ----------------------------------------------------------------------
// Bilinear resize - shared shape for both RGB (3ch) and mask (1ch)
// ----------------------------------------------------------------------

template <int Channels>
static void bilinear_resize_impl(const float* src, int sw, int sh,
                                  float*       dst, int dw, int dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    // Map output pixel center to input pixel center. The (-0.5)/(+0.5)
    // half-pixel offset gives correct results for both upsampling and
    // downsampling without "edge bleed."
    const float x_scale = static_cast<float>(sw) / static_cast<float>(dw);
    const float y_scale = static_cast<float>(sh) / static_cast<float>(dh);

    for (int y = 0; y < dh; ++y) {
        float src_y = (static_cast<float>(y) + 0.5f) * y_scale - 0.5f;
        int   y0    = static_cast<int>(std::floor(src_y));
        float dy    = src_y - static_cast<float>(y0);
        int   y0c   = clamp_int(y0,     0, sh - 1);
        int   y1c   = clamp_int(y0 + 1, 0, sh - 1);

        for (int x = 0; x < dw; ++x) {
            float src_x = (static_cast<float>(x) + 0.5f) * x_scale - 0.5f;
            int   x0    = static_cast<int>(std::floor(src_x));
            float dx    = src_x - static_cast<float>(x0);
            int   x0c   = clamp_int(x0,     0, sw - 1);
            int   x1c   = clamp_int(x0 + 1, 0, sw - 1);

            const float w00 = (1.0f - dx) * (1.0f - dy);
            const float w01 =         dx  * (1.0f - dy);
            const float w10 = (1.0f - dx) *         dy;
            const float w11 =         dx  *         dy;

            for (int c = 0; c < Channels; ++c) {
                const float v00 = src[(y0c * sw + x0c) * Channels + c];
                const float v01 = src[(y0c * sw + x1c) * Channels + c];
                const float v10 = src[(y1c * sw + x0c) * Channels + c];
                const float v11 = src[(y1c * sw + x1c) * Channels + c];
                dst[(y * dw + x) * Channels + c] =
                    v00 * w00 + v01 * w01 + v10 * w10 + v11 * w11;
            }
        }
    }
}

void bilinear_resize_rgb(const float* src, int sw, int sh,
                          float*       dst, int dw, int dh) {
    bilinear_resize_impl<3>(src, sw, sh, dst, dw, dh);
}

void bilinear_resize_mask(const float* src, int sw, int sh,
                           float*       dst, int dw, int dh) {
    bilinear_resize_impl<1>(src, sw, sh, dst, dw, dh);
}

// ----------------------------------------------------------------------
// Canvas-pad helpers - paste small image into top-left of larger
// ----------------------------------------------------------------------

void copy_into_canvas_rgb(const float* src, int sw, int sh,
                           float*       dst, int dw, int dh)
{
    const int row_bytes = std::min(sw, dw) * 3 * static_cast<int>(sizeof(float));
    const int rows      = std::min(sh, dh);
    for (int y = 0; y < rows; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dw * 3,
                    src + static_cast<size_t>(y) * sw * 3,
                    static_cast<size_t>(row_bytes));
    }
}

void copy_into_canvas_mask(const float* src, int sw, int sh,
                            float*       dst, int dw, int dh)
{
    const int row_bytes = std::min(sw, dw) * static_cast<int>(sizeof(float));
    const int rows      = std::min(sh, dh);
    for (int y = 0; y < rows; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dw,
                    src + static_cast<size_t>(y) * sw,
                    static_cast<size_t>(row_bytes));
    }
}

// ----------------------------------------------------------------------
// Composite - dst[i] = mask[i] >= threshold ? inpainted[i] : base[i]
// ----------------------------------------------------------------------

void composite_masked(const float* base_rgb,
                      const float* inpainted_rgb,
                      const float* mask,
                      int          width,
                      int          height,
                      float        threshold,
                      float*       dst_rgb)
{
    const int N = width * height;
    for (int i = 0; i < N; ++i) {
        if (mask[i] >= threshold) {
            dst_rgb[i * 3 + 0] = inpainted_rgb[i * 3 + 0];
            dst_rgb[i * 3 + 1] = inpainted_rgb[i * 3 + 1];
            dst_rgb[i * 3 + 2] = inpainted_rgb[i * 3 + 2];
        } else {
            dst_rgb[i * 3 + 0] = base_rgb[i * 3 + 0];
            dst_rgb[i * 3 + 1] = base_rgb[i * 3 + 1];
            dst_rgb[i * 3 + 2] = base_rgb[i * 3 + 2];
        }
    }
}

// ----------------------------------------------------------------------
// inpaint_image - the main entry point
// ----------------------------------------------------------------------

void inpaint_image(LamaSession& session,
                   const float* rgb_in,
                   const float* mask,
                   int          width,
                   int          height,
                   float*       rgb_out,
                   ProgressFn   progress,
                   void*        progress_user)
{
    if (!rgb_in || !mask || !rgb_out) {
        throw std::runtime_error("inpaint_image: null pointer");
    }
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("inpaint_image: non-positive dimensions");
    }

    report_progress(progress, progress_user, 0.0f);

    const int  max_dim     = std::max(width, height);
    const bool need_resize = (max_dim > kN);

    // Scratch buffers sized to LaMa's fixed 512x512 input.
    std::vector<float> img_512(static_cast<size_t>(kN) * kN * 3, 0.0f);
    std::vector<float> msk_512(static_cast<size_t>(kN) * kN,     0.0f);
    std::vector<float> out_512(static_cast<size_t>(kN) * kN * 3, 0.0f);

    int scaled_w = width;
    int scaled_h = height;

    if (need_resize) {
        // Downsample preserving aspect ratio so max(scaled_w, scaled_h) == kN.
        if (width >= height) {
            scaled_w = kN;
            scaled_h = std::max(1,
                static_cast<int>(std::round(
                    static_cast<float>(height) * kN /
                    static_cast<float>(width))));
        } else {
            scaled_h = kN;
            scaled_w = std::max(1,
                static_cast<int>(std::round(
                    static_cast<float>(width) * kN /
                    static_cast<float>(height))));
        }

        // Resize input image and mask into the (scaled_w, scaled_h)
        // top-left region of the 512x512 canvas. Outside that region,
        // the canvas stays zero - which for the mask means "don't
        // inpaint", and for the image means black; LaMa handles the
        // black border gracefully because the mask there is zero.
        std::vector<float> tmp_img(static_cast<size_t>(scaled_w) * scaled_h * 3);
        std::vector<float> tmp_msk(static_cast<size_t>(scaled_w) * scaled_h);
        bilinear_resize_rgb(rgb_in, width, height,
                            tmp_img.data(), scaled_w, scaled_h);
        bilinear_resize_mask(mask, width, height,
                             tmp_msk.data(), scaled_w, scaled_h);

        copy_into_canvas_rgb(tmp_img.data(), scaled_w, scaled_h,
                             img_512.data(), kN, kN);
        copy_into_canvas_mask(tmp_msk.data(), scaled_w, scaled_h,
                              msk_512.data(), kN, kN);
    } else {
        // Image fits in 512x512; pad with zeros.
        copy_into_canvas_rgb(rgb_in, width, height,
                             img_512.data(), kN, kN);
        copy_into_canvas_mask(mask, width, height,
                              msk_512.data(), kN, kN);
    }

    report_progress(progress, progress_user, 0.2f);

    // Run inference. This is the long step.
    session.run(img_512.data(), msk_512.data(), out_512.data());

    report_progress(progress, progress_user, 0.8f);

    if (need_resize) {
        // Crop the active region out of the 512x512 result, then
        // upsample back to original (width, height).
        std::vector<float> tmp_out(static_cast<size_t>(scaled_w) * scaled_h * 3);
        for (int y = 0; y < scaled_h; ++y) {
            std::memcpy(
                tmp_out.data() + static_cast<size_t>(y) * scaled_w * 3,
                out_512.data() + static_cast<size_t>(y) * kN * 3,
                static_cast<size_t>(scaled_w) * 3 * sizeof(float));
        }

        std::vector<float> upsampled(static_cast<size_t>(width) * height * 3);
        bilinear_resize_rgb(tmp_out.data(), scaled_w, scaled_h,
                            upsampled.data(), width, height);

        // Composite: preserve full-res source everywhere except the
        // masked region, which comes from the upsampled inference.
        composite_masked(rgb_in, upsampled.data(), mask,
                         width, height, 0.5f, rgb_out);
    } else {
        // For images that fit in 512x512, the inferred result IS at
        // the original resolution. Crop the (width, height) region
        // out of the 512x512 output, then composite.
        std::vector<float> cropped(static_cast<size_t>(width) * height * 3);
        for (int y = 0; y < height; ++y) {
            std::memcpy(
                cropped.data() + static_cast<size_t>(y) * width * 3,
                out_512.data() + static_cast<size_t>(y) * kN * 3,
                static_cast<size_t>(width) * 3 * sizeof(float));
        }
        composite_masked(rgb_in, cropped.data(), mask,
                         width, height, 0.5f, rgb_out);
    }

    report_progress(progress, progress_user, 1.0f);
}

} // namespace nukeaifill
