// nuke-ai-fill / ops / AISmartFill / AISmartFill.cpp
//
// AI Smart Fill - context-aware inpainting for Nuke 14, backed by
// LaMa via ONNX Runtime.
//
// Architecture:
//
//   knob_changed("bake") [UI thread]
//      -> extract source + mask pixels into shared_ptr buffers
//      -> compute SHA-256 cache key from input hashes + knob state
//      -> if cache file exists for this key: just invalidate(), done
//      -> else: lazy-create LamaSession, spawn AiWorker with buffers
//
//   AiWorker thread [background]
//      -> calls inpaint_image() with progress callback
//      -> writes result via image_cache_write()
//      -> on exception: stored in worker.error_message()
//
//   menu.py idle timer [UI thread]
//      -> polls all AISmartFill nodes whose status reports "Cooking"
//      -> calls forceValidate() which fires _validate()
//
//   _validate() [UI thread]
//      -> poll worker state; update status / progress display knobs
//      -> on Done: load cache file into in-memory buffer
//
//   engine() [render thread]
//      -> if loaded buffer matches current cache key: emit from buffer
//      -> else: pass-through source unchanged
//
// Per NDK_NOTES section 5: worker never touches knobs, invalidate, or
// asapUpdate. All Nuke API calls from main thread.
//
// String knobs (File_knob, String_knob) take const char**, not
// std::string*. Members are const char* and mutations go through
// Knob::set_text() which is documented main-thread-only.
//
// Strict ASCII per NDK_NOTES 6.1.

#include "ai_cache.h"
#include "ai_hash.h"
#include "ai_worker.h"
#include "image_cache.h"
#include "plugin_path.h"

#ifdef NUKE_AI_FILL_HAS_LAMA
#  include "lama_session.h"
#  include "lama_tiler.h"
#endif

#include <DDImage/Hash.h>
#include <DDImage/Iop.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>

#ifdef POINTS
#  undef POINTS
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace DD::Image;
namespace fs = std::filesystem;
namespace nf = nukeaifill;

