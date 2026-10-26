#include "fence_lifecycle_diagnostics.hpp"
#include <cassert>
#include <iostream>
#include <thread>
using namespace fence_diagnostics;
int main() {
    LifecycleRecorder recorder;
    const auto id = recorder.nextControllerId();
    assert(id == 1 && recorder.nextControllerId() == 2);
    recorder.append(id, Phase::ControllerCloseEntered, true, 5);
    assert(!recorder.read().recording && recorder.read().count == 0);
    recorder.setRecording(true);
    assert(recorder.read().epoch == 1);
    recorder.append(id, Phase::ControllerCloseEntered, true, 10);
    recorder.append(id, Phase::MapStateClearReturned, false, 20);
    recorder.append(id, Phase::NodeDisposeReturned, true, 30);
    auto sample = recorder.read();
    assert(sample.count == 3 && sample.events[0].phase == 1 && sample.events[2].phase == 3);
    assert(sample.events[1].controllerId == id && !sample.events[1].operationAttempted);
    assert(sample.events[2].sequence == 3 && sample.events[2].timeUs == 30);
    assert(recorder.read().count == 3); // Reads never consume evidence.
    recorder.setRecording(true);
    assert(recorder.read().count == 3 && recorder.read().epoch == 1);
    for (unsigned i = 0; i < 64; ++i) recorder.append(id, Phase::ControllerCloseEntered, true, 100+i);
    sample = recorder.read();
    assert(sample.count == 64 && sample.overwritten == 3);
    assert(sample.events[0].sequence == 4 && sample.events[63].sequence == 67);
    recorder.setRecording(false);
    recorder.append(id, Phase::NodeDisposeReturned, true, 300);
    assert(recorder.read().count == 64 && recorder.read().events[63].sequence == 67);
    recorder.setRecording(true);
    recorder.append(id, Phase::MapStateClearReturned, false, 0);
    sample = recorder.read();
    assert(sample.epoch == 2 && sample.count == 1 && sample.overwritten == 0);
    assert(sample.clockFailures == 1 && sample.events[0].sequence == 68);
    LifecycleRecorder concurrent;
    concurrent.setRecording(true);
    auto record = [&concurrent](std::uint64_t actor) {
        for (unsigned index = 0; index < 100; ++index)
            concurrent.appendNow(actor, Phase::ControllerCloseEntered, true);
    };
    std::thread first(record, 1), second(record, 2);
    first.join(); second.join();
    sample = concurrent.read();
    assert(sample.count == 64 && sample.overwritten == 136);
    assert(sample.events[0].sequence == 137 && sample.events[63].sequence == 200);
    for (unsigned index = 1; index < sample.count; ++index) {
        assert(sample.events[index].sequence == sample.events[index-1].sequence + 1);
        assert(sample.events[index].timeUs >= sample.events[index-1].timeUs);
    }
    std::cout << "PASS lifecycle defaults, boundaries, non-draining reads, ring loss, restart and clock failure\n";
}
