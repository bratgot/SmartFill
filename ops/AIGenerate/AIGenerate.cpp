// nuke-ai-fill / ops / AIGenerate / AIGenerate.cpp
//
// Text-to-image generation Op using stable-diffusion.cpp with FLUX
// schnell. Mirrors AISmartFill's architecture: async background worker,
// content-addressed disk cache, Bake button, status/progress display.
//
// Chunk D3 - full integration. Replaces the D1 black-frame stub.
//
// User workflow:
//   1. Drop AIGenerate node in graph
//   2. Type prompt, set dimensions / steps / seed if desired
//   3. Press Bake
//   4. ~30s later the result appears (cached on disk)
//   5. Subsequent re-cooks load from cache; rebake explicitly with same
//      params hits cache; changing prompt or seed forces fresh bake
//
// Op has 0 inputs (txt2img is parameter-only). Output bbox derived from
// width/height knobs at _validate time.

#include <DDImage/Iop.h>
#include <DDImage/Knobs.h>
#include <DDImage/Knob.h>
#include <DDImage/Row.h>
#include <DDImage/Hash.h>

#ifdef POINTS
#  undef POINTS
#endif

#include "ai_worker.h"
#include "ai_hash.h"
#include "ai_cache.h"
#include "image_cache.h"
#include "plugin_path.h"

#ifdef NUKE_AI_FILL_HAS_SD
#  include "sd_session.h"
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

using namespace DD::Image;
namespace fs = std::filesystem;
namespace nf = nukeaifill;

static const char* AI_GENERATE_HELP =
    "AI Generate (FLUX schnell)\n\n"
    "Text-to-image generation using stable-diffusion.cpp.\n\n"
    "Set a prompt, choose dimensions (1024x1024 is the native FLUX size),\n"
    "then press Bake. Generation runs in the background; the result\n"
    "appears in the viewer once complete. Generations are cached on disk\n"
    "by (prompt + seed + steps + dimensions + model files) - repeating\n"
    "the same bake is instant.\n\n"
    "Model files (~16GB total) live in:\n"
    "  %USERPROFILE%\\.nuke\\nuke-ai-fill\\models\\\n"
    "  - flux1-schnell-Q4_K.gguf\n"
    "  - ae.safetensors\n"
    "  - clip_l.safetensors\n"
    "  - t5xxl_fp16.safetensors\n\n"
    "Seed: -1 (default) generates a random seed each bake. Set a positive\n"
    "value to reproduce a specific image.";

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

static inline std::string ck_str(const char* s) {
    return s ? std::string(s) : std::string{};
}

static inline std::string normalize_path(const std::string& s) {
    std::string out = s;
    for (auto& c : out) if (c == '\\') c = '/';
    return out;
}

#ifdef NUKE_AI_FILL_HAS_SD
// Per-process SdSession singleton. Holds the loaded model in VRAM
// across multiple AIGenerate nodes and across bakes.
static nf::SdSession& get_session() {
    static nf::SdSession session;
    return session;
}
#endif

// ----------------------------------------------------------------------
// AIGenerate Op
// ----------------------------------------------------------------------

class AIGenerate : public Iop {
public:
    static const Description description;

    explicit AIGenerate(Node* n)
        : Iop(n)
        , width_(1024)
        , height_(1024)
        , steps_(4)
        , cfg_scale_(1.0f)
        , seed_(-1)
        , progress_(0.0f)
        , prompt_("")
        , negative_prompt_("")
        , models_dir_("")
        , diffusion_model_path_("")
        , vae_path_("")
        , clip_l_path_("")
        , t5xxl_path_("")
        , cache_dir_("")
        , status_text_("(idle)")
        , last_seed_text_("")
        , last_resolved_seed_(0)
    {
        inputs(0);
    }

    const char* Class() const override     { return description.name; }
    const char* node_help() const override { return AI_GENERATE_HELP; }

    int minimum_inputs() const override { return 0; }
    int maximum_inputs() const override { return 0; }

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;
    void append(Hash& hash) override;

