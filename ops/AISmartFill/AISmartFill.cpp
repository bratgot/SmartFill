// nuke-ai-fill / ops / AISmartFill / AISmartFill.cpp
//
// AI Smart Fill - LaMa-based context-aware fill for Nuke 14.
//
// STUB PHASE: Inference is replaced with a placeholder that fills the
// masked region with neutral grey. The purpose of this iteration is
// to prove the build, plugin registration, knob panel, and input
// routing work end to end. Real LaMa via ONNX Runtime arrives in a
// later iteration; the async worker + cache infrastructure is
// already shipped and unit-tested.
//
// Per NDK_NOTES section 5.2: every Op callback runs inside try/catch
// so an unhandled exception cannot escape into Nuke's stack.
//
// Strict ASCII per NDK_NOTES section 6.1.

#include <DDImage/Iop.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>

// NDK_NOTES section 2.1: wingdi.h's POINTS macro clobbers Nuke types.
// On Nuke 14 the conflict comes in transitively. Defensive undef.
#ifdef POINTS
#  undef POINTS
#endif

using namespace DD::Image;

namespace {

const char* const kHelp =
    "AI Smart Fill (stub release).\n"
    "\n"
    "Inputs:\n"
    "  Source - image to fill into\n"
    "  Mask   - alpha channel marks the area to fill\n"
    "\n"
    "This release is a placeholder that fills the masked area with\n"
    "neutral grey. Real LaMa inference will land in a future build.\n"
    "\n"
    "License: MIT.";

} // anonymous namespace

class AISmartFill : public Iop
{
public:
    static const Description description;

    explicit AISmartFill(Node* node)
        : Iop(node)
        , status_("(stub) ready")
        , mask_threshold_(0.5f)
    {
    }

    // ----- Identity -----

    const char* Class() const override   { return description.name; }
    const char* node_help() const override { return kHelp; }

    // ----- Inputs -----

    int  minimum_inputs() const override { return 2; }
    int  maximum_inputs() const override { return 2; }

    const char* input_label(int n, char* /*buffer*/) const override {
        switch (n) {
            case 0: return "Source";
            case 1: return "Mask";
            default: return nullptr;
        }
    }

    // Allow either input to be disconnected without erroring out.
    // The behavior with no mask is pass-through.
    bool test_input(int /*n*/, Op* /*op*/) const override { return true; }

    // ----- Knobs -----

    void knobs(Knob_Callback f) override;

    // ----- Cook -----

    void _validate(bool for_real) override;
    void _request(int x, int y, int r, int t,
                  ChannelMask channels, int count) override;
    void engine(int y, int x, int r, ChannelMask channels, Row& out) override;

private:
    // Cached mask input pointer, set in _validate. Null if no mask
    // connected.
    Iop* mask_iop_ = nullptr;

    // Knob-backed state.
    std::string status_;       // read-only display
    float       mask_threshold_;
};

// ----------------------------------------------------------------------
// Knobs
// ----------------------------------------------------------------------

void AISmartFill::knobs(Knob_Callback f)
{
    Tab_knob(f, "AI Smart Fill");

    Float_knob(f, &mask_threshold_, IRange(0.0f, 1.0f),
               "mask_threshold", "Mask Threshold");
    Tooltip(f,
        "Mask alpha values above this threshold are treated as 'fill "
        "this area'. Below the threshold, the source is passed through "
        "unchanged.");

    Divider(f, "");

    // Read-only status line. Knob::NO_ANIMATION blocks TCL evaluation
    // (NDK_NOTES section 4) - critical for any text the worker thread
    // may eventually feed into here.
    Knob* k = String_knob(f, &status_, "status", "Status");
    if (k) {
        SetFlags(f, Knob::NO_ANIMATION | Knob::DISABLED);
    }

    Divider(f, "");

    // Stub-phase notice. Make it loud so nobody mistakes this build
    // for the finished thing.
    Text_knob(f, "info",
        "STUB BUILD - masked area fills with neutral grey.\n"
        "Real LaMa inference is not yet wired in.");
}

// ----------------------------------------------------------------------
// _validate / _request
// ----------------------------------------------------------------------

void AISmartFill::_validate(bool for_real)
{
    try {
        // Source defines the output bounding box and channel set.
        copy_info(0);

        // Cache the mask input pointer for engine(). It is legal for
        // input(1) to be null (user has not connected anything).
        mask_iop_ = dynamic_cast<Iop*>(input(1));
        if (mask_iop_) {
            mask_iop_->validate(for_real);
        }
    }
    catch (const std::exception& e) {
        error("AISmartFill::_validate failed: %s", e.what());
    }
    catch (...) {
        error("AISmartFill::_validate failed: unknown exception");
    }
}

void AISmartFill::_request(int x, int y, int r, int t,
                           ChannelMask channels, int count)
{
    try {
        input0().request(x, y, r, t, channels, count);
        if (mask_iop_) {
            // We only ever read alpha from the mask input.
            mask_iop_->request(x, y, r, t, Mask_Alpha, count);
        }
    }
    catch (const std::exception& e) {
        error("AISmartFill::_request failed: %s", e.what());
    }
    catch (...) {
        error("AISmartFill::_request failed: unknown exception");
    }
}

// ----------------------------------------------------------------------
// engine
// ----------------------------------------------------------------------

void AISmartFill::engine(int y, int x, int r,
                         ChannelMask channels, Row& out)
{
    try {
        // Read the source row.
        Row src(x, r);
        input0().get(y, x, r, channels, src);
        if (aborted()) return;

        // No mask connected -> pass-through.
        if (!mask_iop_) {
            foreach (c, channels) {
                const float* in_p  = src[c];
                float*       out_p = out.writable(c);
                for (int xi = x; xi < r; ++xi) {
                    out_p[xi] = in_p[xi];
                }
            }
            return;
        }

        // Read the mask row (alpha only).
        Row mask_row(x, r);
        mask_iop_->get(y, x, r, Mask_Alpha, mask_row);
        if (aborted()) return;

        const float* mask_a   = mask_row[Chan_Alpha];
        const float  threshold = mask_threshold_;

        // STUB FILL: pixels above threshold become neutral grey.
        // Replace this loop with LaMa inference output in the next
        // iteration. The loop here is row-local and synchronous;
        // the real version will read from a precomputed cached EXR
        // populated by an async worker (see core/ai_worker.h).
        constexpr float kStubFill = 0.5f;

        foreach (c, channels) {
            const float* in_p  = src[c];
            float*       out_p = out.writable(c);

            // Alpha channel: pass through (we are not modifying matte).
            if (c == Chan_Alpha) {
                for (int xi = x; xi < r; ++xi) {
                    out_p[xi] = in_p[xi];
                }
                continue;
            }

            for (int xi = x; xi < r; ++xi) {
                out_p[xi] = (mask_a[xi] > threshold) ? kStubFill
                                                    : in_p[xi];
            }
        }
    }
    catch (const std::exception& e) {
        error("AISmartFill::engine failed: %s", e.what());
    }
    catch (...) {
        error("AISmartFill::engine failed: unknown exception");
    }
}

// ----------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------
//
// NDK_NOTES section 1.7: the DLL filename MUST match the first arg
// of Description (the Op class name). One source file -> one DLL ->
// one Description. The output filename is set in this Op's
// CMakeLists.txt as OUTPUT_NAME "AISmartFill".

static Iop* build(Node* node) { return new AISmartFill(node); }
const Iop::Description AISmartFill::description(
    "AISmartFill",
    "Filter/AISmartFill",
    build
);
