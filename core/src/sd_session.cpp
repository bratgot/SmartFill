// nuke-ai-fill / core / src / sd_session.cpp
//
// PIMPL implementation of SdSession. Encapsulates everything to do
// with stable-diffusion.cpp's C API.
//
// API CONVENTIONS USED (from include/stable-diffusion.h):
//
//   sd_ctx_t* new_sd_ctx(const sd_ctx_params_t*)
//     - Takes filled-in params struct (init via sd_ctx_params_init).
//     - Returns NULL on failure.
//
//   sd_image_t* generate_image(sd_ctx_t*, const sd_img_gen_params_t*)
//     - Returns array of sd_image_t (one per batch); we pass batch_count=1.
//     - Caller frees image->data and the image array.
//     - image->data is uint8 [0,255], HWC layout, image->channel channels.
//
//   void sd_set_progress_callback(sd_progress_cb_t cb, void* data)
//     - GLOBAL callback (not per-context). Must be installed before
//       generate_image and serialized across concurrent SdSessions.
//
//   void free_sd_ctx(sd_ctx_t*)
//     - Frees context and all loaded model weights.
//
// FLUX SCHNELL CONFIG:
//   We trust sd_ctx_params_init's defaults for prediction, wtype,
//   scheduler, RNG, backend. sd.cpp auto-detects "Flux FLOW mode" from
//   the model file. Earlier rounds of overriding these caused NaN
//   propagation -> VAE saturation -> all-white output. The lesson:
//   call _init, override only the model paths and user-controlled
//   knobs (prompt, dimensions, steps, seed, cfg_scale).
//
//   Reference: sd-cli.exe (sd.cpp's own CLI) leaves all of these at
//   their _init defaults and works correctly with the same model files.

#include "sd_session.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "stable-diffusion.h"

namespace nukeaifill {

namespace {

// sd.cpp's progress and log callbacks are global, not per-context.
// We serialize SdSession use via this state. Concurrent generate()
// calls from multiple SdSession instances would clobber each other's
// callbacks; that case is documented as unsupported in the header.
struct CallbackState {
    std::atomic<bool>   in_use{false};
    SdProgressCallback  callback;
    std::atomic<bool>   cancel_requested{false};
};

static CallbackState  g_cb_state;
static std::mutex     g_cb_install_mutex;

static void log_callback_thunk(enum sd_log_level_t level,
                               const char* text,
                               void* /*data*/)
{
    // For debugging the first-load freeze, we forward EVERYTHING to
    // stderr. Once everything works, this can be filtered back to
    // SD_LOG_ERROR only.
    if (!text) return;
    const char* prefix = "[sd.cpp]";
    switch (level) {
        case SD_LOG_DEBUG: prefix = "[sd.cpp DEBUG]"; break;
        case SD_LOG_INFO:  prefix = "[sd.cpp INFO] "; break;
        case SD_LOG_WARN:  prefix = "[sd.cpp WARN] "; break;
        case SD_LOG_ERROR: prefix = "[sd.cpp ERR]  "; break;
    }
    std::fprintf(stderr, "%s %s", prefix, text);
    std::fflush(stderr);
}

static void progress_callback_thunk(int step,
                                    int steps,
                                    float /*time*/,
                                    void* /*data*/)
{
    if (!g_cb_state.in_use.load()) {
        return;
    }
    if (g_cb_state.callback) {
        bool keep_going = g_cb_state.callback(step, steps);
        if (!keep_going) {
            g_cb_state.cancel_requested.store(true);
        }
    }
}

// Best-effort path -> const char*. sd_ctx_params_t holds raw pointers;
// we keep the std::string members alive in Impl as long as the ctx is.
inline const char* csafe(const std::string& s) {
    return s.empty() ? "" : s.c_str();
}

} // namespace

// ---------------------------------------------------------------------------

struct SdSession::Impl {
    sd_ctx_t*      ctx = nullptr;
    SdModelPaths   loaded_paths;  // owns the strings the ctx params point at
    bool           loaded = false;