    void _validate(bool for_real) override;
    void _request(int /*x*/, int /*y*/, int /*r*/, int /*t*/,
                  ChannelMask /*channels*/, int /*count*/) override {}
    void engine(int y, int x, int r, ChannelMask channels, Row& out) override;

private:
    // ---- knob backing storage ----
    int    width_, height_, steps_;
    float  cfg_scale_;
    int    seed_;
    float  progress_;
    const char* prompt_;
    const char* negative_prompt_;
    const char* models_dir_;
    const char* diffusion_model_path_;
    const char* vae_path_;
    const char* clip_l_path_;
    const char* t5xxl_path_;
    const char* cache_dir_;
    const char* status_text_;
    const char* last_seed_text_;

    // Resolved seed used in the most recent bake. Persists via the
    // hidden last_seed_text_ knob so it survives node serialization.
    int64_t last_resolved_seed_;

    // ---- cache + worker state ----
    nf::AiWorker        worker_;
    std::string         current_key_;
    std::string         loaded_key_;
    std::string         baking_key_;
    std::vector<float>  cached_pixels_;  // RGB HWC float [0,1]
    int                 cached_w_ = 0;
    int                 cached_h_ = 0;

    // Format must persist as a member - info_.full_size_format() and
    // info_.format() store a reference. Passing a temporary results
    // in a dangling reference and Nuke reads garbage dimensions
    // (symptom: viewer reports "image clipped to 1048576 x ..." or
    // similar nonsense numbers).
    Format              output_format_;

    // ---- helpers ----
    void set_knob_text(const char* knob_name, const char* value);
    void set_status(const char* s);
    void set_progress(float v);

    int64_t resolve_seed();
    std::string compute_cache_key() const;
    std::string result_path(const std::string& key) const;
    void try_load_cache_for_key(const std::string& key);
    void start_bake();
    void poll_worker();
    void clear_cache_on_disk();
};

// ----------------------------------------------------------------------
// Knob registration
// ----------------------------------------------------------------------

void AIGenerate::knobs(Knob_Callback f)
{
    // ---- Generation parameters ----
    Multiline_String_knob(f, &prompt_, "prompt", "Prompt");
    SetFlags(f, Knob::NO_ANIMATION);

    Multiline_String_knob(f, &negative_prompt_, "negative_prompt",
                          "Negative Prompt");
    SetFlags(f, Knob::NO_ANIMATION);
    Tooltip(f, "Ignored by FLUX schnell; kept for future model support.");

    Divider(f, "");

    Int_knob(f, &width_,  "width",  "Width");
    SetFlags(f, Knob::STARTLINE);
    Tooltip(f, "Output width. FLUX is trained on 1024.");

    Int_knob(f, &height_, "height", "Height");
    Tooltip(f, "Output height. FLUX is trained on 1024.");

    Int_knob(f, &steps_, "steps", "Steps");
    SetFlags(f, Knob::STARTLINE);
    Tooltip(f, "Denoising steps. FLUX schnell is distilled for 4 or fewer.");

    Int_knob(f, &seed_, "seed", "Seed");
    Tooltip(f, "Random seed. -1 = random per bake. Positive values are "
               "reproducible.");

    Float_knob(f, &cfg_scale_, "cfg_scale", "CFG Scale");
    SetFlags(f, Knob::STARTLINE);
    SetRange(f, 1.0, 15.0);
    Tooltip(f, "Classifier-free guidance scale. FLUX schnell does not "
               "use CFG; leave at 1.0.");

    // ---- Bake controls ----
    Divider(f, "");

    Button(f, "bake", "Bake");
    Tooltip(f, "Run text-to-image generation in the background. The "
               "result caches to disk and renders to the viewer when done.");

    Button(f, "clear_cache", "Clear Cache");
    Tooltip(f, "Delete all cached generations from the on-disk cache.");

    // Status + progress (read-only display)
    String_knob(f, &status_text_, "status", "Status");
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION | Knob::STARTLINE);

    Float_knob(f, &progress_, "progress", "Progress");
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION);
    SetRange(f, 0.0, 1.0);

    String_knob(f, &last_seed_text_, "last_seed", "Last Seed");
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION | Knob::STARTLINE);
    Tooltip(f, "Seed used in the most recent bake. If seed=-1, this "
               "shows the random value picked. Useful for reproducing "
               "a specific result.");

    // ---- Models tab ----
    Tab_knob(f, "Models");

    File_knob(f, &models_dir_, "models_dir", "Models Directory");
    Tooltip(f, "Directory containing the four FLUX model files. If empty, "
               "defaults to the plugin's models/ subdirectory.");

    Divider(f, "Individual model paths (override)");

    File_knob(f, &diffusion_model_path_, "diffusion_model", "Diffusion Model");
    Tooltip(f, "FLUX schnell GGUF (e.g. flux1-schnell-Q4_K.gguf). "
               "If empty, derived from Models Directory.");

    File_knob(f, &vae_path_, "vae", "VAE");
    Tooltip(f, "FLUX VAE (ae.safetensors). "
               "If empty, derived from Models Directory.");

    File_knob(f, &clip_l_path_, "clip_l", "CLIP-L");
    Tooltip(f, "CLIP-L text encoder (clip_l.safetensors). "
               "If empty, derived from Models Directory.");

    File_knob(f, &t5xxl_path_, "t5xxl", "T5-XXL");
    Tooltip(f, "T5-XXL text encoder (t5xxl_fp16.safetensors or Q8 variant). "
               "If empty, derived from Models Directory.");

    Divider(f, "");

    File_knob(f, &cache_dir_, "cache_dir", "Cache Directory");
    Tooltip(f, "Where cached generations are stored. If empty, defaults "
               "to %APPDATA%\\nuke-ai-fill\\cache");
}

