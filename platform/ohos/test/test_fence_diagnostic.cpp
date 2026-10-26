#include "fence_diagnostic_core.hpp"
#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace fence_diagnostics;
struct Fake final : Operations {
    Kind initial = Kind::SyncFile, pinned = Kind::SyncFile;
    Probe probe;
    int duplicateValue = 100, closes = 0, queries = 0;
    bool queryThrows = false;
    std::uint64_t nowNs() override { return 123; }
    Kind kind(int descriptor) override { return descriptor == 100 ? pinned : initial; }
    int duplicate(int) override { return duplicateValue; }
    void closeDuplicate(int descriptor) override { assert(descriptor == 100); ++closes; }
    Probe query(int descriptor) override {
        assert(descriptor == 100); ++queries;
        if (queryThrows) throw std::runtime_error("synthetic failure");
        return probe;
    }
};
int main() {
    unsigned cases = 0;
    for (const auto kind : {Kind::Other, Kind::LegacyFence, Kind::Closed, Kind::Error}) {
        Fake fake; fake.initial = kind; Sample out; observe(5, fake, out);
        assert(out.entries == 1 && out.confirmedSyncDescriptors == 0 && fake.closes == 0);
        assert(out.legacyCandidates == (kind == Kind::LegacyFence));
        assert(out.closed == (kind == Kind::Closed));
        assert(out.linkErrors == (kind == Kind::Error)); ++cases;
    }
    {
        Fake fake; fake.duplicateValue = -1; Sample out; observe(5, fake, out);
        assert(out.syncCandidates == 1 && out.duplicateErrors == 1 && fake.closes == 0); ++cases;
    }
    {
        Fake fake; fake.pinned = Kind::Other; Sample out; observe(5, fake, out);
        assert(out.changedAfterDuplicate == 1 && fake.closes == 1 && fake.queries == 0); ++cases;
    }
    for (const auto query : {Query::Unsupported, Query::Denied, Query::Error}) {
        Fake fake; fake.probe.query = query; Sample out; observe(5, fake, out);
        assert(out.confirmedSyncDescriptors == 1 && out.queryOk == 0 && fake.closes == 1);
        assert(out.unsupported == (query == Query::Unsupported));
        assert(out.denied == (query == Query::Denied));
        assert(out.queryErrors == (query == Query::Error)); ++cases;
    }
    for (const int status : {0, 1, -5, 2}) {
        Fake fake; fake.probe.query = Query::Ok; fake.probe.status = status;
        fake.probe.detailsRead = true; Sample out; observe(5, fake, out);
        assert(out.queryOk == 1 && fake.closes == 1);
        assert(out.active == (status == 0) && out.signaled == (status == 1));
        assert(out.errorStatus == (status < 0) && out.invalidStatus == (status > 1)); ++cases;
    }
    {
        Fake fake; fake.probe.query = Query::Ok; fake.probe.components = 65;
        fake.probe.detailsTooLarge = true; Sample out; observe(5, fake, out);
        assert(out.componentTooLarge == 1 && !out.componentReads && fake.closes == 1); ++cases;
    }
    {
        Fake fake; fake.probe.query = Query::Ok; fake.probe.components = 2;
        Sample out; observe(5, fake, out);
        assert(out.componentReadErrors == 1 && fake.closes == 1); ++cases;
    }
    {
        Fake fake; fake.probe.query = Query::Ok; fake.probe.components = 2;
        fake.probe.detailsRead = true; fake.probe.childSignaled = 2;
        fake.probe.timestamps = 2; fake.probe.firstTimestamp = 10; fake.probe.lastTimestamp = 20;
        Sample out; observe(5, fake, out); observe(6, fake, out);
        assert(out.confirmedSyncDescriptors == 2 && out.componentTotal == 4 && fake.closes == 2);
        assert(out.childSignaled == 4 && out.timestamps == 4 && out.firstTimestamp == 10 && out.lastTimestamp == 20);
        // These are descriptor observations, never a claim of 2 unique objects.
        ++cases;
    }
    {
        Fake fake; fake.queryThrows = true; Sample out;
        try { observe(5, fake, out); assert(false); } catch (const std::runtime_error&) {}
        assert(fake.closes == 1); ++cases;
    }
    {
        Fake fake; fake.probe.query = Query::Ok; fake.probe.detailsRead = true;
        fake.probe.childActive = 1; fake.probe.childError = 2; fake.probe.childInvalid = 3;
        Sample out; observe(5, fake, out);
        assert(out.childActive == 1 && out.childError == 2 && out.childInvalid == 3); ++cases;
    }
    std::cout << "PASS " << cases << " deterministic fence diagnostic cases\n";
}
