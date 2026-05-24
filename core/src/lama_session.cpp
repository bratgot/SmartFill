// nuke-ai-fill / core / lama_session.cpp
//
// LaMa inpainting via ONNX Runtime. See lama_session.h.
//
// Per NDK_NOTES 6.1: strict ASCII.

#include "lama_session.h"

#include <onnxruntime_cxx_api.h>

#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace nukeaifill {

// ----------------------------------------------------------------------
// Impl - all ORT state lives here so the header stays clean
// ----------------------------------------------------------------------

struct LamaSession::Impl {
    std::string model_path;
    Backend     preferred_backend;
    Backend     active_backend{Backend::Cpu};

    std::once_flag        init_flag;
    std::exception_ptr    init_error;

    // ORT objects. Ort::Env must outlive every Session built from it.
    std::unique_ptr<Ort::Env>       env;
    std::unique_ptr<Ort::Session>   session;

    // Discovered I/O names from the model graph. ORT returns these
    // as AllocatedStringPtr; we copy into std::strings so they stay
    // alive for the lifetime of the session.
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;

    // Cached C-string views for the names, populated once and reused
    // on every run() to avoid per-call allocation.
    std::vector<const char*> input_name_cstrs;
    std::vector<const char*> output_name_cstrs;

    Impl(std::string path, Backend backend)
        : model_path(std::move(path))
        , preferred_backend(backend)
    {}

    void initialize();
    void ensure_initialized();
};

// ----------------------------------------------------------------------
// initialize - reads the model file, builds the ORT Session, discovers
// I/O names. Called at most once per Impl via std::call_once. Errors
// stash an exception_ptr that ensure_initialized() will rethrow on
// every subsequent call.
// ----------------------------------------------------------------------

void LamaSession::Impl::initialize()
{
    env = std::make_unique<Ort::Env>(
        ORT_LOGGING_LEVEL_WARNING, "nuke-ai-fill");

    Ort::SessionOptions opts;
    // NDK_NOTES section 5: ORT must not parallelize across threads
    // here - Nuke owns the outer parallelism. One intra-op thread is
    // enough; for inpainting on GPU this is dominated by the CUDA EP
    // anyway. For CPU mode we could raise this carefully later.
    opts.SetIntraOpNumThreads(1);
    opts.SetInterOpNumThreads(1);
    opts.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);

    // Try CUDA first if requested. Failure is non-fatal; fall back
    // to CPU. We keep the exception inline because Ort::Exception
    // doesn't derive from std::exception in all ORT versions.
    if (preferred_backend == Backend::Cuda) {
        try {
            OrtCUDAProviderOptions cuda_opts;
            std::memset(&cuda_opts, 0, sizeof(cuda_opts));
            cuda_opts.device_id                 = 0;
            cuda_opts.arena_extend_strategy     = 0;
            cuda_opts.gpu_mem_limit             = 0;  // unlimited
            cuda_opts.cudnn_conv_algo_search    =
                OrtCudnnConvAlgoSearchExhaustive;
            cuda_opts.do_copy_in_default_stream = 1;
            opts.AppendExecutionProvider_CUDA(cuda_opts);
            active_backend = Backend::Cuda;
        }
        catch (const Ort::Exception&) {
            active_backend = Backend::Cpu;
        }
        catch (const std::exception&) {
            active_backend = Backend::Cpu;
        }
    }

    // Build the session. On Windows ORT wants wchar_t* for the path,
    // so we transcode. For non-ASCII paths use the Win32 API for
    // correctness; for typical ASCII install paths a byte-to-wchar
    // widening would also work but MultiByteToWideChar is robust.
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0,
        model_path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        throw std::runtime_error(
            "LamaSession: failed to convert model path to wide string");
    }
    std::wstring wpath(static_cast<size_t>(wlen - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
        model_path.c_str(), -1, wpath.data(), wlen);
    try {
        session = std::make_unique<Ort::Session>(*env, wpath.c_str(), opts);
    }
    catch (const Ort::Exception& e) {
        throw std::runtime_error(
            std::string("LamaSession: failed to load model: ") + e.what());
    }
#else
    try {
        session = std::make_unique<Ort::Session>(
            *env, model_path.c_str(), opts);
    }
    catch (const Ort::Exception& e) {
        throw std::runtime_error(
            std::string("LamaSession: failed to load model: ") + e.what());
    }
#endif

    // Discover input/output names from the loaded graph. ORT returns
    // them as AllocatedStringPtr; we copy into std::strings for stable
    // storage and cache const char* views for hot-path reuse.
    Ort::AllocatorWithDefaultOptions allocator;

    const size_t n_in = session->GetInputCount();
    input_names.reserve(n_in);
    input_name_cstrs.reserve(n_in);
    for (size_t i = 0; i < n_in; ++i) {
        Ort::AllocatedStringPtr name =
            session->GetInputNameAllocated(i, allocator);
        input_names.emplace_back(name.get());
    }
    for (const auto& s : input_names) {
        input_name_cstrs.push_back(s.c_str());
    }

    const size_t n_out = session->GetOutputCount();
    output_names.reserve(n_out);
    output_name_cstrs.reserve(n_out);
    for (size_t i = 0; i < n_out; ++i) {
        Ort::AllocatedStringPtr name =
            session->GetOutputNameAllocated(i, allocator);
        output_names.emplace_back(name.get());
    }
    for (const auto& s : output_names) {
        output_name_cstrs.push_back(s.c_str());
    }

    // Sanity check: LaMa is image-in, mask-in, image-out. Some custom
    // exports add diagnostics; we tolerate extras but require at least
    // 2 inputs and 1 output.
    if (input_names.size() < 2 || output_names.empty()) {
        std::ostringstream msg;
        msg << "LamaSession: model has unexpected I/O shape ("
            << input_names.size() << " inputs, "
            << output_names.size() << " outputs); expected >=2 in, >=1 out";
        throw std::runtime_error(msg.str());
    }
}

