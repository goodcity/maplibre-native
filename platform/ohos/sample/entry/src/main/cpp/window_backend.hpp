#pragma once

#include <mln/gfx/renderer_backend.hpp>
#include <mln/util/size.hpp>

#include <cstdint>
#include <string>
#include <stdexcept>

namespace mln {
namespace ohos {

class WindowBackend {
public:
    virtual ~WindowBackend() = default;

    virtual gfx::RendererBackend& getRendererBackend() = 0;
    virtual void setSize(Size) = 0;
    virtual std::uint64_t getLastSubmitDurationUs() const { return 0; }
    virtual void requestSnapshot() { throw std::runtime_error("Snapshot unavailable for this backend"); }
    virtual bool writeSnapshot(const std::string&) { return false; }

    virtual std::int32_t getGlesContextClientVersion() const { return 0; }
    virtual const std::string& getRendererDiagnostic() const { return emptyDiagnostic(); }

private:
    static const std::string& emptyDiagnostic() {
        static const std::string empty;
        return empty;
    }
};

} // namespace ohos
} // namespace mln
