// nuke-ai-fill / core / include / sd_session.h
//
// PIMPL wrapper around stable-diffusion.cpp's C API. Consumers
// (AIGenerate Op, future inpaint/img2img Ops) include this header
// without dragging in stable-diffusion.h or any ggml headers.
//
// Lifecycle:
//   1. Construct SdSession (cheap).
//   2. Call ensure_loaded() with paths to model files. The first call
//      loads the model into VRAM/RAM (can take 10-30 seconds and 8-12GB).
//      Subsequent calls with the same paths are no-ops.
//   3. Call generate() one or more times.
//   4. Destructor frees the loaded model and CUDA context.
//
// Threading:
//   - Construction/destruction must happen on the same thread (CUDA
//     context affinity).
//   - generate() can be called from a worker thread provided no other
//     SdSession instance's generate() is running concurrently (sd.cpp's
//     progress callback is global, see implementation notes).
//   - ensure_loaded() and generate() share state; do not interleave from
//     multiple threads.

#ifndef NF_SD_SESSION_H_
#define NF_SD_SESSION_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nukeaifill {

// Locations of the four model files FLUX schnell needs, plus an
// optional ControlNet model file.
struct SdModelPaths {
    std::string diffusion_model;  // flux1-schnell-Q*_*.gguf
    std::string vae;              // ae.safetensors (FLUX VAE)
    std::string clip_l;           // clip_l.safetensors
    std::string t5xxl;            // t5xxl_fp16.safetensors (or Q8 variant)
    std::string control_net;      // optional - leave empty for plain txt2img.
                                  // When set, ensure_loaded() loads this
                                  // alongside the diffusion model, and
                                  // generate() applies it provided
                                  // control_image_rgb is non-empty.
};

// One LoRA file applied at generation time. sd.cpp accepts a list of these
// in sd_img_gen_params_t.loras. The path must point to a file in
// .safetensors or .gguf format compatible with the loaded base model
// (e.g. FLUX-trained LoRAs only work with FLUX). Multiplier is the
// strength: 1.0 = full effect, 0.5 = half, negative values invert.
//
// We do not currently expose is_high_noise (only used for Wan video models)
// or any of the LoRA-apply-mode flags - sd.cpp handles those automatically.
struct LoRASpec {
    std::string path;
    float       weight = 1.0f;
};

// Per-generation parameters.
struct SdGenerateParams {
    std::string prompt;
    std::string negative_prompt;  // schnell ignores; kept for future dev support
    int     width      = 1024;
    int     height     = 1024;
    int     steps      = 4;       // schnell distilled for <=4 steps
    int64_t seed       = -1;      // -1 = random per call
    float   cfg_scale  = 1.0f;    // schnell does not use CFG; keep at 1.0

    // Optional LoRAs to apply on this generation. Order matters for
    // sd.cpp's apply-and-revert bookkeeping. An empty list means no LoRAs
    // (and reverts any LoRAs from prior generate() calls).
    std::vector<LoRASpec> loras;

    // Optional img2img source image. When set, sd.cpp encodes this through
    // the VAE to produce a starting latent and denoises from there instead
    // of from pure noise. Strength controls how much of the input survives
    // (0.0 = output equals input, 1.0 = pure txt2img, ignores input).
    //
    // Layout matches the ControlNet image below: HWC uint8 RGB, TOP-DOWN
    // scanline order. Dimensions don't have to match output width/height
    // (sd.cpp resamples internally) but should be close to avoid distortion.
    //
    // The init image and ControlNet image are independent - both, neither,
    // or either alone are all valid.
    std::vector<uint8_t> init_image_rgb;
    int   init_image_width  = 0;
    int   init_image_height = 0;
    float strength          = 0.75f;  // typical img2img sweet spot

    // Optional ControlNet input. Used only when ALL of the following:
    //   - SdModelPaths::control_net is a valid loaded ControlNet model
    //   - control_image_rgb is non-empty
    //   - control_image_width and _height match the buffer length
    //
    // Layout: HWC uint8 RGB, TOP-DOWN scanline order (row 0 = top of
    // image), values 0..255 in sRGB-ish space (sd.cpp does not assume
    // a specific transfer function but most ControlNets were trained
    // on sRGB inputs).
    //
    // The control image does not have to match width / height; sd.cpp
    // resamples internally. The control_image's aspect should be close
    // to the output aspect to avoid distortion.
    //
    // control_strength is how strictly the ControlNet enforces its
    // structural guidance. 1.0 = full strength (default), 0.0 = disabled
    // (effectively no ControlNet), >1.0 over-emphasizes.
    std::vector<uint8_t> control_image_rgb;
    int   control_image_width  = 0;
    int   control_image_height = 0;
    float control_strength     = 0.9f;
};

// Progress callback: (current_step, total_steps) -> continue?
// Returning false requests cancellation; sd.cpp may not honor it
// mid-denoising-step but will cancel at the next step boundary.
using SdProgressCallback = std::function<bool(int step, int total_steps)>;

class SdSession {
public:
    SdSession();
    ~SdSession();

    SdSession(const SdSession&)            = delete;
    SdSession& operator=(const SdSession&) = delete;

    // Loads the model. Returns true on success, false with error_out
    // populated on failure. Calling again with the same paths is a no-op.
    // Different paths reload (frees old context first).
    bool ensure_loaded(const SdModelPaths& paths,
                       std::string& error_out);

    // Runs txt2img. On success, out_rgb_hwc contains width*height*3
    // float32 values in [0,1] in HWC layout.
    //
    // on_progress is called periodically from sd.cpp; the same call thread
    // as generate(). Returning false requests cancellation.
    //
    // ensure_loaded() must have succeeded first.
    bool generate(const SdGenerateParams& params,
                  std::vector<float>& out_rgb_hwc,
                  int& out_width,
                  int& out_height,
                  const SdProgressCallback& on_progress,
                  std::string& error_out);

    // Signal best-effort cancel from another thread. The current
    // generate() call will return false with error_out = "cancelled"
    // at the next sd.cpp progress callback boundary.
    void request_cancel();

    bool is_loaded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nukeaifill

#endif // NF_SD_SESSION_H_
