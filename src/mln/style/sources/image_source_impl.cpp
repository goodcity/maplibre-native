#include <mln/style/sources/image_source_impl.hpp>
#if defined(OHOS_PLATFORM)
#include <mln/util/native_resource_diagnostics.hpp>
#endif

namespace mln {
namespace style {

ImageSource::Impl::Impl(std::string id_, std::array<LatLng, 4> coords_)
    : Source::Impl(SourceType::Image, std::move(id_)),
      coords(coords_) {
#if defined(OHOS_PLATFORM)
    ohos::retainNativeImageResource(ohos::NativeImageResource::ImageSourceImpl);
#endif
}

ImageSource::Impl::Impl(const Impl& other)
    : Source::Impl(other),
      coords(other.coords),
      image(other.image) {
#if defined(OHOS_PLATFORM)
    ohos::retainNativeImageResource(ohos::NativeImageResource::ImageSourceImpl);
#endif
}

ImageSource::Impl::Impl(const Impl& other, std::array<LatLng, 4> coords_)
    : Source::Impl(other),
      coords(coords_),
      image(other.image) {
#if defined(OHOS_PLATFORM)
    ohos::retainNativeImageResource(ohos::NativeImageResource::ImageSourceImpl);
#endif
}

ImageSource::Impl::Impl(const Impl& rhs, PremultipliedImage&& image_)
    : Source::Impl(rhs),
      coords(rhs.coords),
      image(std::make_shared<PremultipliedImage>(std::move(image_))) {
#if defined(OHOS_PLATFORM)
    ohos::retainNativeImageResource(ohos::NativeImageResource::ImageSourceImpl);
#endif
}

ImageSource::Impl::~Impl() {
#if defined(OHOS_PLATFORM)
    ohos::releaseNativeImageResource(ohos::NativeImageResource::ImageSourceImpl);
#endif
}

std::shared_ptr<PremultipliedImage> ImageSource::Impl::getImage() const {
    return image;
}

std::array<LatLng, 4> ImageSource::Impl::getCoordinates() const {
    return coords;
}

std::optional<std::string> ImageSource::Impl::getAttribution() const {
    return {};
}

} // namespace style
} // namespace mln