// ----------------------------------------------------------------------
// Button handling
// ----------------------------------------------------------------------

int AIGenerate::knob_changed(Knob* k)
{
    if (!k) return Iop::knob_changed(k);

    const char* name = k->name().c_str();

    if (std::strcmp(name, "bake") == 0) {
        std::fprintf(stderr, "[AIGenerate] Bake button pressed\n");
        std::fflush(stderr);
        start_bake();
        return 1;
    }
    if (std::strcmp(name, "clear_cache") == 0) {
        clear_cache_on_disk();
        return 1;
    }

    return Iop::knob_changed(k);
}

// ----------------------------------------------------------------------
// Hash override: include loaded_key_ so Nuke notices cache transitions
// ----------------------------------------------------------------------

void AIGenerate::append(Hash& hash)
{
    Iop::append(hash);
    hash.append(loaded_key_);
}

// ----------------------------------------------------------------------
// _validate: resolve paths, compute cache key, try load, drive worker
// ----------------------------------------------------------------------

void AIGenerate::_validate(bool /*for_real*/)
{
    try {
        // Output format derived from knobs. Persist as member so info_'s
        // stored reference stays valid.
        const int W = width_  > 0 ? width_  : 1024;
        const int H = height_ > 0 ? height_ : 1024;

        output_format_ = Format(W, H, 1.0);
        info_.full_size_format(output_format_);
        info_.format(output_format_);
        info_.set(0, 0, W, H);
        info_.channels(Mask_RGB);

        // NOTE: do NOT call resolve_default_paths() here. Nuke explicitly
        // forbids setting knob values from _validate ("Setting knob values
        // from validate is not supported and may cause unexpected
        // behaviour"). Default paths are resolved lazily inside start_bake()
        // via compute_effective_paths() without touching knob storage.

        poll_worker();

        const std::string prev_loaded = loaded_key_;
        current_key_ = compute_cache_key();
        try_load_cache_for_key(current_key_);

        // Cache-transition refresh pattern (per NDK_NOTES 14.2)
        if (loaded_key_ == current_key_ &&
            !loaded_key_.empty() &&
            prev_loaded != loaded_key_) {
            invalidate();
            asapUpdate();
        }
    }
    catch (const std::exception& e) {
        error("AIGenerate::_validate: %s", e.what());
    }
    catch (...) {
        error("AIGenerate::_validate: unknown exception");
    }
}

// ----------------------------------------------------------------------
// engine: serve from cache buffer if available, else emit black
// ----------------------------------------------------------------------