void LamaSession::Impl::ensure_initialized()
{
    std::call_once(init_flag, [this] {
        try {
            initialize();
        } catch (...) {
            init_error = std::current_exception();
        }
    });
    if (init_error) {
        std::rethrow_exception(init_error);
    }
}

// ----------------------------------------------------------------------
// LamaSession public API
// ----------------------------------------------------------------------

LamaSession::LamaSession(std::string model_path, Backend backend)
    : impl_(std::make_unique<Impl>(std::move(model_path), backend))
{
}

LamaSession::~LamaSession() = default;

LamaSession::Backend LamaSession::active_backend() const
{
    return impl_->active_backend;
}

int LamaSession::input_count() const
{
    return static_cast<int>(impl_->input_names.size());
}

int LamaSession::output_count() const
{
    return static_cast<int>(impl_->output_names.size());
}

std::string LamaSession::input_name(int index) const
{
    if (index < 0 || index >= static_cast<int>(impl_->input_names.size())) {
        return std::string{};
    }
    return impl_->input_names[static_cast<size_t>(index)];
}

std::string LamaSession::output_name(int index) const
{
    if (index < 0 || index >= static_cast<int>(impl_->output_names.size())) {
        return std::string{};
    }
    return impl_->output_names[static_cast<size_t>(index)];
}

// ----------------------------------------------------------------------
// run() - the hot path. Transposes HWC->CHW, builds tensors, invokes
// Session::Run, transposes CHW->HWC into the caller's output buffer.
// ----------------------------------------------------------------------

