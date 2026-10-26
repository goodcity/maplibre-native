#include "map_view.hpp"
#include "renderer_frontend.hpp"

#include <mln/map/bound_options.hpp>
#include <mln/style/style.hpp>
#include <mln/style/source.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/sources/image_source.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/conversion/source.hpp>
#include <mln/style/conversion/layer.hpp>
#include <mln/style/conversion/geojson.hpp>
#include <mln/util/image.hpp>
#include <mln/util/rapidjson.hpp>
#include <mln/renderer/renderer.hpp>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cmath>
#include <future>
#include <unordered_set>
#include <vector>

namespace mln::ohos {
namespace {
std::string json(const JSValue& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return buffer.GetString();
}
const JSValue& member(const JSValue& value, const char* name) {
    if (!value.IsObject() || !value.HasMember(name)) throw std::invalid_argument(std::string("Missing map field: ") + name);
    return value[name];
}
std::string text(const JSValue& value, const char* name) {
    const auto& field = member(value, name);
    if (!field.IsString()) throw std::invalid_argument(std::string("Expected string: ") + name);
    return field.GetString();
}
double number(const JSValue& value, const char* name, double fallback) {
    if (!value.HasMember(name)) return fallback;
    const auto& field = value[name];
    if (!field.IsNumber() || !std::isfinite(field.GetDouble())) throw std::invalid_argument("Expected finite map number");
    return field.GetDouble();
}
double element(const JSValue& value, rapidjson::SizeType index) {
    if (!value.IsArray() || value.Size() <= index || !value[index].IsNumber() || !std::isfinite(value[index].GetDouble()))
        throw std::invalid_argument("Expected finite coordinate");
    return value[index].GetDouble();
}
std::string fileBytes(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not read map image");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}

std::string MapView::execute(const std::string& input) {
    if (!map || !backend) throw std::runtime_error("Map Surface is not ready");
    JSDocument request;
    request.Parse(input.c_str());
    if (request.HasParseError() || !request.IsObject()) throw std::invalid_argument("Invalid map operation JSON");
    const auto op = text(request, "op");
    if (op == "probeImageThreads") {
        // Diagnostic only: no map mutation, render submission or HTTP request.
        const auto bytes = fileBytes(text(request, "path"));
        const auto count = static_cast<int>(number(request, "threads", 1));
        if (count < 1 || count > 8) throw std::invalid_argument("Expected 1 to 8 decoder probe threads");
        std::vector<std::future<PremultipliedImage>> tasks;
        for (int i = 0; i < count; ++i) {
            tasks.emplace_back(std::async(std::launch::async, [&bytes] { return decodeImage(bytes); }));
        }
        for (auto& task : tasks) {
            const auto image = task.get();
            if (image.size.isEmpty()) throw std::runtime_error("Decoder probe returned an empty image");
        }
        return "true";
    }
    auto& style = map->getStyle();
    if (op == "hasSource") return style.getSource(text(request, "id")) ? "true" : "false";
    if (op == "hasLayer") return style.getLayer(text(request, "id")) ? "true" : "false";
    if (op == "source") {
        const auto id = text(request, "id");
        style::conversion::Error error;
        auto converted = style::conversion::convertJSON<std::unique_ptr<style::Source>>(json(member(request, "source")), error, id);
        if (!converted) throw std::invalid_argument(error.message);
        if (style.getSource(id)) throw std::invalid_argument("Map source already exists");
        style.addSource(std::move(*converted));
    } else if (op == "geojson") {
        auto* source = style.getSource(text(request, "id"));
        if (!source || !source->is<style::GeoJSONSource>()) throw std::invalid_argument("Expected GeoJSON source");
        style::conversion::Error error;
        auto data = style::conversion::convertJSON<GeoJSON>(json(member(request, "data")), error);
        if (!data) throw std::invalid_argument(error.message);
        source->as<style::GeoJSONSource>()->setGeoJSON(*data);
    } else if (op == "image") {
        const auto id = text(request, "id");
        const auto& coordinates = member(request, "corners");
        if (!coordinates.IsArray() || coordinates.Size() != 4) throw std::invalid_argument("Expected four image corners");
        std::array<LatLng, 4> corners;
        for (rapidjson::SizeType i = 0; i < 4; ++i) corners[i] = LatLng(element(coordinates[i], 1), element(coordinates[i], 0));
        auto image = decodeImage(fileBytes(text(request, "path")));
        auto* source = style.getSource(id);
        if (source) {
            if (!source->is<style::ImageSource>()) throw std::invalid_argument("Expected image source");
            auto* overlay = source->as<style::ImageSource>();
            overlay->setCoordinates(corners);
            overlay->setImage(std::move(image));
        } else {
            auto overlay = std::make_unique<style::ImageSource>(id, corners);
            overlay->setImage(std::move(image));
            style.addSource(std::move(overlay));
        }
    } else if (op == "layer") {
        style::conversion::Error error;
        auto layer = style::conversion::convertJSON<std::unique_ptr<style::Layer>>(json(member(request, "layer")), error);
        if (!layer) throw std::invalid_argument(error.message);
        const auto id = (*layer)->getID();
        std::optional<std::string> before;
        if (request.HasMember("before") && request["before"].IsString()) {
            before = request["before"].GetString();
            if (!style.getLayer(*before)) before.reset();
        }
        if (style.getLayer(id)) style.removeLayer(id);
        style.addLayer(std::move(*layer), before);
    } else if (op == "removeLayer") {
        const auto id = text(request, "id");
        if (style.getLayer(id)) style.removeLayer(id);
    } else if (op == "removeSource") {
        const auto id = text(request, "id");
        if (style.getSource(id)) style.removeSource(id);
    } else if (op == "styleImage") {
        auto image = decodeImage(fileBytes(text(request, "path")));
        style.addImage(std::make_unique<style::Image>(text(request, "id"), std::move(image), number(request, "pixelRatio", 1.0), false));
    } else if (op == "camera") {
        CameraOptions camera;
        if (request.HasMember("center")) {
            const auto& center = request["center"];
            camera.withCenter(LatLng(element(center, 1), element(center, 0)));
        }
        if (request.HasMember("zoom")) camera.withZoom(number(request, "zoom", 0));
        if (request.HasMember("bearing")) camera.withBearing(number(request, "bearing", 0));
        if (request.HasMember("pitch")) camera.withPitch(number(request, "pitch", 0));
        AnimationOptions animation;
        animation.duration = std::chrono::milliseconds(static_cast<int>(number(request, "durationMs", 0)));
        if (*animation.duration > std::chrono::milliseconds(0)) easeTo(camera, animation); else jumpTo(camera);
    } else if (op == "fitBounds") {
        const auto& sw = member(request, "southwest");
        const auto& ne = member(request, "northeast");
        auto bounds = LatLngBounds::hull(LatLng(element(sw, 1), element(sw, 0)), LatLng(element(ne, 1), element(ne, 0)));
        const auto& padding = member(request, "padding");
        // Flutter's newLatLngBounds resets orientation. Calculate the fit in
        // that orientation as well, so padding and zoom match the final view.
        auto camera = map->cameraForLatLngBounds(bounds, EdgeInsets(element(padding, 0), element(padding, 1),
                                                                  element(padding, 2), element(padding, 3)),
                                                0.0, 0.0);
        AnimationOptions animation;
        animation.duration = std::chrono::milliseconds(static_cast<int>(number(request, "durationMs", 0)));
        if (*animation.duration > std::chrono::milliseconds(0)) easeTo(camera, animation); else jumpTo(camera);
    } else if (op == "scroll") {
        AnimationOptions animation;
        animation.duration = std::chrono::milliseconds(static_cast<int>(number(request, "durationMs", 0)));
        map->moveBy({number(request, "x", 0), number(request, "y", 0)}, animation);
    } else if (op == "zoomBy") {
        auto camera = map->getCameraOptions();
        CameraOptions next;
        next.withZoom(camera.zoom.value_or(0) + number(request, "delta", 0));
        if (request.HasMember("focus") && !request["focus"].IsNull()) {
            next.withAnchor(ScreenCoordinate{element(request["focus"], 0), element(request["focus"], 1)});
        }
        AnimationOptions animation;
        animation.duration = std::chrono::milliseconds(static_cast<int>(number(request, "durationMs", 0)));
        if (*animation.duration > std::chrono::milliseconds(0)) easeTo(next, animation); else jumpTo(next);
    } else if (op == "configure") {
        setBounds(BoundOptions().withMinZoom(number(request, "minZoom", 3))
                                     .withMaxZoom(number(request, "maxZoom", 18)));
        zoomGesturesEnabled = member(request, "zoomGestures").GetBool();
        rotateGesturesEnabled = member(request, "rotateGestures").GetBool();
        tiltGesturesEnabled = member(request, "tiltGestures").GetBool();
    } else if (op == "project" || op == "unproject") {
        JSDocument result(rapidjson::kArrayType);
        const auto& points = member(request, "points");
        if (!points.IsArray()) throw std::invalid_argument("Expected points array");
        for (const auto& point : points.GetArray()) {
            JSValue pair(rapidjson::kArrayType);
            if (op == "project") {
                const auto pixel = map->pixelForLatLng(LatLng(element(point, 1), element(point, 0)));
                pair.PushBack(pixel.x, result.GetAllocator()).PushBack(pixel.y, result.GetAllocator());
            } else {
                const auto coordinate = map->latLngForPixel({element(point, 0), element(point, 1)});
                pair.PushBack(coordinate.longitude(), result.GetAllocator()).PushBack(coordinate.latitude(), result.GetAllocator());
            }
            result.PushBack(pair, result.GetAllocator());
        }
        return json(result);
    } else if (op == "query") {
        const auto& point = member(request, "point");
        const auto& layers = member(request, "layers");
        if (!layers.IsArray()) throw std::invalid_argument("Expected query layers");
        std::unordered_set<std::string> requestedLayers;
        for (const auto& layer : layers.GetArray()) {
            if (!layer.IsString()) throw std::invalid_argument("Expected layer id");
            requestedLayers.emplace(layer.GetString());
        }
        JSDocument result(rapidjson::kArrayType);
        // Registration order can differ from drawing order after a "before"
        // insertion or Style reload. Match the existing mobile tap contract:
        // topmost Style layer first, preserving native feature order per layer.
        const auto styleLayers = style.getLayers();
        for (auto layer = styleLayers.rbegin(); layer != styleLayers.rend(); ++layer) {
            const auto& layerId = (*layer)->getID();
            if (!requestedLayers.count(layerId)) continue;
            const auto features = frontend->queryFeatures({element(point, 0), element(point, 1)}, layerId);
            for (const auto& feature : features) {
                const auto id = featureIDtoString(feature.id);
                if (!id) continue;
                JSValue item(rapidjson::kObjectType);
                item.AddMember("id", JSValue(id->c_str(), result.GetAllocator()), result.GetAllocator());
                item.AddMember("layer", JSValue(layerId.c_str(), result.GetAllocator()), result.GetAllocator());
                result.PushBack(item, result.GetAllocator());
            }
        }
        return json(result);
    } else if (op == "snapshot") {
        backend->requestSnapshot();
    } else if (op == "snapshotResult") {
        return backend->writeSnapshot(text(request, "path")) ? "true" : "false";
    } else {
        throw std::invalid_argument("Unsupported native map operation");
    }
    frontend->markSubmission();
    map->triggerRepaint();
    return "null";
}
}
