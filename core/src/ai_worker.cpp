// nuke-ai-fill / core / ai_worker.cpp
//
// Worker thread implementation. See ai_worker.h for design notes.
//
// Per NDK_NOTES section 5.2, the work lambda runs inside a try/catch
// at the worker level - exceptions never escape into Nuke's call
// stack from this thread.

#include "ai_worker.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace nukeaifill {

// ----------------------------------------------------------------------
// AiWorkerContext - shared lock for status_text
// ----------------------------------------------------------------------

void AiWorkerContext::set_status_text(const std::string& s) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_text_ = s;
}

std::string AiWorkerContext::status_text() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_text_;
}

// ----------------------------------------------------------------------
// AiWorker
// ----------------------------------------------------------------------

AiWorker::AiWorker() = default;

AiWorker::~AiWorker() {
    // The destructor must not let the thread outlive this object,
    // or its lambda capture of cache paths / context references
    // would dangle. Request cancellation and join.
    //
    // If the work lambda doesn't check should_cancel() promptly,
    // this will block until inference finishes naturally. That is
    // the contract - we do not forcibly terminate threads.
    cancel();
    join();
}

bool AiWorker::start(Task task) {
    // Only valid transitions to Running are from Idle.
    AiWorkerState expected = AiWorkerState::Idle;
    if (!state_.compare_exchange_strong(expected, AiWorkerState::Running,
                                        std::memory_order_acq_rel)) {
        // Already running or in a terminal state awaiting reset.
        return false;
    }

    // Fresh run: clear any stale cancellation request and progress
    // state from a previous run. The AiWorkerContext object itself
    // stays stable across runs so its address can be safely cached.
    context_.clear_cancel_for_run();
    context_.set_progress(0.0f);
    context_.set_status_text("Starting");

    // Defensive: if a prior thread has finished but was never joined
    // (which would only happen if reset() was skipped), join now
    // before launching a new one.
    if (thread_.joinable()) {
        thread_.join();
    }

    thread_ = std::thread(&AiWorker::run, this, std::move(task));
    return true;
}

void AiWorker::cancel() {
    context_.request_cancel();
}

void AiWorker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void AiWorker::reset() {
    AiWorkerState s = state_.load(std::memory_order_acquire);
    if (s == AiWorkerState::Idle || s == AiWorkerState::Running) {
        // Cannot reset from Idle (no-op) or Running (wait first).
        return;
    }
    // Ensure thread has been joined before clearing state.
    if (thread_.joinable()) {
        thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_message_.clear();
    }
    context_.set_progress(0.0f);
    context_.set_status_text("");
    state_.store(AiWorkerState::Idle, std::memory_order_release);
}

std::string AiWorker::error_message() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_message_;
}

std::string AiWorker::status_text() const {
    return context_.status_text();
}

void AiWorker::run(Task task) {
    AiWorkerState final_state = AiWorkerState::Done;
    std::string err;

    try {
        task(context_);

        if (context_.should_cancel()) {
            final_state = AiWorkerState::Cancelled;
        }
    }
    catch (const std::exception& e) {
        final_state = AiWorkerState::Error;
        err = e.what();
    }
    catch (...) {
        final_state = AiWorkerState::Error;
        err = "Unknown exception in worker";
    }

    if (final_state == AiWorkerState::Error) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_message_ = std::move(err);
    }
    // Publish state change last so any thread observing a non-Running
    // state sees the corresponding error_message_ or completion.
    state_.store(final_state, std::memory_order_release);
}

} // namespace nukeaifill
