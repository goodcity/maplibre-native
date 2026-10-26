#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mln::ohos {

// Numeric diagnostics only. No object pointers, image data, or ownership is kept.
enum class NativeImageResource : std::size_t {
    ImageSourceImpl,
    RenderImageSource,
    RasterBucket,
    RendererImpl,
    DecoderImageSource,
    DecoderOptions,
    DecoderPixelmap,
    DecoderImageInfo,
    Count,
};

struct NativeImageResourceCounts {
    std::array<std::uint64_t, static_cast<std::size_t>(NativeImageResource::Count)> live;
    // Cumulative failed OH release calls; live decoder wrappers can be zero
    // even when the underlying release failed.
    std::uint64_t decoderReleaseFailures;
};

void retainNativeImageResource(NativeImageResource) noexcept;
void releaseNativeImageResource(NativeImageResource) noexcept;
void recordNativeImageReleaseFailure() noexcept;
NativeImageResourceCounts nativeImageResourceCounts() noexcept;

} // namespace mln::ohos
