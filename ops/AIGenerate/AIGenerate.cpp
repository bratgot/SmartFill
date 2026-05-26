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
#include <sstream>
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
        , strength_(0.75f)
        , cook_revision_(0)
        , model_type_(0)  // 0=FLUX schnell, 1=SDXL, 2=SD 1.5
        , progress_(0.0f)
        , prompt_("")
        , negative_prompt_("")
        , models_dir_("")
        , diffusion_model_path_("")
        , vae_path_("")
        , clip_l_path_("")
        , t5xxl_path_("")
        , cache_dir_("")
        , loras_text_("")
        , loras_dir_("")
        , control_net_path_("")
        , control_strength_(0.9f)
        , status_text_("(idle)")
        , last_seed_text_("")
        , last_resolved_seed_(0)
    {
        inputs(2);
    }

    const char* Class() const override     { return description.name; }
    const char* node_help() const override { return AI_GENERATE_HELP; }

    // Two optional inputs:
    //   0 = img2img source image (the image to restyle).
    //   1 = ControlNet structural guidance (depth, edges, etc).
    // Both are optional. Pure txt2img works with nothing connected.
    int minimum_inputs() const override { return 0; }
    int maximum_inputs() const override { return 2; }
    const char* input_label(int n, char*) const override {
        if (n == 0) return "Source";
        if (n == 1) return "ControlNet";
        return "";
    }

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
    float  strength_;

    // Bumped from menu.py when a bake just finished, so the cache file
    // on disk now contains pixels we want to use. Participates in
    // append(Hash&) so changes invalidate Nuke's internal output cache
    // and force a re-cook (which is what makes the freshly-baked image
    // appear in the viewer without manual knob nudging).
    int    cook_revision_;

    // Base model architecture. Drives which model files are required
    // and which can be left empty (sd.cpp uses embedded copies).
    //   0 = FLUX schnell (requires diffusion + vae + clip_l + t5xxl)
    //   1 = SDXL          (requires diffusion only; CLIPs + VAE
    //                       usually embedded in the .safetensors)
    //   2 = SD 1.5        (requires diffusion only; CLIP + VAE
    //                       usually embedded in the .ckpt/.safetensors)
    int    model_type_;
    float  progress_;
    const char* prompt_;
    const char* negative_prompt_;
    const char* models_dir_;
    const char* diffusion_model_path_;
    const char* vae_path_;
    const char* clip_l_path_;
    const char* t5xxl_path_;
    const char* cache_dir_;
    const char* loras_text_;
    const char* loras_dir_;
    const char* control_net_path_;
    float       control_strength_;
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

    Float_knob(f, &strength_, "strength", "Img2Img Strength");
    SetRange(f, 0.0, 1.0);
    Tooltip(f, "Img2img denoising strength. Only used when the Source "
               "input (input 0) is connected.\n\n"
               "  0.0 = output equals input (no change)\n"
               "  0.4 = mild restyling, structure mostly preserved\n"
               "  0.75 = strong restyling (default, typical sweet spot)\n"
               "  1.0 = ignore input, pure txt2img\n\n"
               "Lower values keep more of the original image; higher "
               "values let the prompt drive the output more. With FLUX "
               "schnell's 4 steps the effective range is narrow - try "
               "0.5-0.85 for most restyling tasks.\n\n"
               "When the Source input is not connected, this knob has "
               "no effect.");

    // ---- Bake controls ----
    Divider(f, "");

    Button(f, "bake", "Bake");
    Tooltip(f, "Run text-to-image generation in the background. The "
               "result caches to disk. After completion, click Refresh "
               "Viewer to display it.");

    Button(f, "clear_cache", "Clear Cache");
    Tooltip(f, "Delete all cached generations from the on-disk cache.");

    Button(f, "refresh_viewer", "Refresh Viewer");
    Tooltip(f, "Force the viewer to recook and display the most recently "
               "baked image. Use this after a bake completes (status: "
               "Ready) if the viewer is still showing the old image.\n\n"
               "Background: Nuke's viewer doesn't automatically refresh "
               "when an Op's internal state changes from a background "
               "thread completing. This button forces the recook.");

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

    // Hidden knobs supporting viewer auto-refresh.
    //
    // cook_revision is an integer that participates in append(Hash&).
    // When it changes, Nuke considers the Op's output stale and will
    // recook. menu.py's polling code increments it (via the
    // bump_cook_revision button below) when a bake transitions from
    // active to done, which makes the freshly-cached pixels appear in
    // the viewer without manual knob nudging.
    //
    // Both are NO_ANIMATION because they participate in caching at
    // a global level; per-frame variation would invalidate the cache
    // on every frame change.
    Int_knob(f, &cook_revision_, "cook_revision", "");
    SetFlags(f, Knob::INVISIBLE | Knob::NO_ANIMATION | Knob::DO_NOT_WRITE);

    Button(f, "bump_cook_revision", "");
    SetFlags(f, Knob::INVISIBLE | Knob::DO_NOT_WRITE);

    // ---- Models tab ----
    Tab_knob(f, "Models");

    static const char* const model_type_options[] = {
        "FLUX schnell", "SDXL", "SD 1.5", nullptr
    };
    Enumeration_knob(f, &model_type_, model_type_options,
                     "model_type", "Model Type");
    Tooltip(f, "Base model architecture. Changes which files are required.\n\n"
               "FLUX schnell (default):\n"
               "  - Requires four separate files: diffusion, VAE, CLIP-L, T5-XXL\n"
               "  - Steps: 4, CFG: 1.0\n"
               "  - Highest quality, slowest. Apache 2.0 (commercial-safe).\n\n"
               "SDXL:\n"
               "  - Requires ONE file: the SDXL .safetensors (everything embedded)\n"
               "  - Steps: 25, CFG: 7.0 (recommend changing knobs manually)\n"
               "  - Mature ControlNet ecosystem\n"
               "  - Clear VAE/CLIP-L/T5-XXL knobs (they're FLUX-only)\n"
               "  - CreativeML Open RAIL-M (commercial-safe).\n\n"
               "SD 1.5:\n"
               "  - Requires ONE file: the SD 1.5 .ckpt or .safetensors\n"
               "  - Steps: 20-30, CFG: 7.0\n"
               "  - Fastest, lowest VRAM\n"
               "  - Clear VAE/CLIP-L/T5-XXL knobs\n"
               "  - CreativeML Open RAIL-M (commercial-safe).\n\n"
               "Behind the scenes: if a file path is empty AND no default "
               "file exists at the path, the plugin won't pass that path to "
               "sd.cpp - which then uses whatever is embedded in the "
               "Diffusion Model file.");

    File_knob(f, &models_dir_, "models_dir", "Models Directory");
    Tooltip(f, "Directory containing model files. If empty, "
               "defaults to the plugin's models/ subdirectory.");

    Divider(f, "Individual model paths (override)");

    File_knob(f, &diffusion_model_path_, "diffusion_model", "Diffusion Model");
    Tooltip(f, "Base model file. REQUIRED.\n"
               "  FLUX: flux1-schnell-q4_0.gguf (or similar)\n"
               "  SDXL: sd_xl_base_1.0.safetensors\n"
               "  SD 1.5: any .ckpt or .safetensors\n"
               "If empty, derived from Models Directory.");

    File_knob(f, &vae_path_, "vae", "VAE");
    Tooltip(f, "VAE file. REQUIRED for FLUX (ae.safetensors).\n"
               "OPTIONAL for SDXL / SD 1.5 (usually embedded in the "
               "diffusion model file). If empty for SDXL/SD1.5 AND no "
               "default file is present, sd.cpp uses the embedded VAE.\n"
               "If empty for FLUX, derived from Models Directory.");

    File_knob(f, &clip_l_path_, "clip_l", "CLIP-L");
    Tooltip(f, "CLIP-L text encoder. REQUIRED for FLUX (clip_l.safetensors).\n"
               "NOT USED by SDXL or SD 1.5 (they use CLIP-L + CLIP-G "
               "embedded in the diffusion model). Clear this knob when "
               "using SDXL or SD 1.5 to avoid sd.cpp loading the wrong "
               "file.\n"
               "If empty for FLUX, derived from Models Directory.");

    File_knob(f, &t5xxl_path_, "t5xxl", "T5-XXL");
    Tooltip(f, "T5-XXL text encoder. REQUIRED for FLUX only "
               "(t5-v1_1-xxl-encoder-Q8_0.gguf).\n"
               "NOT USED by SDXL or SD 1.5 (they don't have a T5 encoder).\n"
               "Clear this knob when using SDXL or SD 1.5.\n"
               "If empty for FLUX, derived from Models Directory.");

    Divider(f, "");

    File_knob(f, &cache_dir_, "cache_dir", "Cache Directory");
    Tooltip(f, "Where cached generations are stored. If empty, defaults "
               "to %APPDATA%\\nuke-ai-fill\\cache");

    Divider(f, "LoRAs");

    File_knob(f, &loras_dir_, "loras_dir", "LoRAs Directory");
    Tooltip(f, "Directory containing LoRA files. Used to resolve relative "
               "filenames in the LoRAs list. If empty, defaults to "
               "<Models Directory>/loras/.");

    Multiline_String_knob(f, &loras_text_, "loras", "LoRAs");
    SetFlags(f, Knob::NO_ANIMATION);
    Tooltip(f, "LoRAs to apply on each bake. One per line.\n\n"
               "Format:  path_or_filename : weight\n\n"
               "- path: absolute, or relative to LoRAs Directory\n"
               "- weight: float, typically 0.0 to 1.5 (default 1.0)\n"
               "- lines starting with # are comments\n"
               "- blank lines are ignored\n\n"
               "Example:\n"
               "  mystyle.safetensors : 0.8\n"
               "  C:/loras/character.safetensors : 1.0\n"
               "  # weight defaults to 1.0\n"
               "  another_style.safetensors\n\n"
               "LoRAs trained for a different base model "
               "(e.g. SD1.5 LoRAs on a FLUX model) will produce noise or "
               "fail to load.");

    Divider(f, "ControlNet");

    File_knob(f, &control_net_path_, "control_net", "ControlNet Model");
    Tooltip(f, "Path to a ControlNet model file (.safetensors or .gguf).\n\n"
               "ControlNet is ACTIVE only when this path is set AND the "
               "node's input is connected. If either is missing, "
               "generation is plain txt2img.\n\n"
               "The connected input should be a structural guidance image - "
               "an edge map, depth pass, or whatever matches the ControlNet "
               "model's training data. Convert it from RGB image in Nuke "
               "(e.g. EdgeDetect or a CG depth pass) before piping it in.\n\n"
               "*** IMPORTANT UPSTREAM LIMITATION ***\n"
               "stable-diffusion.cpp's ControlNet loader currently supports "
               "only U-Net architectures (SD 1.5, SDXL). FLUX, SD3, and "
               "Wan use transformer architectures (DiT) and their "
               "ControlNets will FAIL to load (look for 'unknown tensor' "
               "and 'tensor not in model file' errors in the log).\n\n"
               "Until sd.cpp adds DiT ControlNet support upstream, this "
               "knob is useful only when generating with an SDXL or SD 1.5 "
               "base model, paired with a ControlNet trained for that "
               "base. With the default FLUX schnell pipeline, this feature "
               "is effectively unavailable.\n\n"
               "WARNING: Even if upstream adds FLUX support, BFL's "
               "official FLUX.1-Canny-dev and FLUX.1-Depth-dev ControlNets "
               "are non-commercial-licensed. For commercial studio use, "
               "look for Apache 2.0 FLUX ControlNets from InstantX, "
               "Shakker-Labs, or similar (verify each model's license).");

    Float_knob(f, &control_strength_, "control_strength", "Control Strength");
    SetRange(f, 0.0, 2.0);
    Tooltip(f, "How strictly the ControlNet enforces its structural "
               "guidance:\n\n"
               "  1.0   = full strength (default, recommended starting "
               "point)\n"
               "  0.5   = soft guide; prompt gets more freedom\n"
               "  0.0   = effectively disabled\n"
               "  >1.0  = over-emphasizes structure, may distort\n\n"
               "Only relevant when ControlNet is active (model path set "
               "and input connected).");
}

