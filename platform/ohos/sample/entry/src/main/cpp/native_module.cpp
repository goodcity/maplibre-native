#include "map_view.hpp"
#include "http_request_diagnostics.hpp"
#include "fence_diagnostic_linux.hpp"
#include "fence_lifecycle_diagnostics.hpp"

#include "gesture_handler.hpp"
#include "native_values.hpp"

#include <mln/storage/resource_options.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/size.hpp>
#include <mln/util/native_resource_diagnostics.hpp>

#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <arkui/native_interface.h>
#include <arkui/native_node.h>
#include <arkui/native_node_napi.h>
#include <arkui/ui_input_event.h>
#include <napi/native_api.h>
#include <native_window/external_window.h>

namespace {

using mln::ohos::createCameraOptionsObject;
using mln::ohos::createStringValue;
using mln::ohos::getBool;
using mln::ohos::getBoundOptions;
using mln::ohos::getCameraOptionsObject;
using mln::ohos::getDouble;
using mln::ohos::getOptionalStringProperty;
using mln::ohos::getRequiredInt32Property;
using mln::ohos::getString;
using mln::ohos::isNullOrUndefined;
using mln::ohos::isObject;

class SurfaceController;

std::unordered_map<ArkUI_NodeHandle, std::weak_ptr<SurfaceController>>& controllersByNode() {
    static std::unordered_map<ArkUI_NodeHandle, std::weak_ptr<SurfaceController>> controllers;
    return controllers;
}

std::unordered_map<OH_ArkUI_SurfaceHolder*, std::weak_ptr<SurfaceController>>& controllersByHolder() {
    static std::unordered_map<OH_ArkUI_SurfaceHolder*, std::weak_ptr<SurfaceController>> controllers;
    return controllers;
}

std::mutex& controllerRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

void registerController(ArkUI_NodeHandle node,
                        OH_ArkUI_SurfaceHolder* holder,
                        const std::shared_ptr<SurfaceController>& controller) {
    std::scoped_lock lock(controllerRegistryMutex());
    controllersByNode()[node] = controller;
    controllersByHolder()[holder] = controller;
}

void unregisterController(ArkUI_NodeHandle node, OH_ArkUI_SurfaceHolder* holder, const SurfaceController* controller) {
    std::scoped_lock lock(controllerRegistryMutex());

    if (auto it = controllersByNode().find(node); it != controllersByNode().end()) {
        const auto existing = it->second.lock();
        if (!existing || existing.get() == controller) {
            controllersByNode().erase(it);
        }
    }

    if (auto it = controllersByHolder().find(holder); it != controllersByHolder().end()) {
        const auto existing = it->second.lock();
        if (!existing || existing.get() == controller) {
            controllersByHolder().erase(it);
        }
    }
}

std::shared_ptr<SurfaceController> findController(ArkUI_NodeHandle node) {
    std::scoped_lock lock(controllerRegistryMutex());
    const auto it = controllersByNode().find(node);
    if (it == controllersByNode().end()) {
        return nullptr;
    }
    auto controller = it->second.lock();
    if (!controller) {
        controllersByNode().erase(it);
    }
    return controller;
}

std::shared_ptr<SurfaceController> findController(OH_ArkUI_SurfaceHolder* holder) {
    std::scoped_lock lock(controllerRegistryMutex());
    const auto it = controllersByHolder().find(holder);
    if (it == controllersByHolder().end()) {
        return nullptr;
    }
    auto controller = it->second.lock();
    if (!controller) {
        controllersByHolder().erase(it);
    }
    return controller;
}

napi_value getUndefined(napi_env env) {
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    return result;
}

napi_value throwError(napi_env env, const char* message) {
    napi_throw_error(env, nullptr, message);
    return getUndefined(env);
}

ArkUI_NativeNodeAPI_1* nativeNodeApi() {
    static auto* api = reinterpret_cast<ArkUI_NativeNodeAPI_1*>(
        OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1"));
    return api;
}

ArkUI_NativeNodeAPI_1& requireNativeNodeApi() {
    auto* api = nativeNodeApi();
    if (api == nullptr) {
        throw std::runtime_error("Could not load ArkUI_NativeNodeAPI_1");
    }
    return *api;
}

std::uint32_t toSizeDimension(std::uint64_t value) {
    constexpr auto maxDimension = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    if (value > maxDimension) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(value);
}

mln::Size toSize(std::uint64_t width, std::uint64_t height) {
    return {toSizeDimension(width), toSizeDimension(height)};
}

bool exposedResourceOptionsEqual(const mln::ResourceOptions& lhs, const mln::ResourceOptions& rhs) {
    return lhs.apiKey() == rhs.apiKey() && lhs.cachePath() == rhs.cachePath() && lhs.assetPath() == rhs.assetPath();
}

bool isValidFrameRateRange(const OH_NativeXComponent_ExpectedRateRange& range) {
    return range.min > 0 && range.max >= range.min && range.expected >= range.min && range.expected <= range.max;
}

bool parseFrameRateRange(napi_env env, napi_value value, OH_NativeXComponent_ExpectedRateRange& range) {
    return isObject(env, value) && getRequiredInt32Property(env, value, "min", range.min) &&
           getRequiredInt32Property(env, value, "max", range.max) &&
           getRequiredInt32Property(env, value, "expected", range.expected) && isValidFrameRateRange(range);
}

void setFloatAttribute(ArkUI_NodeHandle node,
                       ArkUI_NodeAttributeType attribute,
                       float value,
                       const char* errorMessage) {
    ArkUI_NumberValue numberValue{};
    numberValue.f32 = value;

    ArkUI_AttributeItem item{};
    item.value = &numberValue;
    item.size = 1;

    if (requireNativeNodeApi().setAttribute(node, attribute, &item) != ARKUI_ERROR_CODE_NO_ERROR) {
        throw std::runtime_error(errorMessage);
    }
}

ArkUI_NodeHandle createNativeXComponentNode() {
    auto& api = requireNativeNodeApi();
    ArkUI_NodeHandle node = api.createNode(ARKUI_NODE_XCOMPONENT);
    if (node == nullptr) {
        throw std::runtime_error("Could not create native XComponent node");
    }

    try {
        setFloatAttribute(node, NODE_WIDTH_PERCENT, 1.0f, "Could not set native XComponent width");
        setFloatAttribute(node, NODE_HEIGHT_PERCENT, 1.0f, "Could not set native XComponent height");
    } catch (...) {
        api.disposeNode(node);
        throw;
    }

    return node;
}

napi_value createStringArray(napi_env env, const std::vector<std::string>& values) {
    napi_value array = nullptr;
    napi_create_array_with_length(env, values.size(), &array);
    for (std::size_t i = 0; i < values.size(); ++i) {
        napi_set_element(env, array, static_cast<std::uint32_t>(i), createStringValue(env, values[i]));
    }
    return array;
}

void setBoolProperty(napi_env env, napi_value object, const char* name, bool value) {
    napi_value property = nullptr;
    napi_get_boolean(env, value, &property);
    napi_set_named_property(env, object, name, property);
}

void setSizeProperty(napi_env env, napi_value object, const char* name, std::uint64_t value) {
    napi_value property = nullptr;
    napi_create_double(env, static_cast<double>(value), &property);
    napi_set_named_property(env, object, name, property);
}

void setDoubleProperty(napi_env env, napi_value object, const char* name, double value) {
    napi_value property = nullptr;
    napi_create_double(env, value, &property);
    napi_set_named_property(env, object, name, property);
}

void setStringProperty(napi_env env, napi_value object, const char* name, const std::string& value) {
    napi_set_named_property(env, object, name, createStringValue(env, value));
}

mln::ohos::TouchAction toTouchAction(int32_t action) {
    switch (action) {
        case UI_TOUCH_EVENT_ACTION_DOWN:
            return mln::ohos::TouchAction::Down;
        case UI_TOUCH_EVENT_ACTION_MOVE:
            return mln::ohos::TouchAction::Move;
        case UI_TOUCH_EVENT_ACTION_UP:
            return mln::ohos::TouchAction::Up;
        case UI_TOUCH_EVENT_ACTION_CANCEL:
            return mln::ohos::TouchAction::Cancel;
        default:
            return mln::ohos::TouchAction::Unknown;
    }
}

class SurfaceController final : public std::enable_shared_from_this<SurfaceController> {
public:
    static std::shared_ptr<SurfaceController> create(ArkUI_NodeHandle node_) {
        auto controller = std::shared_ptr<SurfaceController>(new SurfaceController(node_, nullptr, false));
        try {
            controller->initialize();
        } catch (...) {
            controller->close();
            throw;
        }
        return controller;
    }

    static std::shared_ptr<SurfaceController> create(ArkUI_NodeContentHandle content_) {
        if (content_ == nullptr) {
            throw std::invalid_argument("Expected a NodeContent host");
        }

        auto controller = std::shared_ptr<SurfaceController>(
            new SurfaceController(createNativeXComponentNode(), content_, true));
        try {
            controller->initialize();
            controller->addNodeToContent();
        } catch (...) {
            controller->close();
            throw;
        }
        return controller;
    }

    ~SurfaceController() { close(); }

    SurfaceController(const SurfaceController&) = delete;
    SurfaceController& operator=(const SurfaceController&) = delete;

    bool isClosed() const { return closed; }

    void close() {
        if (closed) {
            return;
        }

        closed = true;
        fence_diagnostics::recordPhase(fenceDiagnosticControllerId, fence_diagnostics::Phase::ControllerCloseEntered, true);

        if (frameCallbackRegistered && node != nullptr &&
            OH_ArkUI_XComponent_UnregisterOnFrameCallback(node) != ARKUI_ERROR_CODE_NO_ERROR) {
            mln::Log::Warning(mln::Event::Setup, "Could not unregister XComponent frame callback");
        }
        frameCallbackRegistered = false;

        if (auto* api = nativeNodeApi()) {
            if (touchEventRegistered && node != nullptr) {
                api->unregisterNodeEvent(node, NODE_TOUCH_EVENT);
            }
            touchEventRegistered = false;

            if (nodeEventReceiverRegistered && node != nullptr) {
                if (api->removeNodeEventReceiver(node, onNodeEvent) != ARKUI_ERROR_CODE_NO_ERROR) {
                    mln::Log::Warning(mln::Event::Setup, "Could not remove XComponent touch event receiver");
                }
            }
            nodeEventReceiverRegistered = false;
        }

        if (surfaceCallbackRegistered && holder != nullptr && surfaceCallback != nullptr &&
            OH_ArkUI_SurfaceHolder_RemoveSurfaceCallback(holder, surfaceCallback) != ARKUI_ERROR_CODE_NO_ERROR) {
            mln::Log::Warning(mln::Event::Setup, "Could not remove XComponent surface callback");
        }
        surfaceCallbackRegistered = false;

        unregisterController(node, holder, this);
        const bool hadMapView = mapView != nullptr;
        clearMapState();
        fence_diagnostics::recordPhase(fenceDiagnosticControllerId, fence_diagnostics::Phase::MapStateClearReturned, hadMapView);

        if (nodeAddedToContent && content != nullptr && node != nullptr &&
            OH_ArkUI_NodeContent_RemoveNode(content, node) != ARKUI_ERROR_CODE_NO_ERROR) {
            mln::Log::Warning(mln::Event::Setup, "Could not remove native XComponent node from NodeContent");
        }
        nodeAddedToContent = false;
        content = nullptr;

        if (surfaceCallback != nullptr) {
            OH_ArkUI_SurfaceCallback_Dispose(surfaceCallback);
            surfaceCallback = nullptr;
        }

        if (holder != nullptr) {
            OH_ArkUI_SurfaceHolder_Dispose(holder);
            holder = nullptr;
        }

        bool nodeDisposeAttempted = false;
        if (ownsNode && node != nullptr) {
            if (auto* api = nativeNodeApi()) {
                nodeDisposeAttempted = true;
                api->disposeNode(node);
            }
        }
        fence_diagnostics::recordPhase(fenceDiagnosticControllerId, fence_diagnostics::Phase::NodeDisposeReturned, nodeDisposeAttempted);

        node = nullptr;
    }

    void onSurfaceCreated(OH_ArkUI_SurfaceHolder* eventHolder) {
        if (closed || eventHolder != holder) {
            return;
        }

        window = OH_ArkUI_XComponent_GetNativeWindow(holder);
        surfaceVisible = window != nullptr;
        if (window == nullptr) {
            lastSurfaceError = "XComponent surface did not provide a native window";
            return;
        }

        updateSurface();
    }

    void onSurfaceChanged(OH_ArkUI_SurfaceHolder* eventHolder, std::uint64_t width_, std::uint64_t height_) {
        if (closed || eventHolder != holder) {
            return;
        }

        window = OH_ArkUI_XComponent_GetNativeWindow(holder);
        width = width_;
        height = height_;
        surfaceVisible = window != nullptr;
        updateSurface();
    }

    void onSurfaceDestroyed(OH_ArkUI_SurfaceHolder* eventHolder) {
        if (closed || eventHolder != holder) {
            return;
        }

        surfaceVisible = false;
        clearSurface();
        window = nullptr;
        width = 0;
        height = 0;
    }

    void onFrame() {
        if (closed) {
            return;
        }

        ++frameCallbackCount;
        renderFrame();
    }

    void handleNodeEvent(ArkUI_NodeEvent* event) {
        if (closed || event == nullptr || OH_ArkUI_NodeEvent_GetEventType(event) != NODE_TOUCH_EVENT) {
            return;
        }

        auto* inputEvent = OH_ArkUI_NodeEvent_GetInputEvent(event);
        if (inputEvent == nullptr || OH_ArkUI_UIInputEvent_GetType(inputEvent) != ARKUI_UIINPUTEVENT_TYPE_TOUCH) {
            return;
        }

        auto touchEvent = createTouchEvent(inputEvent);
        if (mln::ohos::handleTouchEvent(gesture, mapView.get(), touchEvent)) {
            renderFrame();
        }
    }

    void renderFrame() {
        if (!closed && renderingEnabled && surfaceVisible && mapView) {
            mapView->renderFrame();
        }
    }

    std::string execute(const std::string& input) {
        if (closed) throw std::runtime_error("Map is disposed");
        return ensureMapView().execute(input);
    }

    void reduceMemoryUse() {
        if (!closed && mapView) {
            mapView->reduceMemoryUse();
        }
    }

    void setStyleUrl(std::string style_) {
        if (closed) {
            return;
        }

        style = std::move(style_);
        ++styleGeneration;
        applyDesiredStyle();
    }

    void jumpTo(mln::CameraOptions cameraOptions) {
        if (closed) {
            return;
        }

        ensureMapView().jumpTo(std::move(cameraOptions));
        renderFrame();
    }

    void setPixelRatio(float newPixelRatio) {
        if (closed) {
            return;
        }

        if (!mapView) {
            if (pixelRatio == newPixelRatio) {
                return;
            }
            mapView = std::make_unique<mln::ohos::MapView>(newPixelRatio);
        } else if (mapView->getPixelRatio() == newPixelRatio) {
            return;
        } else {
            mapView->setPixelRatio(newPixelRatio);
        }

        pixelRatio = newPixelRatio;
        appliedStyleGeneration = 0;
        updateSurface();
    }

    void setBounds(mln::BoundOptions boundOptions) {
        if (closed) {
            return;
        }

        ensureMapView().setBounds(std::move(boundOptions));
        renderFrame();
    }

    void setRenderingEnabled(bool enabled) {
        if (closed) {
            return;
        }

        renderingEnabled = enabled;
        if (enabled) {
            renderFrame();
        }
    }

    bool setFrameRateRange(const OH_NativeXComponent_ExpectedRateRange& range) {
        if (closed || node == nullptr) {
            return false;
        }

        if (OH_ArkUI_XComponent_SetExpectedFrameRateRange(node, range) != ARKUI_ERROR_CODE_NO_ERROR) {
            return false;
        }

        frameRateRange = range;
        return true;
    }

    void setTileCacheEnabled(bool enabled) {
        if (closed) {
            return;
        }

        ensureMapView().setTileCacheEnabled(enabled);
    }

    void setClientOptions(std::string clientName, std::string clientVersion) {
        if (closed) {
            return;
        }

        if (!mapView && clientName.empty() && clientVersion.empty()) {
            return;
        }
        if (mapView && mapView->getClientName() == clientName && mapView->getClientVersion() == clientVersion) {
            return;
        }

        ensureMapView().setClientOptions(std::move(clientName), std::move(clientVersion));
        appliedStyleGeneration = 0;
        applyDesiredStyle();
        renderFrame();
    }

    void setResourceOptions(const std::optional<std::string>& apiKey,
                            const std::optional<std::string>& cachePath,
                            const std::optional<std::string>& assetPath) {
        if (closed) {
            return;
        }
        if (!mapView && !apiKey && !cachePath && !assetPath) {
            return;
        }

        auto& currentMapView = ensureMapView();
        auto resourceOptions = currentMapView.getResourceOptions().clone();
        if (apiKey) {
            resourceOptions.withApiKey(*apiKey);
        }
        if (cachePath) {
            resourceOptions.withCachePath(*cachePath);
        }
        if (assetPath) {
            resourceOptions.withAssetPath(*assetPath);
        }

        if (exposedResourceOptionsEqual(resourceOptions, currentMapView.getResourceOptions())) {
            return;
        }

        currentMapView.setResourceOptions(std::move(resourceOptions));
        appliedStyleGeneration = 0;
        applyDesiredStyle();
        renderFrame();
    }

    float getPixelRatio() const { return mapView ? mapView->getPixelRatio() : pixelRatio; }

    std::vector<std::string> getStyleAttributions() {
        if (closed) {
            return {};
        }

        return ensureMapView().getStyleAttributions();
    }

    mln::CameraOptions getCameraOptions() {
        if (closed) {
            return {};
        }

        return ensureMapView().getCameraOptions();
    }

    napi_value createSurfaceStateObject(napi_env env) {
        updateFrameRates();

        const bool hasWindow = window != nullptr;
        const bool hasSurface = hasWindow && width > 0 && height > 0;
        const bool hasMap = mapView && mapView->hasMap();
        const bool needsRender = mapView && mapView->hasPendingRender();
        const bool styleLoaded = mapView && mapView->hasLoadedStyle();
        const bool mapLoaded = mapView && mapView->hasLoadedMap();
        const bool fullyLoaded = mapView && mapView->isFullyLoaded();
        const auto loadingState = mapView ? mapView->getLoadingState() : mln::ohos::MapView::LoadingState{};
        std::string backendLabel;
        if (mapView && mapView->getGlesContextClientVersion() > 0) {
            backendLabel = std::string{"OpenGL ES "} + std::to_string(mapView->getGlesContextClientVersion());
        } else if (mapView && !mapView->getRendererDiagnostic().empty()) {
            backendLabel = "Vulkan";
        }

        napi_value object = nullptr;
        napi_create_object(env, &object);
        setSizeProperty(env, object, "submittedRevision", mapView ? mapView->getSubmittedRevision() : 0);
        setSizeProperty(env, object, "renderedRevision", mapView ? mapView->getRenderedRevision() : 0);
        setSizeProperty(env, object, "lastRenderDurationUs", mapView ? mapView->getLastRenderDurationUs() : 0);
        setSizeProperty(env, object, "lastSubmitDurationUs", mapView ? mapView->getLastSubmitDurationUs() : 0);
        setSizeProperty(env, object, "renderedFrames", mapView ? mapView->getRenderedFrameCount() : 0);
        setSizeProperty(env, object, "width", width);
        setSizeProperty(env, object, "height", height);
        setBoolProperty(env, object, "hasWindow", hasWindow);
        setBoolProperty(env, object, "hasSurface", hasSurface);
        setBoolProperty(env, object, "hasMap", hasMap);
        setBoolProperty(env, object, "needsRender", needsRender);
        setBoolProperty(env, object, "styleLoaded", styleLoaded);
        setBoolProperty(env, object, "mapLoaded", mapLoaded);
        setBoolProperty(env, object, "fullyLoaded", fullyLoaded);
        setBoolProperty(env, object, "styleJsonLoaded", loadingState.styleJsonLoaded);
        setBoolProperty(env, object, "styleResourcesLoaded", loadingState.styleResourcesLoaded);
        setBoolProperty(env, object, "spritesLoaded", loadingState.spritesLoaded);
        setSizeProperty(env, object, "sourceCount", loadingState.sourceCount);
        setSizeProperty(env, object, "unloadedSourceCount", loadingState.unloadedSourceCount);
        setSizeProperty(env, object, "unloadedImageSourceCount", loadingState.unloadedImageSourceCount);
        setSizeProperty(env, object, "unloadedGeoJsonSourceCount", loadingState.unloadedGeoJsonSourceCount);
        setSizeProperty(env, object, "unloadedTileSourceCount", loadingState.unloadedTileSourceCount);
        setBoolProperty(env, object, "hasRenderFrameStatus", loadingState.hasRenderFrameStatus);
        setBoolProperty(env, object, "lastFrameWasFull", loadingState.lastFrameWasFull);
        setBoolProperty(env, object, "lastFrameNeedsRepaint", loadingState.lastFrameNeedsRepaint);
        setDoubleProperty(env, object, "renderedFrameRate", renderedFrameRate);
        setDoubleProperty(env, object, "frameCallbackRate", frameCallbackRate);
        if (!backendLabel.empty()) {
            setStringProperty(env, object, "backend", backendLabel);
        }
        setBoolProperty(env, object, "surfaceVisible", surfaceVisible);
        if (!lastSurfaceError.empty()) {
            setStringProperty(env, object, "lastSurfaceError", lastSurfaceError);
        }
        if (mapView && !mapView->getLastMapLoadError().empty()) {
            setStringProperty(env, object, "lastMapLoadError", mapView->getLastMapLoadError());
        }
        if (mapView && !mapView->getLastRenderError().empty()) {
            setStringProperty(env, object, "lastRenderError", mapView->getLastRenderError());
        }
        if (mapView && !mapView->getLastStyleImageMissing().empty()) {
            setStringProperty(env, object, "lastStyleImageMissing", mapView->getLastStyleImageMissing());
        }
        if (mapView && !mapView->getLastGlyphsError().empty()) {
            setStringProperty(env, object, "lastGlyphsError", mapView->getLastGlyphsError());
        }
        if (mapView && !mapView->getLastSpritesError().empty()) {
            setStringProperty(env, object, "lastSpritesError", mapView->getLastSpritesError());
        }
        return object;
    }

private:
    SurfaceController(ArkUI_NodeHandle node_, ArkUI_NodeContentHandle content_, bool ownsNode_)
        : node(node_),
          content(content_),
          ownsNode(ownsNode_) {}

    void initialize() {
        if (node == nullptr) {
            throw std::invalid_argument("Expected an XComponent node");
        }

        holder = OH_ArkUI_SurfaceHolder_Create(node);
        if (holder == nullptr) {
            throw std::runtime_error("Could not create XComponent surface holder");
        }

        surfaceCallback = OH_ArkUI_SurfaceCallback_Create();
        if (surfaceCallback == nullptr) {
            throw std::runtime_error("Could not create XComponent surface callback");
        }

        OH_ArkUI_SurfaceCallback_SetSurfaceCreatedEvent(surfaceCallback, handleSurfaceCreated);
        OH_ArkUI_SurfaceCallback_SetSurfaceChangedEvent(surfaceCallback, handleSurfaceChanged);
        OH_ArkUI_SurfaceCallback_SetSurfaceDestroyedEvent(surfaceCallback, handleSurfaceDestroyed);

        registerController(node, holder, shared_from_this());

        if (OH_ArkUI_SurfaceHolder_AddSurfaceCallback(holder, surfaceCallback) != ARKUI_ERROR_CODE_NO_ERROR) {
            throw std::runtime_error("Could not add XComponent surface callback");
        }
        surfaceCallbackRegistered = true;

        if (OH_ArkUI_XComponent_RegisterOnFrameCallback(node, onFrame) != ARKUI_ERROR_CODE_NO_ERROR) {
            throw std::runtime_error("Could not register XComponent frame callback");
        }
        frameCallbackRegistered = true;

        auto& api = requireNativeNodeApi();
        if (api.addNodeEventReceiver(node, onNodeEvent) != ARKUI_ERROR_CODE_NO_ERROR) {
            throw std::runtime_error("Could not add XComponent touch event receiver");
        }
        nodeEventReceiverRegistered = true;

        if (api.registerNodeEvent(node, NODE_TOUCH_EVENT, 0, nullptr) != ARKUI_ERROR_CODE_NO_ERROR) {
            throw std::runtime_error("Could not register XComponent touch event");
        }
        touchEventRegistered = true;
    }

    void addNodeToContent() {
        if (content == nullptr || node == nullptr) {
            throw std::invalid_argument("Expected a NodeContent host");
        }
        if (OH_ArkUI_NodeContent_AddNode(content, node) != ARKUI_ERROR_CODE_NO_ERROR) {
            throw std::runtime_error("Could not add native XComponent node to NodeContent");
        }
        nodeAddedToContent = true;
    }

    static void handleSurfaceCreated(OH_ArkUI_SurfaceHolder* holder) {
        if (auto controller = findController(holder)) {
            controller->onSurfaceCreated(holder);
        }
    }

    static void handleSurfaceChanged(OH_ArkUI_SurfaceHolder* holder, std::uint64_t width, std::uint64_t height) {
        if (auto controller = findController(holder)) {
            controller->onSurfaceChanged(holder, width, height);
        }
    }

    static void handleSurfaceDestroyed(OH_ArkUI_SurfaceHolder* holder) {
        if (auto controller = findController(holder)) {
            controller->onSurfaceDestroyed(holder);
        }
    }

    static void onFrame(ArkUI_NodeHandle node, std::uint64_t, std::uint64_t) {
        if (auto controller = findController(node)) {
            controller->onFrame();
        }
    }

    static void onNodeEvent(ArkUI_NodeEvent* event) {
        if (event == nullptr) {
            return;
        }
        if (auto controller = findController(OH_ArkUI_NodeEvent_GetNodeHandle(event))) {
            controller->handleNodeEvent(event);
        }
    }

    float coordinateScale() const { return std::isfinite(pixelRatio) && pixelRatio > 0.0f ? pixelRatio : 1.0f; }

    mln::ohos::TouchPoint touchPointAt(const ArkUI_UIInputEvent* event, std::uint32_t pointerIndex) const {
        const auto scale = coordinateScale();
        return {
            OH_ArkUI_PointerEvent_GetPointerId(event, pointerIndex),
            OH_ArkUI_PointerEvent_GetXByIndex(event, pointerIndex) * scale,
            OH_ArkUI_PointerEvent_GetYByIndex(event, pointerIndex) * scale,
        };
    }

    mln::ohos::TouchEvent createTouchEvent(const ArkUI_UIInputEvent* event) const {
        mln::ohos::TouchEvent touchEvent;
        touchEvent.action = toTouchAction(OH_ArkUI_UIInputEvent_GetAction(event));

        const auto pointerCount = OH_ArkUI_PointerEvent_GetPointerCount(event);
        touchEvent.points.reserve(pointerCount);
        for (std::uint32_t i = 0; i < pointerCount; ++i) {
            touchEvent.points.push_back(touchPointAt(event, i));
        }

        std::uint32_t changedPointerId = 0;
        const bool hasChangedPointerId = OH_ArkUI_PointerEvent_GetChangedPointerId(event, &changedPointerId) ==
                                         ARKUI_ERROR_CODE_NO_ERROR;

        std::optional<std::uint32_t> changedIndex;
        if (hasChangedPointerId) {
            touchEvent.id = static_cast<int32_t>(changedPointerId);
            for (std::uint32_t i = 0; i < pointerCount; ++i) {
                if (touchEvent.points[i].id == touchEvent.id) {
                    changedIndex = i;
                    break;
                }
            }
        } else if (!touchEvent.points.empty()) {
            changedIndex = 0;
        }

        if (changedIndex) {
            const auto& changedPoint = touchEvent.points[*changedIndex];
            touchEvent.id = changedPoint.id;
            touchEvent.x = changedPoint.x;
            touchEvent.y = changedPoint.y;
        } else {
            const auto scale = coordinateScale();
            touchEvent.x = OH_ArkUI_PointerEvent_GetX(event) * scale;
            touchEvent.y = OH_ArkUI_PointerEvent_GetY(event) * scale;
        }

        return touchEvent;
    }

    void clearSurface() {
        if (mapView) {
            if (mln::ohos::hasActiveGesture(gesture)) {
                mapView->setGestureInProgress(false);
            }
            mapView->clearSurface();
        }
        appliedStyleGeneration = 0;
        mln::ohos::resetGestureState(gesture);
    }

    void clearMapState() {
        clearSurface();
        window = nullptr;
        width = 0;
        height = 0;
        surfaceVisible = false;
        mapView.reset();
    }

    void applyDesiredStyle() {
        if (closed || !mapView || !mapView->hasMap() || style.empty() || appliedStyleGeneration == styleGeneration) {
            return;
        }

        mapView->setStyleURL(style);
        appliedStyleGeneration = styleGeneration;
    }

    void updateSurface() {
        if (closed) {
            return;
        }

        auto& currentMapView = ensureMapView();
        if (window == nullptr || width == 0 || height == 0) {
            clearSurface();
            return;
        }

        try {
            const bool needsStyleReapply = !currentMapView.hasMap() || currentMapView.getNativeWindow() != window;
            currentMapView.setSurface(window, toSize(width, height));
            if (needsStyleReapply) {
                appliedStyleGeneration = 0;
            }
            applyDesiredStyle();
            renderFrame();
            lastSurfaceError.clear();
        } catch (const std::exception& exception) {
            lastSurfaceError = exception.what();
            clearSurface();
            mln::Log::Error(mln::Event::Render, exception.what());
        }
    }

    mln::ohos::MapView& ensureMapView() {
        if (!mapView) {
            mapView = std::make_unique<mln::ohos::MapView>(pixelRatio);
        }
        return *mapView;
    }

    void updateFrameRates() {
        const auto now = std::chrono::steady_clock::now();
        const auto renderedFrames = mapView ? mapView->getRenderedFrameCount() : 0;
        const auto frameCallbacks = frameCallbackCount;

        if (frameRateSampleTime.time_since_epoch().count() == 0) {
            frameRateSampleTime = now;
            frameRateRenderedFrames = renderedFrames;
            frameRateCallbacks = frameCallbacks;
            return;
        }

        const auto elapsed = std::chrono::duration<double>(now - frameRateSampleTime).count();
        if (elapsed <= 0.0) {
            return;
        }

        renderedFrameRate = static_cast<double>(renderedFrames - frameRateRenderedFrames) / elapsed;
        frameCallbackRate = static_cast<double>(frameCallbacks - frameRateCallbacks) / elapsed;
        frameRateSampleTime = now;
        frameRateRenderedFrames = renderedFrames;
        frameRateCallbacks = frameCallbacks;
    }

    ArkUI_NodeHandle node = nullptr;
    ArkUI_NodeContentHandle content = nullptr;
    OH_ArkUI_SurfaceHolder* holder = nullptr;
    OH_ArkUI_SurfaceCallback* surfaceCallback = nullptr;
    OHNativeWindow* window = nullptr;
    std::unique_ptr<mln::ohos::MapView> mapView;
    std::string style;
    std::uint64_t styleGeneration = 0;
    std::uint64_t appliedStyleGeneration = 0;
    float pixelRatio = 1.0f;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    bool renderingEnabled = true;
    bool frameCallbackRegistered = false;
    bool nodeEventReceiverRegistered = false;
    bool touchEventRegistered = false;
    bool surfaceCallbackRegistered = false;
    bool ownsNode = false;
    bool nodeAddedToContent = false;
    bool closed = false;
    const std::uint64_t fenceDiagnosticControllerId = fence_diagnostics::lifecycleRecorder().nextControllerId();
    std::uint64_t frameCallbackCount = 0;
    bool surfaceVisible = false;
    std::string lastSurfaceError;
    mln::ohos::GestureState gesture;
    std::optional<OH_NativeXComponent_ExpectedRateRange> frameRateRange;
    std::chrono::steady_clock::time_point frameRateSampleTime;
    std::uint64_t frameRateRenderedFrames = 0;
    std::uint64_t frameRateCallbacks = 0;
    double renderedFrameRate = 0.0;
    double frameCallbackRate = 0.0;
};

using ControllerHandle = std::shared_ptr<SurfaceController>;

napi_value executeBusiness(napi_env env, napi_callback_info info);
napi_value destroy(napi_env env, napi_callback_info info);
napi_value renderFrame(napi_env env, napi_callback_info info);
napi_value reduceMemoryUse(napi_env env, napi_callback_info info);
napi_value setStyleUrl(napi_env env, napi_callback_info info);
napi_value jumpTo(napi_env env, napi_callback_info info);
napi_value setPixelRatio(napi_env env, napi_callback_info info);
napi_value setBounds(napi_env env, napi_callback_info info);
napi_value setRenderingEnabled(napi_env env, napi_callback_info info);
napi_value setFrameRateRange(napi_env env, napi_callback_info info);
napi_value setTileCacheEnabled(napi_env env, napi_callback_info info);
napi_value setClientOptions(napi_env env, napi_callback_info info);
napi_value setResourceOptions(napi_env env, napi_callback_info info);
napi_value getPixelRatio(napi_env env, napi_callback_info info);
napi_value getStyleAttributions(napi_env env, napi_callback_info info);
napi_value getSurfaceState(napi_env env, napi_callback_info info);
napi_value getCameraOptions(napi_env env, napi_callback_info info);

ControllerHandle resolveController(napi_env env, napi_value thisArg) {
    void* nativeController = nullptr;
    if (napi_unwrap(env, thisArg, &nativeController) != napi_ok || nativeController == nullptr) {
        throwError(env, "Expected a MapLibre XComponent context");
        return nullptr;
    }

    auto* controller = static_cast<ControllerHandle*>(nativeController);
    if (controller == nullptr || !*controller) {
        throwError(env, "Expected a MapLibre XComponent context");
        return nullptr;
    }

    return *controller;
}

void finalizeController(napi_env, void* data, void*) {
    auto* controller = static_cast<ControllerHandle*>(data);
    if (controller != nullptr) {
        if (*controller) {
            (*controller)->close();
        }
        delete controller;
    }
}

const napi_property_descriptor* contextProperties(std::size_t& count) {
    static napi_property_descriptor properties[] = {
        {"execute", nullptr, executeBusiness, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroy", nullptr, destroy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCameraOptions", nullptr, getCameraOptions, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getPixelRatio", nullptr, getPixelRatio, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getStyleAttributions", nullptr, getStyleAttributions, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getSurfaceState", nullptr, getSurfaceState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jumpTo", nullptr, jumpTo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"reduceMemoryUse", nullptr, reduceMemoryUse, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"renderFrame", nullptr, renderFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setBounds", nullptr, setBounds, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setClientOptions", nullptr, setClientOptions, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFrameRateRange", nullptr, setFrameRateRange, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPixelRatio", nullptr, setPixelRatio, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setRenderingEnabled", nullptr, setRenderingEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setResourceOptions", nullptr, setResourceOptions, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setTileCacheEnabled", nullptr, setTileCacheEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setStyleUrl", nullptr, setStyleUrl, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    count = sizeof(properties) / sizeof(properties[0]);
    return properties;
}

napi_value createContextObject(napi_env env, ControllerHandle controller) {
    napi_value object = nullptr;
    if (napi_create_object(env, &object) != napi_ok || object == nullptr) {
        return throwError(env, "Could not create MapLibre XComponent context");
    }

    std::size_t propertyCount = 0;
    const auto* properties = contextProperties(propertyCount);
    if (napi_define_properties(env, object, propertyCount, properties) != napi_ok) {
        return throwError(env, "Could not define MapLibre XComponent context");
    }

    auto* boxedController = new ControllerHandle(std::move(controller));
    if (napi_wrap(env, object, boxedController, finalizeController, nullptr, nullptr) != napi_ok) {
        delete boxedController;
        return throwError(env, "Could not wrap MapLibre XComponent context");
    }

    return object;
}

ControllerHandle createController(ArkUI_NodeContentHandle content) {
    return SurfaceController::create(content);
}

napi_value createMap(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected a NodeContent host");
    }

    ArkUI_NodeContentHandle content = nullptr;
    if (OH_ArkUI_GetNodeContentFromNapiValue(env, argv[0], &content) != ARKUI_ERROR_CODE_NO_ERROR ||
        content == nullptr) {
        return throwError(env, "Expected a NodeContent host");
    }

    try {
        return createContextObject(env, createController(content));
    } catch (const std::exception& exception) {
        return throwError(env, exception.what());
    }
}

napi_value executeBusiness(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value thisArg = nullptr;
    napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr);
    std::string input;
    if (argc != 1 || !getString(env, argv[0], input)) return throwError(env, "Expected map operation JSON");
    auto controller = resolveController(env, thisArg);
    if (!controller) return getUndefined(env);
    try { return createStringValue(env, controller->execute(input)); }
    catch (const std::exception& error) { return throwError(env, error.what()); }
}

napi_value destroy(napi_env env, napi_callback_info info) {
    std::size_t argc = 0;
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, nullptr, &thisArg, nullptr) != napi_ok) {
        return throwError(env, "Expected a MapLibre XComponent context");
    }

