// nuke-ai-fill / core / lama_tiler.h
//
// High-level inpaint orchestration around a fixed-shape LamaSession.
// Handles arbitrary input sizes:
//
//   max(w, h) <= 512  : zero-pad to 512x512, infer once, crop back
//   max(w, h) >  512  : downsample to fit in 512 (preserve aspect),
//                       infer, upsample result, composite only over
//                       the masked pixels in the original resolution
//
// The composite step uses the ORIGINAL full-resolution mask, so the
// boundary between inpainted and original is crisp. Unmasked pixels
// retain full source detail.
//
// This is a simpler-but-correct alternative to tile-based inference
// with overlap-feather blending. Tile-based is on the roadmap as a
// "Quality: High" mode but the resize-composite approach handles the
// common cases (small object removal on HD/4K images) at the cost of
// resolution inside the inpainted area only.
//
// All buffers are HWC interleaved float32 in [0, 1]. Mask values
// >= 0.5 are treated as "inpaint this pixel"; values < 0.5 keep the
// original. Strict ASCII per NDK_NOTES 6.1.

#ifndef NUKE_AI_FILL_LAMA_TILER_H
#define NUKE_AI_FILL_LAMA_TILER_H

#include "lama_session.h"

namespace nukeaifill {

// Progress callback. Called periodically with progress in [0, 1].
// Return false from the callback to cancel - inpaint_image will
// throw std::runtime_error("cancelled") in response. Pass nullptr
// to opt out of progress reporting.
using ProgressFn = bool (*)(float fraction, void* user);

// Inpaint an image of any size using the given LamaSession.
//
//   session  : initialized (or to-be-initialized; lazy load is fine)
//   rgb_in   : [w*h*3] HWC float32 in [0,1]
//   mask     : [w*h]   float32, >=0.5 = inpaint
//   width    : image width  (any positive int)
//   height   : image height (any positive int)
//   rgb_out  : [w*h*3] HWC float32 in [0,1], caller-owned
//   progress : optional progress callback; may be nullptr
//   progress_user : opaque user pointer passed to progress callback
//
// Throws std::runtime_error on failure or cancellation.
void inpaint_image(LamaSession& session,
                   const float* rgb_in,
                   const float* mask,
                   int          width,
                   int          height,
                   float*       rgb_out,
                   ProgressFn   progress      = nullptr,
                   void*        progress_user = nullptr);

// ----------------------------------------------------------------------
// Internal helpers exposed for unit testing. Not part of the stable
// API; callers should use inpaint_image. These deal with pure pixel
// math (no LamaSession dependency).
// ----------------------------------------------------------------------

// Bilinear resize for 3-channel HWC float32 image.
void bilinear_resize_rgb(const float* src, int sw, int sh,
                         float*       dst, int dw, int dh);

// Bilinear resize for 1-channel float32 image (mask).
void bilinear_resize_mask(const float* src, int sw, int sh,
                          float*       dst, int dw, int dh);

// Zero-pad small image into a larger canvas, top-left aligned.
// Caller pre-zeros dst; this function only writes the (sw, sh) region.
void copy_into_canvas_rgb(const float* src, int sw, int sh,
                          float*       dst, int dw, int dh);
void copy_into_canvas_mask(const float* src, int sw, int sh,
                           float*       dst, int dw, int dh);

// Composite inpainted RGB over a base image only where mask >= threshold.
// dst is initialized with base; pixels with mask >= threshold are
// overwritten by inpainted. dst, base, inpainted, mask all w*h sized.
void composite_masked(const float* base_rgb,
                      const float* inpainted_rgb,
                      const float* mask,
                      int          width,
                      int          height,
                      float        threshold,
                      float*       dst_rgb);

} // namespace nukeaifill

#endif // NUKE_AI_FILL_LAMA_TILER_H