void AIGenerate::engine(int y, int x, int r,
                        ChannelMask channels, Row& out)
{
    const bool have_cache =
        !cached_pixels_.empty() &&
        cached_w_ == width_ &&
        cached_h_ == height_ &&
        cached_w_ > 0 && cached_h_ > 0;

    if (!have_cache) {
        // No baked result yet: emit black.
        foreach (c, channels) {
            float* p = out.writable(c);
            for (int i = x; i < r; ++i) p[i] = 0.0f;
        }
        return;
    }

    // Nuke's coordinate system has y=0 at bottom; our cache is stored
    // top-down (row 0 = top), so flip on read.
    const int H = cached_h_;
    const int W = cached_w_;
    const int flipped_y = (H - 1) - y;
    if (flipped_y < 0 || flipped_y >= H) {
        foreach (c, channels) {
            float* p = out.writable(c);
            for (int i = x; i < r; ++i) p[i] = 0.0f;
        }
        return;
    }

    const float* row_ptr =
        cached_pixels_.data() + static_cast<size_t>(flipped_y) * W * 3;

    Channel R_chan = Chan_Red;
    Channel G_chan = Chan_Green;
    Channel B_chan = Chan_Blue;

    foreach (c, channels) {
        float* p = out.writable(c);
        int offset = -1;
        if (c == R_chan) offset = 0;
        else if (c == G_chan) offset = 1;
        else if (c == B_chan) offset = 2;

        if (offset < 0) {
            for (int i = x; i < r; ++i) p[i] = 0.0f;
            continue;
        }

        for (int i = x; i < r; ++i) {
            if (i < 0 || i >= W) {
                p[i] = 0.0f;
            } else {
                p[i] = row_ptr[i * 3 + offset];
            }
        }
    }
}

// ----------------------------------------------------------------------
// Knob mutation helpers (refresh GUI display, per NDK_NOTES 13.1-13.2)
// ----------------------------------------------------------------------

void AIGenerate::set_knob_text(const char* knob_name, const char* value)
{
    if (Knob* k = knob(knob_name)) {
        k->set_text(value ? value : "");
    }
}

void AIGenerate::set_status(const char* s)
{
    set_knob_text("status", s);
}

void AIGenerate::set_progress(float v)
{
    progress_ = v;
    if (Knob* k = knob("progress")) {
        k->set_value(static_cast<double>(v));
    }
}

// ----------------------------------------------------------------------
// Resolve default paths from models_dir if individual paths are empty
// ----------------------------------------------------------------------

// Compute the effective paths used by the worker. User-supplied knob
// values take precedence; empty knobs fall back to plugin defaults.
// Returns by struct (does NOT modify any knob - that would be illegal
// from _validate per Nuke's docs).
struct EffectivePaths {
    std::string models_dir;
    std::string diffusion_model;
    std::string vae;
    std::string clip_l;
    std::string t5xxl;
    std::string cache_dir;
};

static EffectivePaths compute_effective_paths(
    const char* models_dir_knob,
    const char* diffusion_knob,
    const char* vae_knob,
    const char* clip_l_knob,
    const char* t5xxl_knob,
    const char* cache_dir_knob)
{
    auto empty_ck = [](const char* s) { return !s || !*s; };
    auto pick = [&empty_ck](const char* knob_value,
                            const std::string& fallback) -> std::string {
        return empty_ck(knob_value) ? fallback : std::string(knob_value);
    };

    EffectivePaths ep;

    // Models directory: knob or plugin_dir/models.
    if (!empty_ck(models_dir_knob)) {
        ep.models_dir = models_dir_knob;
    } else {
        const std::string plugin_dir = nf::current_plugin_dir();
        if (!plugin_dir.empty()) {
            ep.models_dir = plugin_dir + "/models";
        }
    }

    // Cache directory: knob or platform default.
    if (!empty_ck(cache_dir_knob)) {
        ep.cache_dir = cache_dir_knob;
    } else {
        ep.cache_dir = nf::default_cache_dir();
    }

    // Individual file paths: knob or <models_dir>/<expected filename>.
    ep.diffusion_model = pick(diffusion_knob,
                              ep.models_dir + "/flux1-schnell-Q4_K.gguf");
    ep.vae             = pick(vae_knob,
                              ep.models_dir + "/ae.safetensors");
    ep.clip_l          = pick(clip_l_knob,
                              ep.models_dir + "/clip_l.safetensors");
    ep.t5xxl           = pick(t5xxl_knob,
                              ep.models_dir + "/t5xxl_fp16.safetensors");

    return ep;
}

// ----------------------------------------------------------------------
// Resolve seed: if user-specified seed_ >= 0 use it; else pick random
// ----------------------------------------------------------------------