    if (auto controller = resolveController(env, thisArg)) {
        controller->close();
    }
    return getUndefined(env);
}

napi_value renderFrame(napi_env env, napi_callback_info info) {
    std::size_t argc = 0;
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, nullptr, &thisArg, nullptr) != napi_ok) {
        return throwError(env, "Expected a MapLibre XComponent context");
    }

    if (auto controller = resolveController(env, thisArg)) {
        controller->renderFrame();
    }
    return getUndefined(env);
}

napi_value reduceMemoryUse(napi_env env, napi_callback_info info) {
    std::size_t argc = 0;
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, nullptr, &thisArg, nullptr) != napi_ok) {
        return throwError(env, "Expected a MapLibre XComponent context");
    }

    if (auto controller = resolveController(env, thisArg)) {
        controller->reduceMemoryUse();
    }
    return getUndefined(env);
}

napi_value setStyleUrl(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected style URL");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    std::string style;
    if (!getString(env, argv[0], style)) {
        return throwError(env, "Expected style URL string");
    }

    controller->setStyleUrl(std::move(style));
    return getUndefined(env);
}

napi_value jumpTo(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected camera options");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    mln::CameraOptions cameraOptions;
    if (!getCameraOptionsObject(env, argv[0], cameraOptions)) {
        return throwError(env, "Expected valid camera options object");
    }

    try {
        controller->jumpTo(std::move(cameraOptions));
    } catch (const std::exception& exception) {
        return throwError(env, exception.what());
    }
    return getUndefined(env);
}