// ----------------------------------------------------------------------
// Button handling
// ----------------------------------------------------------------------

int AIGenerate::knob_changed(Knob* k)
{
    if (!k) return Iop::knob_changed(k);

    const char* name = k->name().c_str();

    if (std::strcmp(name, "bake") == 0) {
        start_bake();
        return 1;
    }
    if (std::strcmp(name, "clear_cache") == 0) {
        clear_cache_on_disk();
        return 1;
    }
    if (std::strcmp(name, "bump_cook_revision") == 0 ||
        std::strcmp(name, "refresh_viewer") == 0) {
        // bump_cook_revision: invisible button, triggered from menu.py's
        //   polling code after a bake completes. (See known limitation
        //   note in menu.py - the polling-driven version doesn't reliably
        //   refresh the viewer, but the button infrastructure is here.)
        // refresh_viewer: visible button, user clicks it after a bake.
        //   This is the confirmed-working manual-refresh path.
        //
        // Both mutate cook_revision through the knob system (NOT direct
        // member write) so Nuke registers it as a real value change,
        // updates the hash via append(Hash&), and forces a re-cook.
        Knob* cr = knob("cook_revision");
        if (cr) {
            const int next = cook_revision_ + 1;
            cr->set_value(static_cast<double>(next));
        }
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
    // cook_revision is bumped from menu.py when a bake completes;
    // including it here makes the hash change which forces Nuke to
    // recook the Op and pick up the newly-cached pixels.
    hash.append(cook_revision_);
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

// Read the connected input as a uint8 RGB top-down buffer suitable for
// sd.cpp's sd_image_t. Reads at the input's native format dimensions;
// sd.cpp resizes internally to match generation dimensions if needed.
//
// Must be called from start_bake (main thread), NOT from _validate or
// the worker thread. Iop input cooking is allowed from knob_changed
// context.
//
// Returns true on success, false if input not connected, cook aborted,
// or wrong channel set. error_out is set on failure.
static bool read_input_rgb_u8(DD::Image::Iop* in,
                              std::vector<uint8_t>& out_rgb,
                              int& out_w,
                              int& out_h,
                              std::string& error_out)
{
    using namespace DD::Image;

    out_rgb.clear();
    out_w = 0;
    out_h = 0;

    if (!in) {
        error_out = "no input connected";
        return false;
    }

    in->validate(true);
    if (in->aborted()) {
        error_out = "input validate aborted";
        return false;
    }

    const Format& fmt = in->format();
    const int W = fmt.width();
    const int H = fmt.height();
    if (W <= 0 || H <= 0) {
        error_out = "input format has zero dimensions";
        return false;
    }

    // Cap pathological sizes - control images bigger than 4Kx4K make no
    // sense and would waste memory.
    if (W > 8192 || H > 8192) {
        error_out = "input dimensions exceed 8192x8192";
        return false;
    }

    in->request(0, 0, W, H, Mask_RGB, 1);

    out_rgb.assign(static_cast<size_t>(W) * H * 3, 0);
    out_w = W;
    out_h = H;

    auto to_u8 = [](float v) -> uint8_t {
        if (v <= 0.0f) return 0;
        if (v >= 1.0f) return 255;
        return static_cast<uint8_t>(v * 255.0f + 0.5f);
    };

    for (int y = 0; y < H; ++y) {
        Row row(0, W);
        row.get(*in, y, 0, W, Mask_RGB);
        if (in->aborted()) {
            error_out = "input cook aborted at row " + std::to_string(y);
            return false;
        }

        // sd.cpp expects top-down scanline order. Nuke's y=0 is bottom,
        // so flip on write.
        const int dst_y = (H - 1) - y;
        uint8_t* dst = &out_rgb[static_cast<size_t>(dst_y) * W * 3];

        const float* r_chan = row[Chan_Red];
        const float* g_chan = row[Chan_Green];
        const float* b_chan = row[Chan_Blue];

        for (int x = 0; x < W; ++x) {
            dst[x * 3 + 0] = to_u8(r_chan[x]);
            dst[x * 3 + 1] = to_u8(g_chan[x]);
            dst[x * 3 + 2] = to_u8(b_chan[x]);
        }
    }

    return true;
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
    std::string loras_dir;
    std::string control_net;   // empty when knob is empty (no auto-default)
};

static EffectivePaths compute_effective_paths(
    const char* models_dir_knob,
    const char* diffusion_knob,
    const char* vae_knob,
    const char* clip_l_knob,
    const char* t5xxl_knob,
    const char* cache_dir_knob,
    const char* loras_dir_knob,
    const char* control_net_knob,
    int model_type = 0)
{
    auto empty_ck = [](const char* s) { return !s || !*s; };
    auto pick = [&empty_ck](const char* knob_value,
                            const std::string& fallback) -> std::string {
        return empty_ck(knob_value) ? fallback : std::string(knob_value);
    };

    // Optional path resolver: if knob set, use it as-is. If empty:
    //   - For FLUX, try the default filename (file must exist on disk).
    //   - For SDXL/SD1.5, leave empty (sd.cpp uses embedded encoder).
    // Either way, only auto-fill if the file actually exists; otherwise
    // pass empty to sd.cpp (which keeps the _init nullptr default).
    auto pick_optional = [&empty_ck](const char* knob_value,
                                     const std::string& default_path,
                                     bool try_default) -> std::string {
        if (!empty_ck(knob_value)) {
            return std::string(knob_value);
        }
        if (!try_default || default_path.empty()) {
            return std::string{};
        }
        std::error_code ec;
        if (fs::exists(fs::u8path(default_path), ec)) {
            return default_path;
        }
        return std::string{};
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

    // Diffusion model: REQUIRED for all model types. Default depends on
    // type; if user clears the knob, we fall back to the default which
    // start_bake() validates as existing.
    const bool is_flux = (model_type == 0);
    if (is_flux) {
        ep.diffusion_model = pick(diffusion_knob,
                                  ep.models_dir + "/flux1-schnell-q4_0.gguf");
    } else {
        // SDXL/SD1.5 - no canonical default filename, require explicit
        // knob value. If knob is empty, ep.diffusion_model stays empty
        // and start_bake reports the missing model.
        ep.diffusion_model = empty_ck(diffusion_knob) ? std::string{}
                                                      : std::string(diffusion_knob);
    }

    // VAE, CLIP-L, T5-XXL: REQUIRED for FLUX, OPTIONAL otherwise.
    // For FLUX, fall back to canonical defaults. For SDXL/SD1.5, only
    // use the knob value if set (and even then, only if the file
    // exists - else leave empty so sd.cpp uses embedded).
    ep.vae = pick_optional(vae_knob,
                           ep.models_dir + "/ae.safetensors",
                           is_flux);
    ep.clip_l = pick_optional(clip_l_knob,
                              ep.models_dir + "/clip_l.safetensors",
                              is_flux);
    ep.t5xxl = pick_optional(t5xxl_knob,
                             ep.models_dir + "/t5-v1_1-xxl-encoder-Q8_0.gguf",
                             is_flux);

    // LoRAs directory: knob or <models_dir>/loras.
    ep.loras_dir = pick(loras_dir_knob, ep.models_dir + "/loras");

    // ControlNet model: knob only, no auto-default. Empty means
    // "no ControlNet", which disables the feature entirely.
    ep.control_net = empty_ck(control_net_knob) ? std::string{}
                                                 : std::string(control_net_knob);

    return ep;
}

// Parse the multi-line LoRA spec string into a list of (path, weight)
// records. Format:
//   path : weight    (weight defaults to 1.0)
//   # comment        (ignored)
//   <blank>          (ignored)
// Relative paths are resolved against effective_loras_dir.
// LoRAs with weight 0.0 are skipped (no effect anyway).
static std::vector<nf::LoRASpec> parse_loras(
    const char* loras_text_knob,
    const std::string& effective_loras_dir)
{
    std::vector<nf::LoRASpec> result;
    if (!loras_text_knob || !*loras_text_knob) return result;

    auto trim = [](std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) { s.clear(); return; }
        const size_t b = s.find_last_not_of(" \t\r\n");
        s = s.substr(a, b - a + 1);
    };

    std::istringstream iss(loras_text_knob);
    std::string line;
    while (std::getline(iss, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;

        // Split "path : weight" by the LAST colon. Windows drive-letter
        // colons (position 1, e.g. "C:") must NOT be treated as the weight
        // separator - hence rfind plus the > 1 guard.
        std::string path;
        float weight = 1.0f;
        const size_t colon_pos = line.rfind(':');
        if (colon_pos != std::string::npos && colon_pos > 1) {
            std::string maybe_w = line.substr(colon_pos + 1);
            trim(maybe_w);
            try {
                size_t consumed = 0;
                const float w = std::stof(maybe_w, &consumed);
                if (consumed == maybe_w.size()) {
                    weight = w;
                    path = line.substr(0, colon_pos);
                    trim(path);
                } else {
                    path = line;  // trailing junk - not a weight
                }
            } catch (...) {
                path = line;  // not a float - treat whole line as path
            }
        } else {
            path = line;
        }

        if (path.empty()) continue;
        if (weight == 0.0f) continue;  // no-op LoRA

        // Resolve relative path against the loras_dir.
        bool is_absolute = false;
        if (path.size() >= 1 && (path[0] == '/' || path[0] == '\\')) is_absolute = true;
        if (path.size() >= 2 && path[1] == ':') is_absolute = true;
        if (!is_absolute && !effective_loras_dir.empty()) {
            path = effective_loras_dir + "/" + path;
        }

        nf::LoRASpec spec;
        spec.path   = std::move(path);
        spec.weight = weight;
        result.push_back(std::move(spec));
    }
    return result;
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
        clip_l_path_, t5xxl_path_, cache_dir_, loras_dir_,
        control_net_path_, model_type_);

    nf::CacheKeyBuilder kb;
    kb.add(std::string("AIGenerate.v1"));
    kb.add(static_cast<int32_t>(model_type_));
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

    // LoRAs are part of the input identity - same prompt with a different
    // LoRA stack produces a different image. Include each resolved path
    // and weight in the hash, in order.
    const std::vector<nf::LoRASpec> loras = parse_loras(loras_text_, ep.loras_dir);
    kb.add(std::string("loras"));
    kb.add(static_cast<int32_t>(loras.size()));
    for (const auto& l : loras) {
        kb.add(normalize_path(l.path));
        kb.add(l.weight);
    }

    // img2img state: strength, and if input 0 is connected, the upstream
    // node's hash (covers pixel content + format). When no input is
    // connected, strength has no effect, but we still include both in the
    // key so disconnecting and reconnecting a different image invalidates
    // the cache correctly.
    kb.add(std::string("img2img"));
    {
        Op* in = const_cast<AIGenerate*>(this)->input(0);
        if (in) {
            kb.add(strength_);
            DD::Image::Hash input_hash;
            in->append(input_hash);
            kb.add(static_cast<int64_t>(input_hash.value()));
        } else {
            kb.add(std::string("no_input"));
        }
    }

    // ControlNet state: model path, strength, and if an input is
    // connected, the upstream node's hash (covers pixel content + format).
    kb.add(std::string("controlnet"));
    kb.add(normalize_path(ep.control_net));
    if (!ep.control_net.empty()) {
        kb.add(control_strength_);
        Op* in = const_cast<AIGenerate*>(this)->input(1);
        if (in) {
            DD::Image::Hash input_hash;
            in->append(input_hash);
            kb.add(static_cast<int64_t>(input_hash.value()));
        } else {
            kb.add(std::string("no_input"));
        }
    }

    return kb.finalize();
}

std::string AIGenerate::result_path(const std::string& key) const
{
    const EffectivePaths ep = compute_effective_paths(
        models_dir_, diffusion_model_path_, vae_path_,
        clip_l_path_, t5xxl_path_, cache_dir_, loras_dir_,
        control_net_path_, model_type_);
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
        clip_l_path_, t5xxl_path_, cache_dir_, loras_dir_,
        control_net_path_, model_type_);
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
        clip_l_path_, t5xxl_path_, cache_dir_, loras_dir_,
        control_net_path_, model_type_);

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
    paths.control_net     = ep.control_net;  // empty when ControlNet off

    // Diffusion model is required for all model types.
    if (paths.diffusion_model.empty()) {
        error("AIGenerate: diffusion model path is empty");
        set_status("Error: diffusion model path missing");
        return;
    }

    // For FLUX, all four separate files are required (no embedded copies).
    // For SDXL/SD1.5, the supporting paths can be empty - sd.cpp will use
    // whatever is embedded in the diffusion model file.
    const bool is_flux = (model_type_ == 0);
    if (is_flux) {
        if (paths.vae.empty() || paths.clip_l.empty() || paths.t5xxl.empty()) {
            error("AIGenerate: FLUX requires VAE, CLIP-L, and T5-XXL paths");
            set_status("Error: FLUX model paths missing");
            return;
        }
    }

    // Verify any non-empty file paths actually exist on disk.
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
    if (!paths.vae.empty() && !exists(paths.vae)) {
        error("AIGenerate: missing VAE: %s", paths.vae.c_str());
        set_status("Error: missing VAE file");
        return;
    }
    if (!paths.clip_l.empty() && !exists(paths.clip_l)) {
        error("AIGenerate: missing CLIP-L: %s", paths.clip_l.c_str());
        set_status("Error: missing CLIP-L file");
        return;
    }
    if (!paths.t5xxl.empty() && !exists(paths.t5xxl)) {
        error("AIGenerate: missing T5-XXL: %s", paths.t5xxl.c_str());
        set_status("Error: missing T5-XXL file");
        return;
    }
    if (!paths.control_net.empty() && !exists(paths.control_net)) {
        error("AIGenerate: missing ControlNet model: %s",
              paths.control_net.c_str());
        set_status("Error: missing ControlNet file");
        return;
    }

    // img2img source pixels. Active whenever input 0 is connected;
    // strength comes from the knob. No model-path gating like ControlNet
    // since img2img uses the same diffusion model.
    std::vector<uint8_t> init_image_rgb;
    int init_image_w = 0;
    int init_image_h = 0;
    {
        Iop* init_in = dynamic_cast<Iop*>(input(0));
        if (init_in) {
            std::string read_err;
            if (!read_input_rgb_u8(init_in, init_image_rgb,
                                   init_image_w, init_image_h,
                                   read_err)) {
                error("AIGenerate: Source input read failed: %s",
                      read_err.c_str());
                set_status("Error: img2img input read failed");
                return;
            }
            std::fprintf(stderr,
                "[AIGenerate] img2img active: %dx%d source, strength %.2f\n",
                init_image_w, init_image_h,
                static_cast<double>(strength_));
        }
    }

    // ControlNet input pixels. Only read when BOTH the model path is set
    // AND the upstream input is connected. If model is set but input is
    // not, fall through to plain txt2img and warn in the log; this is
    // usually a setup mistake rather than intent.
    std::vector<uint8_t> control_image_rgb;
    int control_image_w = 0;
    int control_image_h = 0;
    if (!paths.control_net.empty()) {
        Iop* control_in = dynamic_cast<Iop*>(input(1));
        if (control_in) {
            std::string read_err;
            if (!read_input_rgb_u8(control_in, control_image_rgb,
                                   control_image_w, control_image_h,
                                   read_err)) {
                error("AIGenerate: ControlNet input read failed: %s",
                      read_err.c_str());
                set_status("Error: ControlNet input read failed");
                return;
            }
            std::fprintf(stderr,
                "[AIGenerate] ControlNet active: %s, %dx%d input, "
                "strength %.2f\n",
                paths.control_net.c_str(),
                control_image_w, control_image_h,
                static_cast<double>(control_strength_));
        } else {
            std::fprintf(stderr,
                "[AIGenerate] WARNING: ControlNet model is set but no "
                "input is connected. Falling back to plain txt2img.\n");
            // Clear path so SdSession doesn't try to load the
            // ControlNet for nothing.
            paths.control_net.clear();
        }
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

    // Resolve LoRAs against effective loras_dir. parse_loras drops
    // weight-0 entries and resolves relative paths.
    std::vector<nf::LoRASpec> loras = parse_loras(loras_text_, ep.loras_dir);

    // Validate every LoRA file actually exists, else the worker will
    // fail mysteriously inside sd.cpp. Drop missing ones and warn.
    {
        std::vector<nf::LoRASpec> kept;
        kept.reserve(loras.size());
        for (const auto& l : loras) {
            if (fs::exists(fs::u8path(l.path), ec)) {
                kept.push_back(l);
            } else {
                std::fprintf(stderr,
                    "[AIGenerate] LoRA file not found, skipping: %s\n",
                    l.path.c_str());
            }
        }
        loras = std::move(kept);
    }

    baking_key_ = key;
    set_progress(0.0f);
    set_status("Cooking 0%");

    // Capture img2img and ControlNet state by value into the worker.
    const float strength_val         = strength_;
    const float control_strength_val = control_strength_;

    worker_.start([prompt, neg_prompt, W, H, steps, seed, cfg, paths,
                   cache_path, loras,
                   init_image_rgb, init_image_w, init_image_h,
                   strength_val,
                   control_image_rgb, control_image_w, control_image_h,
                   control_strength_val]
                  (nf::AiWorkerContext& ctx) {
        ctx.set_status_text("Loading model");
        ctx.set_progress(0.01f);


        std::string err;
        auto& sess = get_session();
        if (!sess.ensure_loaded(paths, err)) {
            std::fprintf(stderr, "[AIGenerate] ensure_loaded FAILED: %s\n", err.c_str());
            throw std::runtime_error("Model load failed: " + err);
        }

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
        params.loras            = loras;
        params.init_image_rgb       = init_image_rgb;
        params.init_image_width     = init_image_w;
        params.init_image_height    = init_image_h;
        params.strength             = strength_val;
        params.control_image_rgb    = control_image_rgb;
        params.control_image_width  = control_image_w;
        params.control_image_height = control_image_h;
        params.control_strength     = control_strength_val;

        // Map sd.cpp's per-step progress (0..steps) into our 5%-90%
        // visible progress band, leaving headroom for cache write.
        auto progress_cb = [&ctx](int step, int total) -> bool {
            if (total > 0) {
                float frac = static_cast<float>(step) /
                             static_cast<float>(total);
                ctx.set_progress(0.05f + frac * 0.85f);
            }
            return !ctx.should_cancel();
        };

        std::vector<float> out_rgb;
        int out_w = 0, out_h = 0;
        if (!sess.generate(params, out_rgb, out_w, out_h,
                           progress_cb, err)) {
            std::fprintf(stderr, "[AIGenerate] generate FAILED: %s\n", err.c_str());
            throw std::runtime_error("Generation failed: " + err);
        }
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