    ~Impl() {
        if (ctx) {
            free_sd_ctx(ctx);
            ctx = nullptr;
        }
    }
};

// ---------------------------------------------------------------------------

SdSession::SdSession()
    : impl_(std::make_unique<Impl>()) {}

SdSession::~SdSession() = default;

bool SdSession::is_loaded() const {
    return impl_->loaded;
}

void SdSession::request_cancel() {
    g_cb_state.cancel_requested.store(true);
}

// ---------------------------------------------------------------------------

bool SdSession::ensure_loaded(const SdModelPaths& paths,
                              std::string& error_out)
{
    if (paths.diffusion_model.empty()) {
        error_out = "ensure_loaded: diffusion_model path is empty";
        return false;
    }
    if (paths.vae.empty()) {
        error_out = "ensure_loaded: vae path is empty";
        return false;
    }
    if (paths.clip_l.empty()) {
        error_out = "ensure_loaded: clip_l path is empty";
        return false;
    }
    if (paths.t5xxl.empty()) {
        error_out = "ensure_loaded: t5xxl path is empty";
        return false;
    }

    // Reuse existing context if paths match.
    if (impl_->loaded
        && impl_->loaded_paths.diffusion_model == paths.diffusion_model
        && impl_->loaded_paths.vae             == paths.vae
        && impl_->loaded_paths.clip_l          == paths.clip_l
        && impl_->loaded_paths.t5xxl           == paths.t5xxl) {
        return true;
    }

    // Free previous if any.
    if (impl_->ctx) {
        free_sd_ctx(impl_->ctx);
        impl_->ctx = nullptr;
        impl_->loaded = false;
    }

    // Store paths so the c_str() pointers remain valid for the
    // lifetime of the sd context.
    impl_->loaded_paths = paths;

    // Install log callback before ctx creation so any load errors
    // are visible. Re-installing on every call is safe.
    {
        std::lock_guard<std::mutex> lock(g_cb_install_mutex);
        sd_set_log_callback(log_callback_thunk, nullptr);
    }

    sd_ctx_params_t ctx_params;
    sd_ctx_params_init(&ctx_params);

    // Model files
    ctx_params.diffusion_model_path = impl_->loaded_paths.diffusion_model.c_str();
    ctx_params.vae_path             = impl_->loaded_paths.vae.c_str();
    ctx_params.clip_l_path          = impl_->loaded_paths.clip_l.c_str();
    ctx_params.t5xxl_path           = impl_->loaded_paths.t5xxl.c_str();

    // Match sd-cli's policy: trust sd_ctx_params_init's defaults for
    // everything except (1) explicit user-controlled options and (2)
    // values we genuinely need to differ from default.
    //
    // EXPLICITLY DO NOT set: wtype, prediction, rng_type,
    // sampler_rng_type, backend, params_backend. The defaults
    // (NONE/auto) let sd.cpp pick correct values from the model
    // metadata. Overriding caused NaN propagation in earlier rounds.

    ctx_params.n_threads               = -1;       // auto: phys core count
    ctx_params.vae_decode_only         = false;    // may add img2img later
    ctx_params.free_params_immediately = false;    // reuse session loaded
    ctx_params.enable_mmap             = false;    // sd-cli default; some
                                                   //   builds can't mmap
                                                   //   onto VRAM cleanly
    ctx_params.lora_apply_mode         = LORA_APPLY_AUTO;

    sd_ctx_t* ctx = new_sd_ctx(&ctx_params);
    if (!ctx) {
        error_out = "new_sd_ctx returned NULL "
                    "(model paths invalid, CUDA unavailable, or out of VRAM)";
        return false;
    }

    impl_->ctx = ctx;
    impl_->loaded = true;
    return true;
}

// ---------------------------------------------------------------------------

bool SdSession::generate(const SdGenerateParams& params,
                         std::vector<float>& out_rgb_hwc,
                         int& out_width,
                         int& out_height,
                         const SdProgressCallback& on_progress,
                         std::string& error_out)
{
    if (!impl_->loaded || !impl_->ctx) {
        error_out = "generate: ensure_loaded must succeed first";
        return false;
    }

    // Install progress callback for the duration of this call.
    // The callback is global; if another SdSession instance is calling
    // generate() concurrently, they'll race on the callback. The API
    // doc forbids that case.
    g_cb_state.cancel_requested.store(false);
    g_cb_state.callback = on_progress;
    g_cb_state.in_use.store(true);
    {
        std::lock_guard<std::mutex> lock(g_cb_install_mutex);
        sd_set_progress_callback(progress_callback_thunk, nullptr);
    }

    sd_img_gen_params_t gen_params;
    sd_img_gen_params_init(&gen_params);

    // User-controlled fields:
    gen_params.prompt          = params.prompt.c_str();
    gen_params.negative_prompt = params.negative_prompt.c_str();
    gen_params.width           = params.width;
    gen_params.height          = params.height;
    gen_params.batch_count     = 1;
    gen_params.seed            = params.seed;
    gen_params.sample_params.sample_method = EULER_SAMPLE_METHOD;  // FLUX schnell uses Euler
    gen_params.sample_params.sample_steps  = params.steps;
    gen_params.sample_params.guidance.txt_cfg = params.cfg_scale;
    gen_params.sample_params.guidance.img_cfg = params.cfg_scale;

    // Match sd-cli's defaults exactly. sd_img_gen_params_init sets these,
    // but we make the values explicit here for clarity and to document
    // that overriding them (as earlier versions of this code did) caused
    // NaN-saturated white output:
    //
    //   guidance.distilled_guidance = 3.5f  (NOT 0 - even though FLUX
    //                                        schnell has guidance_embed
    //                                        false, sd.cpp uses this
    //                                        value internally)
    //   eta            = inf  (sentinel: use sampler's default)
    //   flow_shift     = inf  (sentinel: use model's default schedule)
    //   scheduler      = NONE (auto-pick based on model)
    //
    // We do NOT override these; the _init call above gave them the right
    // values. If you find yourself wanting to "fix" these later, run
    // sd-cli with -v and compare its dump to ours first.

    // VAE tiling stays as we had it - this is a user-facing knob (cuts
    // VAE compute buffer from 6.6GB to ~1.5GB at the cost of mild seams).
    gen_params.vae_tiling_params.enabled         = true;
    gen_params.vae_tiling_params.temporal_tiling = false;
    gen_params.vae_tiling_params.tile_size_x     = 0;
    gen_params.vae_tiling_params.tile_size_y     = 0;
    gen_params.vae_tiling_params.target_overlap  = 0.5f;
    gen_params.vae_tiling_params.rel_size_x      = 0.5f;
    gen_params.vae_tiling_params.rel_size_y      = 0.5f;
    gen_params.vae_tiling_params.extra_tiling_args = nullptr;

    // LoRAs. Build the sd_lora_t array referencing the string memory
    // already owned by params.loras (which the caller keeps alive across
    // this call). When params.loras is empty, leave gen_params.loras at
    // the _init default (nullptr) and lora_count at 0 - this also signals
    // sd.cpp to revert any LoRAs applied in a previous generate() call
    // (which is what we want; LoRA persistence across generations would
    // be surprising).
    std::vector<sd_lora_t> sd_loras;
    sd_loras.reserve(params.loras.size());
    for (const auto& l : params.loras) {
        sd_lora_t sl{};
        sl.path          = l.path.c_str();
        sl.multiplier    = l.weight;
        sl.is_high_noise = false;  // only for Wan video models
        sd_loras.push_back(sl);
    }
    if (!sd_loras.empty()) {
        gen_params.loras      = sd_loras.data();
        gen_params.lora_count = static_cast<int>(sd_loras.size());
    }

    sd_image_t* result = generate_image(impl_->ctx, &gen_params);

    // Detach callback (later calls will replace it).
    g_cb_state.in_use.store(false);
    g_cb_state.callback = nullptr;

    if (!result) {
        error_out = "generate_image returned NULL";
        return false;
    }

    // Handle cancellation: free the buffer and exit with cancelled.
    if (g_cb_state.cancel_requested.load()) {
        if (result->data) std::free(result->data);
        std::free(result);
        error_out = "cancelled";
        return false;
    }

    if (!result->data || result->width == 0 || result->height == 0) {
        if (result->data) std::free(result->data);
        std::free(result);
        error_out = "generate_image returned empty image";
        return false;
    }

    const int W = static_cast<int>(result->width);
    const int H = static_cast<int>(result->height);
    const int C = static_cast<int>(result->channel ? result->channel : 3);

    out_width  = W;
    out_height = H;
    out_rgb_hwc.assign(static_cast<size_t>(W) * H * 3, 0.0f);

    // Convert uint8 [0,255] HWC -> float [0,1] HWC RGB.
    // If source has 4 channels (RGBA), drop alpha; if 1 channel (grayscale),
    // broadcast.
    const float kInv255 = 1.0f / 255.0f;
    const uint8_t* src = result->data;
    float* dst = out_rgb_hwc.data();

    if (C >= 3) {
        for (int y = 0; y < H; ++y) {
            const uint8_t* sr = src + static_cast<size_t>(y) * W * C;
            float*         dr = dst + static_cast<size_t>(y) * W * 3;
            for (int x = 0; x < W; ++x) {
                dr[x * 3 + 0] = sr[x * C + 0] * kInv255;
                dr[x * 3 + 1] = sr[x * C + 1] * kInv255;
                dr[x * 3 + 2] = sr[x * C + 2] * kInv255;
            }
        }
    } else {
        // Grayscale: replicate to all three channels.
        for (int y = 0; y < H; ++y) {
            const uint8_t* sr = src + static_cast<size_t>(y) * W;
            float*         dr = dst + static_cast<size_t>(y) * W * 3;
            for (int x = 0; x < W; ++x) {
                float v = sr[x] * kInv255;
                dr[x * 3 + 0] = v;
                dr[x * 3 + 1] = v;
                dr[x * 3 + 2] = v;
            }
        }
    }

    std::free(result->data);
    std::free(result);
    return true;
}

} // namespace nukeaifill