napi_value setPixelRatio(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected a pixel ratio");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    double pixelRatio = 0.0;
    if (!getDouble(env, argv[0], pixelRatio) || !std::isfinite(pixelRatio) || pixelRatio <= 0.0) {
        return throwError(env, "Expected a finite positive pixel ratio");
    }

    try {
        controller->setPixelRatio(static_cast<float>(pixelRatio));
    } catch (const std::exception& exception) {
        return throwError(env, exception.what());
    }
    return getUndefined(env);
}

napi_value setBounds(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected bounds options");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    mln::BoundOptions boundOptions;
    if (!getBoundOptions(env, argv[0], boundOptions)) {
        return throwError(env, "Expected valid bounds options object");
    }

    try {
        controller->setBounds(std::move(boundOptions));
    } catch (const std::exception& exception) {
        return throwError(env, exception.what());
    }
    return getUndefined(env);
}

napi_value setRenderingEnabled(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected rendering enabled boolean");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    bool enabled = false;
    if (!getBool(env, argv[0], enabled)) {
        return throwError(env, "Expected rendering enabled boolean");
    }

    controller->setRenderingEnabled(enabled);
    return getUndefined(env);
}

napi_value setFrameRateRange(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected frame-rate range");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    OH_NativeXComponent_ExpectedRateRange range{};
    if (!parseFrameRateRange(env, argv[0], range)) {
        return throwError(env, "Expected frame-rate range object with positive min <= expected <= max");
    }

    if (!controller->setFrameRateRange(range)) {
        return throwError(env, "Could not apply XComponent frame-rate range");
    }
    return getUndefined(env);
}