namespace {

const char* const kHelp =
    "AI Smart Fill\n"
    "\n"
    "Context-aware inpainting backed by LaMa via ONNX Runtime.\n"
    "\n"
    "Inputs:\n"
    "  Source - image to fill into\n"
    "  Mask   - alpha channel marks pixels to inpaint\n"
    "\n"
    "Workflow:\n"
    "  1. Connect Source and Mask inputs\n"
    "  2. Press Bake to run inpainting (background thread)\n"
    "  3. Result is cached on disk by input hash; subsequent renders\n"
    "     are instant unless inputs change\n"
    "\n"
    "License: MIT.";

// Safely convert Nuke const char* to std::string (handles null).
std::string ck_str(const char* s) {
    return s ? std::string(s) : std::string{};
}

// True if the knob string pointer is null or empty.
bool empty_ck(const char* s) {
    return s == nullptr || s[0] == '\0';
}

// Strip trailing whitespace and normalize backslashes (NDK_NOTES 4).
std::string normalize_path(std::string s) {
    while (!s.empty() && (s.back() == ' '  || s.back() == '\t' ||
                          s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}

} // anonymous namespace

class AISmartFill : public Iop
{
public:
    static const Description description;

    explicit AISmartFill(Node* node)
        : Iop(node)
        , mask_threshold_(0.5f)
        , backend_(1)            // default CUDA
        , progress_(0.0f)
        , model_path_("")
        , cache_dir_("")
        , status_text_("(idle) press Bake to inpaint")
    {
    }

    ~AISmartFill() override = default;

    const char* Class() const override   { return description.name; }
    const char* node_help() const override { return kHelp; }

    int  minimum_inputs() const override { return 2; }
    int  maximum_inputs() const override { return 2; }

    const char* input_label(int n, char* /*buf*/) const override {
        switch (n) {
            case 0: return "Source";
            case 1: return "Mask";
            default: return nullptr;
        }
    }

    bool test_input(int /*n*/, Op* /*op*/) const override { return true; }

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;

    // Override append() to include our cache-load state in the Op hash.
    // Without this, Nuke caches our cooked pixels and never recooks even
    // after we call invalidate() from _validate, because the input chain
    // (and thus our default hash) hasn't changed - only our internal
    // cache state has. Including loaded_key_ here makes the hash flip
    // when the worker completes and try_load_cache_for_key populates
    // the buffer, which makes Nuke notice and re-cook engine().
    void append(Hash& hash) override {
        Iop::append(hash);
        hash.append(loaded_key_);
    }

    void _validate(bool for_real) override;
    void _request(int x, int y, int r, int t,
                  ChannelMask channels, int count) override;
    void engine(int y, int x, int r,
                ChannelMask channels, Row& out) override;

private:
    // Mutate a string knob's display value. NDK_NOTES 5.1: only call
    // from main thread (knob_changed, _validate, knobs).
    void set_status(const char* s);
    void set_knob_text(const char* knob_name, const char* value);
    void set_progress(float v);

    void resolve_default_paths();
    std::string compute_cache_key();
    std::string result_path(const std::string& key) const;
    void poll_worker();
    void try_load_cache_for_key(const std::string& key);
    void start_bake();
    void clear_cache_dir();

    // ---- Knob-backed state ----
    float       mask_threshold_;
    int         backend_;         // 0=CPU, 1=CUDA
    float       progress_;        // read-only display
    const char* model_path_;
    const char* cache_dir_;
    const char* status_text_;     // read-only display

    // ---- Runtime state ----
    Iop* mask_iop_ = nullptr;

    std::string current_key_;

    std::vector<float> cached_pixels_;
    int                cached_w_ = 0;
    int                cached_h_ = 0;
    std::string        loaded_key_;

    // ORDERING: session_ first, worker_ last. Worker destructor cancels
    // + joins, so we want it to run BEFORE session_ goes away.
#ifdef NUKE_AI_FILL_HAS_LAMA
    std::unique_ptr<nf::LamaSession> session_;
#endif

    std::string baking_key_;

    nf::AiWorker worker_;
};

// ----------------------------------------------------------------------
// Knob mutation helpers
// ----------------------------------------------------------------------

void AISmartFill::set_knob_text(const char* knob_name, const char* value)
{
    if (Knob* k = knob(knob_name)) {
        k->set_text(value ? value : "");
    }
}

void AISmartFill::set_status(const char* s)
{
    set_knob_text("status", s);
}

// Update the progress knob's value AND signal the GUI to redraw.
// Direct writes to progress_ (the variable backing the knob) update
// memory but do NOT cause the panel display to refresh. Knob::set_value()
// goes through the proper mutation path that triggers UI updates.
void AISmartFill::set_progress(float v)
{
    progress_ = v;
    if (Knob* k = knob("progress")) {
        k->set_value(static_cast<double>(v));
    }
}

// ----------------------------------------------------------------------
// Knob panel
// ----------------------------------------------------------------------

void AISmartFill::knobs(Knob_Callback f)
{
    Tab_knob(f, "AI Smart Fill");

    File_knob(f, &model_path_, "model_path", "Model");
    Tooltip(f,
        "Path to LaMa ONNX model. Defaults to <plugin>/models/lama_fp32.onnx.");
    SetFlags(f, Knob::NO_ANIMATION);

    static const char* const backend_labels[] = { "CPU", "CUDA", nullptr };
    Enumeration_knob(f, &backend_, backend_labels, "backend", "Backend");
    Tooltip(f, "Execution provider. CUDA requires a compatible NVIDIA GPU.");

    Float_knob(f, &mask_threshold_, IRange(0.0f, 1.0f),
               "mask_threshold", "Mask Threshold");
    Tooltip(f,
        "Mask alpha at or above this value is treated as 'inpaint here'.");

    Divider(f, "Cache");

    File_knob(f, &cache_dir_, "cache_dir", "Cache Directory");
    Tooltip(f,
        "Where baked results are stored as .aifill files. "
        "Defaults to %APPDATA%/nuke-ai-fill/cache on Windows.");
    SetFlags(f, Knob::NO_ANIMATION);

    Button(f, "bake", "Bake");
    Tooltip(f,
        "Run LaMa inpainting on the current source and mask, store the "
        "result in the cache. Subsequent renders read from the cache "
        "until inputs change.");

    Button(f, "clear_cache", "Clear Cache");
    Tooltip(f, "Delete all .aifill files in the cache directory.");

    Divider(f, "Status");

    Knob* sk = String_knob(f, &status_text_, "status", "Status");
    if (sk) SetFlags(f, Knob::NO_ANIMATION | Knob::DISABLED);

    Float_knob(f, &progress_, IRange(0.0f, 1.0f), "progress", "Progress");
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION);
}

int AISmartFill::knob_changed(Knob* k)
{
    if (!k) return 0;
    try {
        if (k->is("bake")) {
            start_bake();
            return 1;
        }
        if (k->is("clear_cache")) {
            clear_cache_dir();
            return 1;
        }
    }
    catch (const std::exception& e) {
        error("AISmartFill::knob_changed: %s", e.what());
    }
    catch (...) {
        error("AISmartFill::knob_changed: unknown exception");
    }
    return 0;
}

// ----------------------------------------------------------------------
// Default-path resolution
// ----------------------------------------------------------------------

void AISmartFill::resolve_default_paths()
{
    if (empty_ck(cache_dir_)) {
        const std::string d = nf::default_cache_dir();
        if (!d.empty()) {
            set_knob_text("cache_dir", d.c_str());
        }
    }
    if (empty_ck(model_path_)) {
        std::string dir = nf::current_plugin_dir();
        if (!dir.empty()) {
            const std::string mp = dir + "/models/lama_fp32.onnx";
            set_knob_text("model_path", mp.c_str());
        }
    }
}

std::string AISmartFill::result_path(const std::string& key) const
{
    std::string dir = normalize_path(ck_str(cache_dir_));
    if (dir.empty() || key.empty()) return std::string{};
    return dir + "/" + key + ".aifill";
}

// ----------------------------------------------------------------------
// Cache key
// ----------------------------------------------------------------------

std::string AISmartFill::compute_cache_key()
{
    nf::CacheKeyBuilder kb;
    kb.add(std::string("AISmartFill.v1"));

    if (input(0)) {
        Hash h;
        input(0)->append(h);
        kb.add_op_hash(static_cast<uint64_t>(h.value()));
    } else {
        kb.add_op_hash(0);
    }

    if (input(1)) {
        Hash h;
        input(1)->append(h);
        kb.add_op_hash(static_cast<uint64_t>(h.value()));
    } else {
        kb.add_op_hash(0);
    }

    kb.add(mask_threshold_);
    kb.add(static_cast<int32_t>(backend_));
    kb.add(normalize_path(ck_str(model_path_)));

    return kb.finalize();
}

// ----------------------------------------------------------------------
// Cook stages
// ----------------------------------------------------------------------

void AISmartFill::_validate(bool for_real)
{
    try {
        copy_info(0);

        mask_iop_ = dynamic_cast<Iop*>(input(1));
        if (mask_iop_) {
            mask_iop_->validate(for_real);
        }

        resolve_default_paths();
        poll_worker();

        const std::string prev_loaded = loaded_key_;
        current_key_ = compute_cache_key();
        try_load_cache_for_key(current_key_);

        // If we just transitioned to a freshly-loaded cache for the
        // current key (i.e. the bake completed and the worker's cache
        // file became readable since last validate), force a re-cook.
        // Without this, Nuke serves the stale pass-through result from
        // before the bake completed.
        if (loaded_key_ == current_key_ &&
            !loaded_key_.empty() &&
            prev_loaded != loaded_key_) {
            invalidate();
            asapUpdate();
        }
    }
    catch (const std::exception& e) {
        error("AISmartFill::_validate: %s", e.what());
    }
    catch (...) {
        error("AISmartFill::_validate: unknown exception");
    }
}

void AISmartFill::_request(int x, int y, int r, int t,
                           ChannelMask channels, int count)
{
    try {
        input0().request(x, y, r, t, channels, count);
        if (mask_iop_) {
            mask_iop_->request(x, y, r, t, Mask_Alpha, count);
        }
    }
    catch (const std::exception& e) {
        error("AISmartFill::_request: %s", e.what());
    }
    catch (...) {
        error("AISmartFill::_request: unknown exception");
    }
}

void AISmartFill::engine(int y, int x, int r,
                         ChannelMask channels, Row& out)
{
    try {
        Row src(x, r);
        input0().get(y, x, r, channels, src);
        if (aborted()) return;

        const bool use_cache =
            !cached_pixels_.empty() &&
            cached_w_ > 0 && cached_h_ > 0 &&
            !loaded_key_.empty() &&
            loaded_key_ == current_key_;

        if (!use_cache) {
            foreach (c, channels) {
                const float* in_p  = src[c];
                float*       out_p = out.writable(c);
                for (int xi = x; xi < r; ++xi) {
                    out_p[xi] = in_p[xi];
                }
            }
            return;
        }

        const Box bbox = info().box();
        const int cache_y = y - bbox.y();

        foreach (c, channels) {
            const float* in_p  = src[c];
            float*       out_p = out.writable(c);

            int channel_idx = -1;
            if      (c == Chan_Red)   channel_idx = 0;
            else if (c == Chan_Green) channel_idx = 1;
            else if (c == Chan_Blue)  channel_idx = 2;

            if (channel_idx < 0 || cache_y < 0 || cache_y >= cached_h_) {
                for (int xi = x; xi < r; ++xi) out_p[xi] = in_p[xi];
                continue;
            }

            const float* cache_row =
                cached_pixels_.data() +
                static_cast<size_t>(cache_y) * cached_w_ * 3;

            for (int xi = x; xi < r; ++xi) {
                const int cache_x = xi - bbox.x();
                if (cache_x >= 0 && cache_x < cached_w_) {
                    out_p[xi] = cache_row[cache_x * 3 + channel_idx];
                } else {
                    out_p[xi] = in_p[xi];
                }
            }
        }
    }
    catch (const std::exception& e) {
        error("AISmartFill::engine: %s", e.what());
    }
    catch (...) {
        error("AISmartFill::engine: unknown exception");
    }
}

// ----------------------------------------------------------------------
// Cache file management
// ----------------------------------------------------------------------

void AISmartFill::try_load_cache_for_key(const std::string& key)
{
    if (key.empty()) {
        cached_pixels_.clear();
        cached_w_ = 0;
        cached_h_ = 0;
        loaded_key_.clear();
        return;
    }

    if (key == loaded_key_ && !cached_pixels_.empty()) {
        return;
    }

    const std::string path = result_path(key);
    if (path.empty()) return;

    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) {
        cached_pixels_.clear();
        cached_w_ = 0;
        cached_h_ = 0;
        loaded_key_.clear();
        return;
    }