int64_t AIGenerate::resolve_seed()
{
    if (seed_ >= 0) {
        last_resolved_seed_ = static_cast<int64_t>(seed_);
    } else {
        std::random_device rd;
        std::mt19937_64 rng(rd());
        std::uniform_int_distribution<int64_t> dist(0, (1LL << 31) - 1);
        last_resolved_seed_ = dist(rng);
    }
    // Display the resolved seed in the panel.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(last_resolved_seed_));
    set_knob_text("last_seed", buf);
    return last_resolved_seed_;
}

// ----------------------------------------------------------------------
// Cache key: SHA-256 of all generation inputs
// ----------------------------------------------------------------------

std::string AIGenerate::compute_cache_key() const
{
    const EffectivePaths ep = compute_effective_paths(
        models_dir_, diffusion_model_path_, vae_path_,
        clip_l_path_, t5xxl_path_, cache_dir_);

    nf::CacheKeyBuilder kb;
    kb.add(std::string("AIGenerate.v1"));
    kb.add(ck_str(prompt_));
    kb.add(ck_str(negative_prompt_));
    kb.add(static_cast<int32_t>(width_));
    kb.add(static_cast<int32_t>(height_));
    kb.add(static_cast<int32_t>(steps_));
    kb.add(cfg_scale_);
    kb.add(static_cast<int64_t>(last_resolved_seed_));
    kb.add(normalize_path(ep.diffusion_model));
    kb.add(normalize_path(ep.vae));
    kb.add(normalize_path(ep.clip_l));
    kb.add(normalize_path(ep.t5xxl));
    return kb.finalize();
}

std::string AIGenerate::result_path(const std::string& key) const
{
    const EffectivePaths ep = compute_effective_paths(
        models_dir_, diffusion_model_path_, vae_path_,
        clip_l_path_, t5xxl_path_, cache_dir_);
    if (ep.cache_dir.empty()) return std::string{};
    return ep.cache_dir + "/" + key + ".aigen";
}

void AIGenerate::try_load_cache_for_key(const std::string& key)
{
    if (key.empty() || key == loaded_key_) return;
    const std::string path = result_path(key);
    if (path.empty()) return;

    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) {
        return;
    }

    nf::ImageCacheReadResult res = nf::image_cache_read(path);
    if (!res.ok || res.channels != 3 || res.width <= 0 || res.height <= 0) {
        return;
    }

    cached_pixels_ = std::move(res.pixels);
    cached_w_ = res.width;
    cached_h_ = res.height;
    loaded_key_ = key;
}

// ----------------------------------------------------------------------
// Cache management
// ----------------------------------------------------------------------

void AIGenerate::clear_cache_on_disk()
{
    const EffectivePaths ep = compute_effective_paths(
        models_dir_, diffusion_model_path_, vae_path_,
        clip_l_path_, t5xxl_path_, cache_dir_);
    const std::string dir = ep.cache_dir;
    if (dir.empty()) return;

    std::error_code ec;
    fs::path p = fs::u8path(dir);
    if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) {
        set_status("Cache dir does not exist");
        return;
    }

    int deleted = 0;
    for (auto& entry : fs::directory_iterator(p, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() == ".aigen") {
            std::error_code rm_ec;
            fs::remove(entry.path(), rm_ec);
            if (!rm_ec) ++deleted;
        }
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "Cleared %d cached file(s)", deleted);
    set_status(buf);

    cached_pixels_.clear();
    cached_w_ = cached_h_ = 0;
    loaded_key_.clear();
    invalidate();
    asapUpdate();
}

// ----------------------------------------------------------------------
// Worker state polling - same pattern as AISmartFill
// ----------------------------------------------------------------------