napi_value setTileCacheEnabled(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected tile cache enabled boolean");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    bool enabled = false;
    if (!getBool(env, argv[0], enabled)) {
        return throwError(env, "Expected tile cache enabled boolean");
    }

    controller->setTileCacheEnabled(enabled);
    return getUndefined(env);
}

napi_value setClientOptions(napi_env env, napi_callback_info info) {
    std::size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected client name and optional client version");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    std::string clientName;
    if (!getString(env, argv[0], clientName)) {
        return throwError(env, "Expected client name string");
    }

    std::string clientVersion;
    if (argc > 1 && !isNullOrUndefined(env, argv[1])) {
        if (!getString(env, argv[1], clientVersion)) {
            return throwError(env, "Expected client version string");
        }
    }

    try {
        controller->setClientOptions(std::move(clientName), std::move(clientVersion));
    } catch (const std::exception& exception) {
        return throwError(env, exception.what());
    }
    return getUndefined(env);
}

napi_value setResourceOptions(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok || argc < 1 || argv[0] == nullptr) {
        return throwError(env, "Expected resource options");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }
    if (!isObject(env, argv[0])) {
        return throwError(env, "Expected resource options object");
    }

    std::optional<std::string> apiKey;
    std::optional<std::string> cachePath;
    std::optional<std::string> assetPath;
    if (!getOptionalStringProperty(env, argv[0], "apiKey", apiKey) ||
        !getOptionalStringProperty(env, argv[0], "cachePath", cachePath) ||
        !getOptionalStringProperty(env, argv[0], "assetPath", assetPath)) {
        return throwError(env, "Expected string resource option values");
    }

    try {
        controller->setResourceOptions(apiKey, cachePath, assetPath);
    } catch (const std::exception& exception) {
        return throwError(env, exception.what());
    }
    return getUndefined(env);
}