void LamaSession::run(const float* rgb_in,
                      const float* mask,
                      float* rgb_out)
{
    if (!rgb_in || !mask || !rgb_out) {
        throw std::runtime_error("LamaSession::run: null pointer");
    }

    impl_->ensure_initialized();

    constexpr int N = kImageSize;
    constexpr int C = kChannels;

    // HWC interleaved [N*N*C] -> CHW planar [C*N*N]. The std::vector
    // allocates and frees per call; for a 512x512 RGB image that's
    // 3 MiB which is well under any reasonable per-call budget. If
    // we ever profile this as hot we can move to a per-thread scratch
    // buffer cached on the Impl.
    //
    // Per Carve's documentation (huggingface.co/Carve/LaMa-ONNX):
    //   - Input image: [0, 1] float, NO pre-masking (the network
    //     handles masking internally; pre-masking produces wrong
    //     output).
    //   - Output: [0, 255] float - we divide by 255 below to get
    //     back to [0, 1].
    std::vector<float> img_chw(static_cast<size_t>(C) * N * N);
    for (int c = 0; c < C; ++c) {
        float*       dst = img_chw.data() + static_cast<size_t>(c) * N * N;
        const float* src = rgb_in + c;
        for (int i = 0; i < N * N; ++i) {
            dst[i] = src[i * C];
        }
    }

    // Mask is single-channel so HWC == CHW for it; no transpose. We
    // still copy because Ort::Value::CreateTensor wants a non-const
    // pointer when the data is owned by the caller (CPU tensor mode).
    std::vector<float> mask_chw(static_cast<size_t>(N) * N);
    std::memcpy(mask_chw.data(), mask,
                static_cast<size_t>(N) * N * sizeof(float));

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    std::array<int64_t, 4> img_shape = {1, C, N, N};
    std::array<int64_t, 4> msk_shape = {1, 1, N, N};

    std::vector<Ort::Value> inputs;
    inputs.reserve(2);
    inputs.push_back(Ort::Value::CreateTensor<float>(
        mem, img_chw.data(), img_chw.size(),
        img_shape.data(), img_shape.size()));
    inputs.push_back(Ort::Value::CreateTensor<float>(
        mem, mask_chw.data(), mask_chw.size(),
        msk_shape.data(), msk_shape.size()));

    // Run. We use only the first 2 input names (image, mask) - some
    // exports include diagnostic inputs we don't supply, which ORT
    // tolerates as long as they have defaults.
    std::vector<Ort::Value> outputs;
    try {
        outputs = impl_->session->Run(
            Ort::RunOptions{nullptr},
            impl_->input_name_cstrs.data(),
            inputs.data(), inputs.size(),
            impl_->output_name_cstrs.data(),
            impl_->output_name_cstrs.size());
    }
    catch (const Ort::Exception& e) {
        throw std::runtime_error(
            std::string("LamaSession::run: inference failed: ") + e.what());
    }

    if (outputs.empty()) {
        throw std::runtime_error("LamaSession::run: no outputs returned");
    }

    // Output 0 is the inpainted RGB image as CHW float32. Transpose
    // back to HWC into the caller's buffer.
    const float* out_chw = outputs[0].GetTensorData<float>();

    // Defensive: confirm shape matches what we expect. If the model
    // returns a different size (some unofficial exports do), abort
    // before we overrun the caller's buffer.
    {
        Ort::TensorTypeAndShapeInfo info =
            outputs[0].GetTensorTypeAndShapeInfo();
        std::vector<int64_t> shape = info.GetShape();
        const bool ok =
            shape.size() == 4 &&
            shape[0] == 1 && shape[1] == C &&
            shape[2] == N && shape[3] == N;
        if (!ok) {
            std::ostringstream msg;
            msg << "LamaSession::run: unexpected output shape [";
            for (size_t i = 0; i < shape.size(); ++i) {
                if (i) msg << ",";
                msg << shape[i];
            }
            msg << "], expected [1," << C << "," << N << "," << N << "]";
            throw std::runtime_error(msg.str());
        }
    }

    // Carve ONNX outputs image * 255, so divide by 255 to get [0, 1].
    // See huggingface.co/Carve/LaMa-ONNX/discussions/2
    constexpr float kOutputScale = 1.0f / 255.0f;
    for (int c = 0; c < C; ++c) {
        const float* src = out_chw + static_cast<size_t>(c) * N * N;
        float*       dst = rgb_out + c;
        for (int i = 0; i < N * N; ++i) {
            dst[i * C] = src[i] * kOutputScale;
        }
    }
}

} // namespace nukeaifill
