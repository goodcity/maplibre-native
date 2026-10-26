#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace mln::ohos {

// Called under RequestState's mutex. Data callbacks provide binary-safe chunks;
// Http_Response::body.buffer does not on affected OpenHarmony versions.
class HttpResponseBody {
public:
    static constexpr std::size_t maxBytes = 64 * 1024 * 1024;

    void append(const char* data, std::size_t length) noexcept {
        if (invalid || length == 0) return;
        if (!data || length > maxBytes - bytes.size()) {
            discard();
            return;
        }
        try {
            bytes.append(data, length);
        } catch (...) {
            // No C++ exception may escape the system's C callback.
            discard();
        }
    }

    std::optional<std::string> take(std::size_t expectedLength) noexcept {
        if (invalid || bytes.size() != expectedLength) return std::nullopt;
        invalid = true;
        return std::move(bytes);
    }

    void discard() noexcept {
        invalid = true;
        std::string{}.swap(bytes);
    }

private:
    std::string bytes;
    bool invalid = false;
};

} // namespace mln::ohos