napi_value getPixelRatio(napi_env env, napi_callback_info info) {
    std::size_t argc = 0;
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, nullptr, &thisArg, nullptr) != napi_ok) {
        return throwError(env, "Expected a MapLibre XComponent context");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    napi_value result = nullptr;
    napi_create_double(env, controller->getPixelRatio(), &result);
    return result;
}

napi_value getStyleAttributions(napi_env env, napi_callback_info info) {
    std::size_t argc = 0;
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, nullptr, &thisArg, nullptr) != napi_ok) {
        return throwError(env, "Expected a MapLibre XComponent context");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    return createStringArray(env, controller->getStyleAttributions());
}

napi_value getSurfaceState(napi_env env, napi_callback_info info) {
    std::size_t argc = 0;
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, nullptr, &thisArg, nullptr) != napi_ok) {
        return throwError(env, "Expected a MapLibre XComponent context");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    return controller->createSurfaceStateObject(env);
}

napi_value getCameraOptions(napi_env env, napi_callback_info info) {
    std::size_t argc = 0;
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, nullptr, &thisArg, nullptr) != napi_ok) {
        return throwError(env, "Expected a MapLibre XComponent context");
    }

    auto controller = resolveController(env, thisArg);
    if (!controller) {
        return getUndefined(env);
    }

    return createCameraOptionsObject(env, controller->getCameraOptions());
}