    auto result = nf::image_cache_read(path);
    if (!result.ok) {
        warning("AISmartFill: cache file unreadable (%s); will re-bake on next press",
                result.error.c_str());
        cached_pixels_.clear();
        cached_w_ = 0;
        cached_h_ = 0;
        loaded_key_.clear();
        return;
    }

    cached_pixels_ = std::move(result.pixels);
    cached_w_      = result.width;
    cached_h_      = result.height;
    loaded_key_    = key;
}

void AISmartFill::clear_cache_dir()
{
    resolve_default_paths();
    if (empty_ck(cache_dir_)) {
        set_status("Cache dir not set");
        return;
    }
    std::error_code ec;
    fs::path dir = fs::u8path(normalize_path(ck_str(cache_dir_)));
    if (!fs::is_directory(dir, ec)) {
        set_status("Cache dir does not exist");
        return;
    }

    int removed = 0;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".aifill") {
            std::error_code rm_ec;
            if (fs::remove(entry.path(), rm_ec) && !rm_ec) ++removed;
        }
    }

    cached_pixels_.clear();
    cached_w_ = 0;
    cached_h_ = 0;
    loaded_key_.clear();

    char buf[96];
    std::snprintf(buf, sizeof(buf), "Cleared %d cached file(s)", removed);
    set_status(buf);
}

