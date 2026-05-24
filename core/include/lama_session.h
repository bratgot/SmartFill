// nuke-ai-fill / core / lama_session.h
//
// LaMa inpainting session via ONNX Runtime. PIMPL idiom keeps ORT
// headers fully out of this header file - clients see only standard
// C++ types. Important because ORT headers interact awkwardly with
// windows.h and we want to keep the public surface lean.
//
// Lifecycle:
//   LamaSession s("path/to/lama_fp32.onnx", Backend::Cuda);  // cheap, no model load
//   s.run(rgb_in, mask, rgb_out);                            // first call lazy-loads
//   s.run(...);                                              // subsequent calls reuse
//
// Threading: ORT documents Session::Run() as thread-safe for concurrent
// calls. The lazy initialization is guarded by std::call_once. Calling
// run() from multiple worker threads on the same LamaSession is safe.
//
// Per NDK_NOTES section 6.1 (ASCII only) and section 5 (worker boundary
// exception handling - run() throws std::runtime_error; callers catch).

#ifndef NUKE_AI_FILL_LAMA_SESSION_H
#define NUKE_AI_FILL_LAMA_SESSION_H

#include <memory>
#include <string>

namespace nukeaifill {

class LamaSession {
public:
    enum class Backend : int {
        // Preferred at construction. Actual backend used is reported
        // by active_backend() after first successful run() - may differ
        // if Cuda was requested but unavailable.
        Cpu  = 0,
        Cuda = 1,
    };

    // LaMa is a fixed-shape model. These are immutable contract values
    // exposed for buffer sizing on the caller side.
    static constexpr int kImageSize = 512;
    static constexpr int kChannels  = 3;
    static constexpr int kRgbFloats  = kChannels * kImageSize * kImageSize;
    static constexpr int kMaskFloats = kImageSize * kImageSize;

    // model_path: full path to lama_fp32.onnx. Must remain valid
    //   until the session is destroyed (we copy the string internally,
    //   so the caller can free it after the constructor returns).
    // backend: preferred execution provider. Cuda falls back to Cpu if
    //   the CUDA EP cannot be instantiated.
    //
    // Construction does NOT load the model. The model file is read
    // and the ORT session built on the first call to run(). This
    // keeps plugin load fast and defers any disk/GPU errors until
    // they can be caught in a worker thread.
    LamaSession(std::string model_path, Backend backend);

    ~LamaSession();

    LamaSession(const LamaSession&)            = delete;
    LamaSession& operator=(const LamaSession&) = delete;
    LamaSession(LamaSession&&)                 = delete;
    LamaSession& operator=(LamaSession&&)      = delete;

    // Run inpainting.
    //
    //   rgb_in : [kRgbFloats] floats, RGB interleaved (HWC), values
    //            in [0, 1]. Layout = pixel(y, x) channel c at
    //            index (y * 512 + x) * 3 + c.
    //   mask   : [kMaskFloats] floats. 1.0 means "inpaint this pixel",
    //            0.0 means "keep". Soft values in between are allowed
    //            but LaMa was trained on binary masks; soft edges may
    //            be ignored by the network. Layout = mask(y, x) at
    //            index y * 512 + x.
    //   rgb_out: [kRgbFloats] floats, same layout as rgb_in. The
    //            caller owns the buffer; we only write to it.
    //
    // Throws std::runtime_error on any failure (file not found,
    // model load error, CUDA init failure that also failed CPU
    // fallback, Ort::Exception during inference, etc).
    void run(const float* rgb_in,
             const float* mask,
             float* rgb_out);

    // Reports the backend actually in use. Valid only after the
    // first successful run() - returns the preferred backend if
    // never run.
    Backend active_backend() const;

    // Diagnostic: input/output tensor names discovered from the
    // model graph. Useful if our hard-coded I/O-order assumption
    // ever needs revisiting (logged on first run for sanity).
    // Empty until the session is initialized.
    std::string input_name(int index) const;
    std::string output_name(int index) const;
    int input_count() const;
    int output_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nukeaifill

#endif // NUKE_AI_FILL_LAMA_SESSION_H