void AIGenerate::poll_worker()
{
    const auto state = worker_.state();

    if (state == nf::AiWorkerState::Idle) return;

    if (state == nf::AiWorkerState::Running) {
        const float p = worker_.progress();
        set_progress(p);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Cooking %d%%",
                      static_cast<int>(p * 100.0f));
        set_status(buf);
        return;
    }

    if (state == nf::AiWorkerState::Done) {
        set_status("Ready");
        set_progress(1.0f);
        worker_.reset();
        // Force cache reload for baking_key_
        if (!baking_key_.empty()) {
            // Force re-attempt of cache load.
            if (loaded_key_ != baking_key_) {
                cached_pixels_.clear();
                cached_w_ = cached_h_ = 0;
                loaded_key_.clear();
            }
        }
        baking_key_.clear();
        asapUpdate();
        return;
    }

    if (state == nf::AiWorkerState::Error) {
        std::string msg = worker_.error_message();
        error("AIGenerate: %s", msg.c_str());
        std::string display = "Error: " + msg;
        if (display.size() > 80) display.resize(80);
        set_status(display.c_str());
        set_progress(0.0f);
        worker_.reset();
        baking_key_.clear();
        return;
    }

    if (state == nf::AiWorkerState::Cancelled) {
        set_status("Cancelled");
        set_progress(0.0f);
        worker_.reset();
        baking_key_.clear();
        return;
    }
}

// ----------------------------------------------------------------------
// start_bake: validate params, resolve seed, launch worker
// ----------------------------------------------------------------------

