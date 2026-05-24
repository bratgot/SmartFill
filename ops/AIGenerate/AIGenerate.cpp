// nuke-ai-fill / ops / AIGenerate / AIGenerate.cpp
//
// Chunk D1 stub: registers the AIGenerate Op so we can verify the
// full build chain (sd.cpp + ggml-cuda + our static core + Nuke NDK)
// links cleanly. No actual generation yet - just emits a black frame
// at requested dimensions.
//
// D2 will add the SdSession wrapper.
// D3 will replace this stub with the real worker integration.

#include <DDImage/Iop.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>

#ifdef POINTS
#  undef POINTS
#endif

#include <string>

using namespace DD::Image;

class AIGenerate : public Iop {
public:
    static const Description description;

    explicit AIGenerate(Node* node)
        : Iop(node)
        , width_(1024)
        , height_(1024)
    {
        inputs(0);
    }

    const char* Class() const override { return description.name; }
    const char* node_help() const override {
        return "AI Generate (stub)\n\n"
               "Stable Diffusion / FLUX-based text-to-image generation.\n"
               "Phase 1 stub: emits a black frame at requested dimensions.";
    }

    int minimum_inputs() const override { return 0; }
    int maximum_inputs() const override { return 0; }

    void knobs(Knob_Callback f) override {
        Int_knob(f, &width_,  "width",  "Width");
        Int_knob(f, &height_, "height", "Height");
    }

    void _validate(bool /*for_real*/) override {
        info_.full_size_format(Format(width_, height_));
        info_.format(Format(width_, height_));
        info_.set(0, 0, width_, height_);
        info_.channels(Mask_RGB);
    }

    void _request(int /*x*/, int /*y*/, int /*r*/, int /*t*/,
                  ChannelMask /*channels*/, int /*count*/) override {}

    void engine(int /*y*/, int x, int r,
                ChannelMask channels, Row& out) override {
        foreach (c, channels) {
            float* p = out.writable(c);
            for (int i = x; i < r; ++i) p[i] = 0.0f;
        }
    }

private:
    int width_;
    int height_;
};

static Iop* build(Node* node) { return new AIGenerate(node); }
const Iop::Description AIGenerate::description(
    "AIGenerate",
    "Image/AIGenerate",
    build
);
