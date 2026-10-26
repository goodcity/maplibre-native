#pragma once
#include <cstdint>

namespace fence_diagnostics {
enum class Kind { Other, SyncFile, LegacyFence, Closed, Error };
enum class Query { Ok, Unsupported, Denied, Error };
struct Probe {
    Query query = Query::Error;
    int status = 0;
    unsigned components = 0;
    unsigned childActive = 0, childSignaled = 0, childError = 0, childInvalid = 0;
    unsigned timestamps = 0;
    std::uint64_t firstTimestamp = 0, lastTimestamp = 0;
    bool detailsRead = false, detailsTooLarge = false;
};
// Only numeric data leaves the observer. Descriptor counts are NOT unique
// fence-object counts; multiple descriptors may refer to the same sync_file.
struct Sample {
    std::uint64_t beginNs = 0, endNs = 0;
    unsigned entries = 0, syncCandidates = 0, legacyCandidates = 0;
    unsigned confirmedSyncDescriptors = 0, closed = 0, linkErrors = 0;
    unsigned duplicateErrors = 0, changedAfterDuplicate = 0;
    unsigned queryOk = 0, unsupported = 0, denied = 0, queryErrors = 0;
    unsigned active = 0, signaled = 0, errorStatus = 0, invalidStatus = 0;
    unsigned componentTotal = 0, componentReads = 0, componentReadErrors = 0;
    unsigned componentTooLarge = 0, childActive = 0, childSignaled = 0, childError = 0, childInvalid = 0;
    unsigned timestamps = 0;
    std::uint64_t firstTimestamp = 0, lastTimestamp = 0;
    bool directoryAvailable = false, directoryComplete = false;
};
struct Operations {
    virtual ~Operations() = default;
    virtual std::uint64_t nowNs() = 0;
    virtual Kind kind(int descriptor) = 0;
    virtual int duplicate(int descriptor) = 0;
    virtual void closeDuplicate(int descriptor) = 0;
    virtual Probe query(int descriptor) = 0;
};
inline void observe(int descriptor, Operations& ops, Sample& out) {
    ++out.entries;
    const auto initial = ops.kind(descriptor);
    if (initial == Kind::Closed) { ++out.closed; return; }
    if (initial == Kind::Error) { ++out.linkErrors; return; }
    if (initial == Kind::LegacyFence) { ++out.legacyCandidates; return; }
    if (initial != Kind::SyncFile) return;
    ++out.syncCandidates;
    const int pinned = ops.duplicate(descriptor);
    if (pinned < 0) { ++out.duplicateErrors; return; }
    struct Guard {
        Operations& ops;
        int descriptor;
        ~Guard() { ops.closeDuplicate(descriptor); }
    } guard{ops, pinned};
    // Recheck the pinned open-file reference: the original descriptor may have
    // closed/reused since readlink. This still does NOT establish cross-sample
    // identity or detect replacement by a different sync_file before dup.
    if (ops.kind(pinned) != Kind::SyncFile) {
        ++out.changedAfterDuplicate;
        return;
    }
    ++out.confirmedSyncDescriptors;
    const auto probe = ops.query(pinned);
    switch (probe.query) {
        case Query::Unsupported: ++out.unsupported; return;
        case Query::Denied: ++out.denied; return;
        case Query::Error: ++out.queryErrors; return;
        case Query::Ok: ++out.queryOk; break;
    }
    if (probe.status == 0) ++out.active;
    else if (probe.status == 1) ++out.signaled;
    else if (probe.status < 0) ++out.errorStatus;
    else ++out.invalidStatus;
    out.componentTotal += probe.components;
    if (probe.detailsTooLarge) { ++out.componentTooLarge; return; }
    if (!probe.detailsRead) { ++out.componentReadErrors; return; }
    ++out.componentReads;
    out.childActive += probe.childActive;
    out.childSignaled += probe.childSignaled;
    out.childError += probe.childError;
    out.childInvalid += probe.childInvalid;
    if (probe.timestamps) {
        if (!out.timestamps || probe.firstTimestamp < out.firstTimestamp)
            out.firstTimestamp = probe.firstTimestamp;
        if (probe.lastTimestamp > out.lastTimestamp) out.lastTimestamp = probe.lastTimestamp;
        out.timestamps += probe.timestamps;
    }
}
} // namespace fence_diagnostics