void AIGenerate::start_bake()
{
#ifndef NUKE_AI_FILL_HAS_SD
    error("AIGenerate: stable-diffusion.cpp backend not compiled in. "
          "Re-build with -DBUILD_GENERATE=ON.");
    set_status("Error: backend missing");
    return;
#else
    if (worker_.state() == nf::AiWorkerState::Running) {
        set_status("Already cooking");
        return;
    }

    // Compute effective paths once for this bake. We do NOT write them
    // back to knobs - that would violate "no knob writes from
    // _validate" (this is called from knob_changed, but the path
    // resolution result is purely a worker-input, not a UI display).
    const EffectivePaths ep = compute_effective_paths(
        models_dir_, diffusion_model_path_, vae_path_,
        clip_l_path_, t5xxl_path_, cache_dir_);

    if (!prompt_ || !*prompt_) {
        error("AIGenerate: empty prompt");
        set_status("Error: empty prompt");
        return;
    }

    if (width_ < 64 || height_ < 64 || width_ > 4096 || height_ > 4096) {
        error("AIGenerate: bad dimensions %dx%d", width_, height_);
        set_status("Error: bad dimensions");
        return;
    }
    if (steps_ < 1 || steps_ > 100) {
        error("AIGenerate: bad steps %d", steps_);
        set_status("Error: bad steps");
        return;
    }

    nf::SdModelPaths paths;
    paths.diffusion_model = ep.diffusion_model;
    paths.vae             = ep.vae;
    paths.clip_l          = ep.clip_l;
    paths.t5xxl           = ep.t5xxl;

    if (paths.diffusion_model.empty() || paths.vae.empty() ||
        paths.clip_l.empty() || paths.t5xxl.empty()) {
        error("AIGenerate: one or more model paths are empty");
        set_status("Error: model paths missing");
        return;
    }

    // Verify model files exist before kicking off the worker.
    std::error_code ec;
    auto exists = [&ec](const std::string& p) {
        return fs::exists(fs::u8path(p), ec);
    };
    if (!exists(paths.diffusion_model)) {
        error("AIGenerate: missing diffusion model: %s",
              paths.diffusion_model.c_str());
        set_status("Error: missing diffusion model file");
        return;
    }
    if (!exists(paths.vae)) {
        error("AIGenerate: missing VAE: %s", paths.vae.c_str());
        set_status("Error: missing VAE file");
        return;
    }
    if (!exists(paths.clip_l)) {
        error("AIGenerate: missing CLIP-L: %s", paths.clip_l.c_str());
        set_status("Error: missing CLIP-L file");
        return;
    }
    if (!exists(paths.t5xxl)) {
        error("AIGenerate: missing T5-XXL: %s", paths.t5xxl.c_str());
        set_status("Error: missing T5-XXL file");
        return;
    }

    // Resolve seed (random or user-provided), display it, use for key.
    const int64_t seed = resolve_seed();

    const std::string key  = compute_cache_key();
    const std::string path = result_path(key);

    // Ensure cache directory exists.
    if (!path.empty()) {
        fs::path parent = fs::u8path(path).parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
        }
    }

    // Cache hit: skip the bake.
    if (!path.empty() && fs::exists(fs::u8path(path), ec)) {
        set_status("Already cached for this input");
        set_progress(1.0f);
        current_key_ = key;
        try_load_cache_for_key(key);
        invalidate();
        asapUpdate();
        return;
    }

    // Snapshot params for the worker thread.
    const int W      = width_;
    const int H      = height_;
    const int steps  = steps_;
    const float cfg  = cfg_scale_;
    const std::string prompt     = ck_str(prompt_);
    const std::string neg_prompt = ck_str(negative_prompt_);
    const std::string cache_path = path;

    baking_key_ = key;
    set_progress(0.0f);
    set_status("Cooking 0%");

    worker_.start([prompt, neg_prompt, W, H, steps, seed, cfg, paths,
                   cache_path]
                  (nf::AiWorkerContext& ctx) {
        std::fprintf(stderr, "[AIGenerate] worker started\n"); std::fflush(stderr);
        ctx.set_status_text("Loading model");
        ctx.set_progress(0.01f);

        std::fprintf(stderr, "[AIGenerate] paths:\n  diffusion=%s\n  vae=%s\n  clip_l=%s\n  t5xxl=%s\n",
                     paths.diffusion_model.c_str(),
                     paths.vae.c_str(),
                     paths.clip_l.c_str(),
                     paths.t5xxl.c_str());
        std::fflush(stderr);

        std::string err;
        std::fprintf(stderr, "[AIGenerate] calling get_session()\n"); std::fflush(stderr);
        auto& sess = get_session();
        std::fprintf(stderr, "[AIGenerate] calling ensure_loaded()\n"); std::fflush(stderr);
        if (!sess.ensure_loaded(paths, err)) {
            std::fprintf(stderr, "[AIGenerate] ensure_loaded FAILED: %s\n", err.c_str());
            std::fflush(stderr);
            throw std::runtime_error("Model load failed: " + err);
        }
        std::fprintf(stderr, "[AIGenerate] ensure_loaded OK\n"); std::fflush(stderr);

        ctx.set_status_text("Generating");
        ctx.set_progress(0.05f);

        nf::SdGenerateParams params;
        params.prompt           = prompt;
        params.negative_prompt  = neg_prompt;
        params.width            = W;
        params.height           = H;
        params.steps            = steps;
        params.seed             = seed;
        params.cfg_scale        = cfg;

        // Map sd.cpp's per-step progress (0..steps) into our 5%-90%
        // visible progress band, leaving headroom for cache write.
        auto progress_cb = [&ctx](int step, int total) -> bool {
            std::fprintf(stderr, "[AIGenerate] denoise step %d/%d\n", step, total);
            std::fflush(stderr);
            if (total > 0) {
                float frac = static_cast<float>(step) /
                             static_cast<float>(total);
                ctx.set_progress(0.05f + frac * 0.85f);
            }
            return !ctx.should_cancel();
        };

        std::vector<float> out_rgb;
        int out_w = 0, out_h = 0;
        std::fprintf(stderr, "[AIGenerate] calling sess.generate()\n"); std::fflush(stderr);
        if (!sess.generate(params, out_rgb, out_w, out_h,
                           progress_cb, err)) {
            std::fprintf(stderr, "[AIGenerate] generate FAILED: %s\n", err.c_str());
            std::fflush(stderr);
            throw std::runtime_error("Generation failed: " + err);
        }
        std::fprintf(stderr, "[AIGenerate] generate OK: %dx%d\n", out_w, out_h); std::fflush(stderr);
        if (out_w != W || out_h != H) {
            throw std::runtime_error("Output dimensions mismatch");
        }

        ctx.set_status_text("Writing cache");
        ctx.set_progress(0.93f);

        if (!nf::image_cache_write(cache_path,
                                   out_rgb.data(), out_w, out_h, 3)) {
            throw std::runtime_error(
                std::string("failed to write cache file: ") + cache_path);
        }
        std::fprintf(stderr, "[AIGenerate] cache written, done\n"); std::fflush(stderr);
        ctx.set_progress(1.0f);
    });
#endif // NUKE_AI_FILL_HAS_SD
}

// ----------------------------------------------------------------------
// Op registration
// ----------------------------------------------------------------------

static Iop* build(Node* node) { return new AIGenerate(node); }

const Iop::Description AIGenerate::description(
    "AIGenerate",
    "Image/AIGenerate",
    build
);