// Diagnostic only: counts match the existing probe, without exposing FD paths.
napi_value sampleResources(napi_env env, napi_callback_info) {
    int counts[] = {0, 0, 0, 0, 0};
    {
        std::scoped_lock lock(controllerRegistryMutex());
        counts[0] = static_cast<int>(controllersByNode().size());
    }
    DIR* directory = opendir("/proc/self/fd");
    if (directory) {
        while (const auto* entry = readdir(directory)) {
            if (entry->d_name[0] == '.') continue;
            char target[512];
            const std::string path = std::string("/proc/self/fd/") + entry->d_name;
            const auto length = readlink(path.c_str(), target, sizeof(target) - 1);
            if (length < 0) {
                // Other threads can close a descriptor after readdir. A missing
                // link is no longer an open resource; retain unexpected errors.
                if (errno != ENOENT) ++counts[4];
                continue;
            }
            ++counts[1];
            target[length] = 0;
            if (strncmp(target, "/dev/dri/", 9) == 0 || strcmp(target, "/dev/teleport_express") == 0 ||
                strcmp(target, "/dev/hvgr0") == 0) ++counts[2];
            if (strstr(target, "sync_file") || strstr(target, "fence") || strcmp(target, "/dev/express_sync") == 0) ++counts[3];
        }
        closedir(directory);
    } else { counts[1] = -1; }
    napi_value result;
    napi_create_array_with_length(env, 5, &result);
    for (uint32_t index = 0; index < 5; ++index) {
        napi_value value;
        napi_create_int32(env, counts[index], &value);
        napi_set_element(env, result, index, value);
    }
    return result;
}