// ----------------------------------------------------------------------
// Worker lifecycle
// ----------------------------------------------------------------------

void AISmartFill::poll_worker()
{
    const auto state = worker_.state();

    if (state == nf::AiWorkerState::Idle) {
        return;
    }

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
        if (loaded_key_ != baking_key_) {
            cached_pixels_.clear();
            cached_w_ = 0;
            cached_h_ = 0;
            loaded_key_.clear();
        }
        baking_key_.clear();
        // Signal the UI to redraw. Without this, the viewer keeps
        // showing the pre-bake cooked output even though our hash
        // has changed (via append() including loaded_key_) and
        // invalidate() has been called. asapUpdate is the explicit
        // "redraw now" signal that triggers the viewer to recook.
        asapUpdate();
        return;
    }

    if (state == nf::AiWorkerState::Error) {
        std::string msg = worker_.error_message();
        error("AISmartFill: %s", msg.c_str());
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
// Bake
// ----------------------------------------------------------------------

void AISmartFill::start_bake()
{
#ifndef NUKE_AI_FILL_HAS_LAMA
    error("AISmartFill: built without LaMa support. "
          "Reconfigure with -DORT_DIR=<onnxruntime-path> and rebuild.");
    set_status("Error: no LaMa support");
    return;
#else
    if (worker_.state() == nf::AiWorkerState::Running) {
        set_status("Already cooking");
        return;
    }
    if (worker_.state() != nf::AiWorkerState::Idle) {
        worker_.reset();
    }

    if (!input(0)) {
        error("AISmartFill: no source connected");
        set_status("Error: no source");
        return;
    }
    if (!input(1)) {
        error("AISmartFill: no mask connected");
        set_status("Error: no mask");
        return;
    }

    resolve_default_paths();
    validate(true);

    const Box bbox = input(0)->info().box();
    const int W = bbox.w();
    const int H = bbox.h();
    if (W <= 0 || H <= 0) {
        error("AISmartFill: source has zero size");
        set_status("Error: empty source");
        return;
    }

    const std::string key  = compute_cache_key();
    const std::string path = result_path(key);

    std::error_code ec;
    if (fs::exists(fs::u8path(path), ec)) {
        set_status("Already cached for this input");
        set_progress(1.0f);
        current_key_ = key;
        try_load_cache_for_key(key);
        invalidate();
        return;
    }

    input(0)->request(bbox.x(), bbox.y(), bbox.r(), bbox.t(), Mask_RGB,   0);
    input(1)->request(bbox.x(), bbox.y(), bbox.r(), bbox.t(), Mask_Alpha, 0);

    auto rgb  = std::make_shared<std::vector<float>>(
        static_cast<size_t>(W) * H * 3, 0.0f);
    auto mask = std::make_shared<std::vector<float>>(
        static_cast<size_t>(W) * H, 0.0f);

    Iop* src_iop  = dynamic_cast<Iop*>(input(0));
    Iop* mask_in  = dynamic_cast<Iop*>(input(1));
    if (!src_iop || !mask_in) {
        error("AISmartFill: inputs are not Iops");
        set_status("Error: invalid inputs");
        return;
    }

    for (int y = 0; y < H; ++y) {
        Row row(bbox.x(), bbox.r());
        src_iop->get(bbox.y() + y, bbox.x(), bbox.r(), Mask_RGB, row);
        const float* rp = row[Chan_Red];
        const float* gp = row[Chan_Green];
        const float* bp = row[Chan_Blue];
        float* dst = rgb->data() + static_cast<size_t>(y) * W * 3;
        for (int x = 0; x < W; ++x) {
            const int sx = bbox.x() + x;
            dst[x * 3 + 0] = rp[sx];
            dst[x * 3 + 1] = gp[sx];
            dst[x * 3 + 2] = bp[sx];
        }
    }
    for (int y = 0; y < H; ++y) {
        Row row(bbox.x(), bbox.r());
        mask_in->get(bbox.y() + y, bbox.x(), bbox.r(), Mask_Alpha, row);
        const float* ap = row[Chan_Alpha];
        float* dst = mask->data() + static_cast<size_t>(y) * W;
        const float thr = mask_threshold_;
        for (int x = 0; x < W; ++x) {
            dst[x] = (ap[bbox.x() + x] >= thr) ? 1.0f : 0.0f;
        }
    }

    if (!session_) {
        nf::LamaSession::Backend backend = (backend_ == 1)
            ? nf::LamaSession::Backend::Cuda
            : nf::LamaSession::Backend::Cpu;
        session_ = std::make_unique<nf::LamaSession>(
            normalize_path(ck_str(model_path_)), backend);
    }

    nf::LamaSession* session_ptr = session_.get();

    baking_key_  = key;
    current_key_ = key;
    set_progress(0.0f);
    set_status("Cooking 0%");

    worker_.start([rgb, mask, W, H, path, session_ptr]
                  (nf::AiWorkerContext& ctx) {
        ctx.set_status_text("Preparing");
        ctx.set_progress(0.05f);

        std::vector<float> out(static_cast<size_t>(W) * H * 3, 0.0f);

        // Progress callback maps inpaint_image's internal [0,1] to the
        // 5%-90% range of the user-visible progress. inpaint_image
        // emits at four points: 0 (start), 0.2 (after resize), 0.8
        // (after inference), 1.0 (end). User sees: 5%, 22%, 73%, 90%.
        auto progress_cb = [](float f, void* u) -> bool {
            auto* c = static_cast<nf::AiWorkerContext*>(u);
            c->set_progress(0.05f + f * 0.85f);
            return !c->should_cancel();
        };

        ctx.set_status_text("Inferencing");
        nf::inpaint_image(*session_ptr,
                          rgb->data(), mask->data(),
                          W, H, out.data(),
                          progress_cb, &ctx);

        ctx.set_status_text("Writing cache");
        ctx.set_progress(0.93f);

        if (!nf::image_cache_write(path, out.data(), W, H, 3)) {
            throw std::runtime_error(
                std::string("failed to write cache file: ") + path);
        }
        ctx.set_progress(1.0f);
    });
#endif
}

// ----------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------

static Iop* build(Node* node) { return new AISmartFill(node); }
const Iop::Description AISmartFill::description(
    "AISmartFill",
    "Filter/AISmartFill",
    build
);
