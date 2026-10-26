#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <time.h>

namespace fence_diagnostics {
// Values identify call boundaries only, never GPU completion or ownership.
enum class Phase : unsigned { ControllerCloseEntered = 1, MapStateClearReturned = 2, NodeDisposeReturned = 3 };
struct PhaseEvent {
    std::uint64_t sequence = 0, controllerId = 0, timeUs = 0;
    unsigned phase = 0;
    bool operationAttempted = false;
};
struct LifecycleSample {
    static constexpr unsigned capacity = 64;
    std::array<PhaseEvent, capacity> events{};
    unsigned count = 0;
    std::uint64_t epoch = 0, overwritten = 0, clockFailures = 0;
    bool recording = false;
};
class LifecycleRecorder {
public:
    std::uint64_t nextControllerId() {
        std::scoped_lock lock(mutex);
        return ++controllerSequence;
    }
    void setRecording(bool enabled) {
        std::scoped_lock lock(mutex);
        if (enabled && !sample.recording) {
            const auto epoch = sample.epoch + 1;
            sample = LifecycleSample{};
            sample.epoch = epoch;
        }
        sample.recording = enabled;
    }
    void appendNow(std::uint64_t controllerId, Phase phase, bool operationAttempted) {
        std::scoped_lock lock(mutex);
        if (!sample.recording) return;
        timespec timestamp{};
        const auto timeUs = clock_gettime(CLOCK_MONOTONIC, &timestamp) == 0
            ? std::uint64_t(timestamp.tv_sec) * 1000000ULL + timestamp.tv_nsec / 1000 : 0;
        appendLocked(controllerId, phase, operationAttempted, timeUs);
    }
    void append(std::uint64_t controllerId, Phase phase, bool operationAttempted,
                std::uint64_t timeUs) {
        std::scoped_lock lock(mutex);
        appendLocked(controllerId, phase, operationAttempted, timeUs);
    }
    LifecycleSample read() {
        std::scoped_lock lock(mutex);
        return sample; // Read does not drain or reset events.
    }
private:
    void appendLocked(std::uint64_t controllerId, Phase phase, bool operationAttempted,
                      std::uint64_t timeUs) {
        if (!sample.recording) return;
        if (!timeUs) ++sample.clockFailures;
        const PhaseEvent event{++eventSequence, controllerId, timeUs,
            static_cast<unsigned>(phase), operationAttempted};
        if (sample.count < LifecycleSample::capacity) {
            sample.events[sample.count++] = event;
        } else {
            // Bounded storage and explicit loss; no heap allocation or logging.
            for (unsigned i = 1; i < LifecycleSample::capacity; ++i)
                sample.events[i-1] = sample.events[i];
            sample.events.back() = event;
            ++sample.overwritten;
        }
    }
    std::mutex mutex;
    LifecycleSample sample;
    std::uint64_t controllerSequence = 0, eventSequence = 0;
};
inline LifecycleRecorder& lifecycleRecorder() {
    static LifecycleRecorder recorder;
    return recorder;
}
inline void recordPhase(std::uint64_t controllerId, Phase phase, bool attempted) {
    lifecycleRecorder().appendNow(controllerId, phase, attempted);
}
} // namespace fence_diagnostics
