#pragma once

#include "camera_bounds_options.hpp"
#include "renderer_frontend.hpp"
#include "window_backend.hpp"

#include <mln/map/bound_options.hpp>
#include <mln/map/map.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/util/size.hpp>

#include <native_window/external_window.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mln {
namespace ohos {

class MapView final : public MapObserver {
public:
    struct LoadingState {
        bool styleJsonLoaded = false;
        bool styleResourcesLoaded = false;
        bool spritesLoaded = false;
        std::uint64_t sourceCount = 0;
        std::uint64_t unloadedSourceCount = 0;
        std::uint64_t unloadedImageSourceCount = 0;
        std::uint64_t unloadedGeoJsonSourceCount = 0;
        std::uint64_t unloadedTileSourceCount = 0;
        bool hasRenderFrameStatus = false;
        bool lastFrameWasFull = false;
        bool lastFrameNeedsRepaint = false;
    };

    explicit MapView(float pixelRatio = 1.0f);
    ~MapView() override;

    MapView(const MapView&) = delete;
    MapView& operator=(const MapView&) = delete;

    void setSurface(OHNativeWindow*, Size);
    void clearSurface();
    bool renderFrame();
    std::string execute(const std::string&);
    void reduceMemoryUse();

    void setStyleURL(const std::string&);
    void setStyleJSON(const std::string&);
    void jumpTo(CameraOptions);
    void easeTo(CameraOptions, AnimationOptions);
    void flyTo(CameraOptions, AnimationOptions);
    void fitBounds(CameraBoundsOptions);
    void setFreeCameraOptions(FreeCameraOptions);
    void setGestureInProgress(bool);
    void moveBy(double x, double y, AnimationOptions = {});
    void pitchBy(double deltaPitch);
    void scaleBy(double scale, double anchorX, double anchorY);
    void flyBy(double scale, double anchorX, double anchorY, AnimationOptions = {});
    void rotateBy(double previousAngle, double currentAngle, double anchorX, double anchorY);
    void setDebugOptions(MapDebugOptions);
    void setBounds(BoundOptions);
    void setClientOptions(std::string name, std::string version);
    void setResourceOptions(ResourceOptions);
    void setTileCacheEnabled(bool);
    void setPixelRatio(float);
    void setSize(Size);

    bool hasMap() const { return map != nullptr; }
    OHNativeWindow* getNativeWindow() const { return window; }
    Size getSurfaceSize() const { return surfaceSize; }
    float getPixelRatio() const { return pixelRatio; }
    MapDebugOptions getDebugOptions() const { return debugOptions; }
    BoundOptions getBounds() const;
    CameraOptions getCameraOptions() const;
    FreeCameraOptions getFreeCameraOptions() const;
    CameraOptions cameraForBounds(const CameraBoundsOptions&) const;
    const std::string& getClientName() const { return clientName; }
    const std::string& getClientVersion() const { return clientVersion; }
    const ResourceOptions& getResourceOptions() const { return resourceOptions; }
    std::vector<std::string> getStyleAttributions() const;
    bool hasPendingRender() const;
    std::uint64_t getSubmittedRevision() const { return frontend ? frontend->getSubmittedRevision() : 0; }
    std::uint64_t getRenderedRevision() const { return frontend ? frontend->getRenderedRevision() : 0; }
    std::uint64_t getLastRenderDurationUs() const { return frontend ? frontend->getLastRenderDurationUs() : 0; }
    std::uint64_t getLastSubmitDurationUs() const { return backend ? backend->getLastSubmitDurationUs() : 0; }
    std::uint64_t getRenderedFrameCount() const { return renderedFrameCount; }
    bool hasLoadedStyle() const { return styleLoaded; }
    bool hasLoadedMap() const { return mapLoaded; }
    bool isFullyLoaded() const;
    LoadingState getLoadingState() const;
    std::int32_t getGlesContextClientVersion() const;
    const std::string& getRendererDiagnostic() const;
    const std::string& getLastMapLoadError() const { return lastMapLoadError; }
    const std::string& getLastRenderError() const { return lastRenderError; }
    const std::string& getLastStyleImageMissing() const { return lastStyleImageMissing; }
    const std::string& getLastGlyphsError() const { return lastGlyphsError; }
    const std::string& getLastSpritesError() const { return lastSpritesError; }

private:
    void createMap(OHNativeWindow*, Size);
    CameraOptions mergeCameraOptions(const CameraOptions&) const;
    FreeCameraOptions mergeFreeCameraOptions(const FreeCameraOptions&) const;
    BoundOptions mergeBoundOptions(const BoundOptions&) const;
    void applyDesiredBounds();
    void applyDesiredCamera();
    void runLoopOnce();

    void onDidFailLoadingMap(MapLoadError, const std::string&) override;
    void onDidFinishLoadingMap() override;
    void onDidFinishLoadingStyle() override;
    void onDidFinishRenderingFrame(const RenderFrameStatus&) override;
    void onStyleImageMissing(const std::string&) override;
    void onGlyphsError(const FontStack&, const GlyphRange&, std::exception_ptr) override;
    void onSpriteError(const std::optional<style::Sprite>&, std::exception_ptr) override;
    void onRenderError(std::exception_ptr) override;

    void resetRuntimeState();

    bool zoomGesturesEnabled = true;
    bool rotateGesturesEnabled = true;
    bool tiltGesturesEnabled = true;
    float pixelRatio = 1.0f;
    OHNativeWindow* window = nullptr;
    Size surfaceSize;
    std::unique_ptr<WindowBackend> backend;
    std::unique_ptr<RendererFrontend> frontend;
    std::unique_ptr<Map> map;
    std::optional<CameraOptions> desiredCamera;
    std::optional<CameraBoundsOptions> desiredCameraBounds;
    std::optional<FreeCameraOptions> desiredFreeCamera;
    std::optional<BoundOptions> desiredBounds;
    MapDebugOptions debugOptions = MapDebugOptions::NoDebug;
    bool tileCacheEnabled = true;
    bool styleLoaded = false;
    bool mapLoaded = false;
    bool hasRenderFrameStatus = false;
    bool lastFrameWasFull = false;
    bool lastFrameNeedsRepaint = false;
    std::uint64_t renderedFrameCount = 0;
    std::string lastMapLoadError;
    std::string lastRenderError;
    std::string lastStyleImageMissing;
    std::string lastGlyphsError;
    std::string lastSpritesError;
    std::string clientName;
    std::string clientVersion;
    ResourceOptions resourceOptions;
};

} // namespace ohos
} // namespace mln