napi_value sampleHttpRequests(napi_env env, napi_callback_info) {
    const auto counts = mln::ohos::httpRequestCounts();
    napi_value result;
    napi_create_array_with_length(env, counts.size(), &result);
    for (uint32_t index = 0; index < counts.size(); ++index) {
        napi_value value;
        napi_create_double(env, static_cast<double>(counts[index]), &value);
        napi_set_element(env, result, index, value);
    }
    return result;
}

napi_value sampleImageResources(napi_env env, napi_callback_info) {
    const auto counts = mln::ohos::nativeImageResourceCounts();
    constexpr std::array<const char*, 8> names = {"imageSourceImpls",
                                                "renderImageSources",
                                                "rasterBuckets",
                                                "rendererImpls",
                                                "decoderImageSources",
                                                "decoderOptions",
                                                "decoderPixelmaps",
                                                "decoderImageInfos"};
    static_assert(names.size() == static_cast<std::size_t>(mln::ohos::NativeImageResource::Count));
    napi_value result;
    napi_create_object(env, &result);
    for (std::size_t index = 0; index < names.size(); ++index) {
        setSizeProperty(env, result, names[index], counts.live[index]);
    }
    setSizeProperty(env, result, "decoderReleaseFailures", counts.decoderReleaseFailures);
    return result;
}

