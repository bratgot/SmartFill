// nuke-ai-fill / core / ai_worker.h
//
// Async inference worker. Encapsulates a std::thread that runs ML
// inference off the main thread, satisfying NDK_NOTES section 5
// (UI-thread-only rules) and section 5.2 (boundary try/catch).
//
// Strict ASCII per NDK_NOTES section 6.1.
//
// Lifecycle:
//
//   1. Op owns an AiWorker by value (or unique_ptr if it needs to swap
//      session types).
//   2. Op::knob_changed for the "Bake" button:
//        - compute input hash
//        - check cache; if hit, set Op dirty and return
//        - else: worker.start(std::move(task));
//   3. Op::_validate (main thread, called by Nuke during cook prep):
//        - poll worker.state()
//        - if transitioned to Done since last validate: cache is now
//          populated, mark Op dirty so renderStripe re-runs
//        - if Error: emit Op::error() and clear
//   4. To get _validate called while the user is idle, menu.py
//      registers a Python idle timer that calls forceValidate on any
//      AIGenerate/AISmartFill node whose status knob says "Cooking".
//      The timer auto-deregisters when no nodes report Cooking state.
//
// The worker NEVER calls back into the Op. It writes to a cache file
// path that was determined before the worker started, and flips an
// atomic state flag. All Nuke API calls happen on the main thread.

#ifndef NUKE_AI_FILL_AI_WORKER_H
#define NUKE_AI_FILL_AI_WORKER_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace nukeaifill {

// ----------------------------------------------------------------------
// State machine
// ----------------------------------------------------------------------
//
//   Idle ----start()----> Running ---worker finishes---> Done
//                            |                            |
//                            +----exception caught-----> Error
//                            |
//                            +----cancel()------------> Cancelled
//
// Transitions out of Running are made by the worker thread.
// Transitions into Running and back to Idle are made by the main thread
// via start() and reset(). Cancellation is cooperative: cancel() sets a
// flag; the work lambda is expected to check should_cancel() and exit
// promptly. There is no forced thread kill.

enum class AiWorkerState : int {
    Idle      = 0,
    Running   = 1,
    Done      = 2,
    Error     = 3,
    Cancelled = 4
};

// ----------------------------------------------------------------------
// Progress + cancellation handle passed to the work lambda
// ----------------------------------------------------------------------
//
// The work lambda receives a reference to this. It MUST check
// should_cancel() periodically (between inference steps for SD, between
// tiles for LaMa) and exit early if true. It MAY call set_progress()
// to report 0.0 -> 1.0; the main thread polls progress() to drive a UI
// progress bar.

class AiWorkerContext {
public:
    AiWorkerContext() = default;

    void  set_progress(float p) noexcept;
    float progress() const noexcept;

    void request_cancel() noexcept;
    bool should_cancel() const noexcept;

    void set_status_text(const std::string& s);
    std::string status_text() const;

private:
    friend class AiWorker;

    // Called by AiWorker between runs to clear stale cancellation
    // state. Not exposed publicly because callers other than AiWorker
    // have no business clearing a cancel request mid-flight.
    void clear_cancel_for_run() noexcept {
        cancel_request_.store(false, std::memory_order_release);
    }

    std::atomic<float>  progress_      {0.0f};
    std::atomic<bool>   cancel_request_{false};

    // status_text_ needs a mutex - std::string is not lock-free.
    // Kept tiny: the main thread reads, the worker writes, both rarely.
    mutable std::mutex status_mutex_;
    std::string status_text_;
};

// ----------------------------------------------------------------------
// Worker
// ----------------------------------------------------------------------
//
// Move-only. Hold one of these by value inside the Op. Destruction
// cancels and joins the thread; do not let it outlive the Op.

class AiWorker {
public:
    // Lambda signature: takes a context, returns nothing. Throw on
    // failure - the worker wraps the body in try/catch and stores the
    // exception message for the main thread.
    using Task = std::function<void(AiWorkerContext&)>;

    AiWorker();
    ~AiWorker();

    AiWorker(const AiWorker&)            = delete;
    AiWorker& operator=(const AiWorker&) = delete;
    AiWorker(AiWorker&&)                 = delete;
    AiWorker& operator=(AiWorker&&)      = delete;

    // Launch task on a background thread. Returns false if a task is
    // already running (caller must wait or cancel first).
    // Called from main thread only.
    bool start(Task task);

    // Cooperative cancel. Returns immediately; the worker may take some
    // time to actually stop. Safe to call from main thread.
    void cancel();

    // Block until the worker exits (whether by completion, error, or
    // cancellation). Safe to call from main thread; do not call from
    // the worker itself.
    void join();

    // Reset state to Idle. Only valid when state is Done, Error, or
    // Cancelled. No-op if Running. Called from main thread after the
    // Op has consumed the result.
    void reset();

    // ----- Main-thread queries (lock-free) -----

    AiWorkerState state() const noexcept;

    // Last error message; only meaningful when state() == Error.
    std::string error_message() const;

    // Progress 0.0 -> 1.0 reported by the work lambda.
    float progress() const noexcept;

    // Status text reported by the work lambda. Free-form, e.g.
    // "Loading model", "Step 12/20", "Decoding VAE".
    std::string status_text() const;

private:
    void run(Task task);

    std::atomic<AiWorkerState> state_{AiWorkerState::Idle};

    // error_message_ is set by worker before transitioning to Error
    // state, read by main thread after seeing Error state. The state
    // transition provides the memory ordering; we additionally lock
    // the string itself because std::string is not lock-free.
    mutable std::mutex error_mutex_;
    std::string error_message_;

    AiWorkerContext context_;
    std::thread     thread_;
};

// ----------------------------------------------------------------------
// Inline / lock-free implementations
// ----------------------------------------------------------------------

inline void AiWorkerContext::set_progress(float p) noexcept {
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    progress_.store(p, std::memory_order_relaxed);
}

inline float AiWorkerContext::progress() const noexcept {
    return progress_.load(std::memory_order_relaxed);
}

inline void AiWorkerContext::request_cancel() noexcept {
    cancel_request_.store(true, std::memory_order_release);
}

inline bool AiWorkerContext::should_cancel() const noexcept {
    return cancel_request_.load(std::memory_order_acquire);
}

inline AiWorkerState AiWorker::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

inline float AiWorker::progress() const noexcept {
    return context_.progress();
}

} // namespace nukeaifill

#endif // NUKE_AI_FILL_AI_WORKER_H
