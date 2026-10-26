#pragma once

#include <mln/gfx/renderer_backend.hpp>
#include <mln/renderer/renderer_frontend.hpp>
#include <mln/util/feature.hpp>
#include <mln/util/geo.hpp>

#include <memory>
#include <optional>
#include <string>

namespace mln {

class Renderer;
class UpdateParameters;

namespace ohos {

class RendererFrontend final : public mln::RendererFrontend {
public:
    RendererFrontend(gfx::RendererBackend&,
                     float pixelRatio,
                     std::optional<std::string> localFontFamily = std::nullopt);
    ~RendererFrontend() override;

    void reset() override;
    void setObserver(RendererObserver&) override;
    void update(std::shared_ptr<UpdateParameters>) override;
    const TaggedScheduler& getThreadPool() const override;

    bool renderFrame();
    void markSubmission() { ++submittedRevision; }
    std::uint64_t getSubmittedRevision() const { return submittedRevision; }
    std::uint64_t getRenderedRevision() const { return renderedRevision; }
    std::uint64_t getLastRenderDurationUs() const { return lastRenderDurationUs; }
    std::vector<Feature> queryFeatures(const ScreenCoordinate&, const std::string&) const;
    const std::string& getLastError() const { return lastError; }
    bool hasPendingRender() const;
    void setTileCacheEnabled(bool);
    void reduceMemoryUse();

private:
    gfx::RendererBackend& backend;
    std::unique_ptr<Renderer> renderer;
    std::shared_ptr<UpdateParameters> updateParameters;
    bool needsRender = false;
    std::uint64_t submittedRevision = 0;
    std::uint64_t updateRevision = 0;
    std::uint64_t renderedRevision = 0;
    std::uint64_t lastRenderDurationUs = 0;
    std::string lastError;
};

} // namespace ohos
} // namespace mln