napi_value setFenceDiagnosticRecording(napi_env env, napi_callback_info info) {
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_valuetype type = napi_undefined;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 1 ||
        napi_typeof(env, argv[0], &type) != napi_ok || type != napi_boolean) {
        return throwError(env, "Expected diagnostic recording boolean");
    }
    bool enabled = false;
    if (napi_get_value_bool(env, argv[0], &enabled) != napi_ok) {
        return throwError(env, "Expected diagnostic recording boolean");
    }
    fence_diagnostics::lifecycleRecorder().setRecording(enabled);
    return getUndefined(env);
}

// Independent opt-in diagnostics. Never changes the existing five-count sample.
// This is a bounded scan interval, not a process-wide atomic snapshot or owner proof.
napi_value sampleFenceResources(napi_env env, napi_callback_info) {
    try {
        const auto sample = fence_diagnostics::sample();
        const auto lifecycle = fence_diagnostics::lifecycleRecorder().read();
        constexpr std::uint64_t safeInteger = 9007199254740991ULL;
        if (sample.beginNs / 1000 > safeInteger || sample.endNs / 1000 > safeInteger ||
            sample.firstTimestamp / 1000 > safeInteger || sample.lastTimestamp / 1000 > safeInteger ||
            lifecycle.epoch > safeInteger || lifecycle.overwritten > safeInteger || lifecycle.clockFailures > safeInteger) {
            return throwError(env, "Fence diagnostic numeric range");
        }
        for (unsigned index = 0; index < lifecycle.count; ++index) {
            const auto& event = lifecycle.events[index];
            if (event.sequence > safeInteger || event.controllerId > safeInteger || event.timeUs > safeInteger) {
                return throwError(env, "Fence diagnostic numeric range");
            }
        }
        napi_value result = nullptr;
        napi_create_object(env, &result);
        setSizeProperty(env, result, "schema", 1);
        setSizeProperty(env, result, "entries", sample.entries);
        setSizeProperty(env, result, "syncCandidates", sample.syncCandidates);
        setSizeProperty(env, result, "legacyCandidates", sample.legacyCandidates);
        setSizeProperty(env, result, "confirmedSyncDescriptors", sample.confirmedSyncDescriptors);
        setSizeProperty(env, result, "closed", sample.closed);
        setSizeProperty(env, result, "linkErrors", sample.linkErrors);
        setSizeProperty(env, result, "duplicateErrors", sample.duplicateErrors);
        setSizeProperty(env, result, "changedAfterDuplicate", sample.changedAfterDuplicate);
        setSizeProperty(env, result, "queryOk", sample.queryOk);
        setSizeProperty(env, result, "unsupported", sample.unsupported);
        setSizeProperty(env, result, "denied", sample.denied);
        setSizeProperty(env, result, "queryErrors", sample.queryErrors);
        setSizeProperty(env, result, "active", sample.active);
        setSizeProperty(env, result, "signaled", sample.signaled);
        setSizeProperty(env, result, "errorStatus", sample.errorStatus);
        setSizeProperty(env, result, "invalidStatus", sample.invalidStatus);
        setSizeProperty(env, result, "componentTotal", sample.componentTotal);
        setSizeProperty(env, result, "componentReads", sample.componentReads);
        setSizeProperty(env, result, "componentReadErrors", sample.componentReadErrors);
        setSizeProperty(env, result, "componentTooLarge", sample.componentTooLarge);
        setSizeProperty(env, result, "childActive", sample.childActive);
        setSizeProperty(env, result, "childSignaled", sample.childSignaled);
        setSizeProperty(env, result, "childError", sample.childError);
        setSizeProperty(env, result, "childInvalid", sample.childInvalid);
        setSizeProperty(env, result, "timestamps", sample.timestamps);
        setBoolProperty(env, result, "directoryAvailable", sample.directoryAvailable);
        setBoolProperty(env, result, "directoryComplete", sample.directoryComplete);
        setSizeProperty(env, result, "beginUs", sample.beginNs / 1000);
        setSizeProperty(env, result, "endUs", sample.endNs / 1000);
        setSizeProperty(env, result, "firstTimestampUs", sample.firstTimestamp / 1000);
        setSizeProperty(env, result, "lastTimestampUs", sample.lastTimestamp / 1000);
        setBoolProperty(env, result, "clockAvailable", sample.beginNs != 0 && sample.endNs >= sample.beginNs);
        setBoolProperty(env, result, "syncReadComplete", sample.directoryAvailable && sample.directoryComplete &&
            sample.closed == 0 && sample.linkErrors == 0 && sample.duplicateErrors == 0 &&
            sample.changedAfterDuplicate == 0 && sample.unsupported == 0 && sample.denied == 0 &&
            sample.queryErrors == 0 && sample.componentReadErrors == 0 && sample.componentTooLarge == 0 &&
            sample.invalidStatus == 0 && sample.childInvalid == 0 && sample.queryOk == sample.syncCandidates);
        setBoolProperty(env, result, "lifecycleRecording", lifecycle.recording);
        setSizeProperty(env, result, "lifecycleEpoch", lifecycle.epoch);
        setSizeProperty(env, result, "lifecycleCapacity", fence_diagnostics::LifecycleSample::capacity);
        setSizeProperty(env, result, "lifecycleOverwritten", lifecycle.overwritten);
        setSizeProperty(env, result, "lifecycleClockFailures", lifecycle.clockFailures);
        napi_value events = nullptr;
        napi_create_array_with_length(env, lifecycle.count, &events);
        for (unsigned index = 0; index < lifecycle.count; ++index) {
            const auto& event = lifecycle.events[index];
            napi_value value = nullptr;
            napi_create_object(env, &value);
            setSizeProperty(env, value, "sequence", event.sequence);
            setSizeProperty(env, value, "controllerId", event.controllerId);
            setSizeProperty(env, value, "timeUs", event.timeUs);
            setSizeProperty(env, value, "phase", event.phase);
            setBoolProperty(env, value, "operationAttempted", event.operationAttempted);
            napi_set_element(env, events, index, value);
        }
        napi_set_named_property(env, result, "lifecycleEvents", events);
        return result;
    } catch (...) {
        return throwError(env, "Fence diagnostics unavailable");
    }
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor properties[] = {
        {"sampleFenceResources", nullptr, sampleFenceResources, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFenceDiagnosticRecording", nullptr, setFenceDiagnosticRecording, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sampleImageResources", nullptr, sampleImageResources, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sampleHttpRequests", nullptr, sampleHttpRequests, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sampleResources", nullptr, sampleResources, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createMap", nullptr, createMap, nullptr, nullptr, nullptr, napi_default, nullptr},
    };

    napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
    return exports;
}

} // namespace

NAPI_MODULE(maplibre_native_ohos, Init)
