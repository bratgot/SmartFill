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

// Locations of the four model files FLUX schnell needs.
struct SdModelPaths {
    std::string diffusion_model;  // flux1-schnell-Q*_*.gguf
    std::string vae;              // ae.safetensors (FLUX VAE)
    std::string clip_l;           // clip_l.safetensors
    std::string t5xxl;            // t5xxl_fp16.safetensors (or Q8 variant)
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
