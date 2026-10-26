#pragma once

#include <array>
#include <cstdint>

namespace mln::ohos {
// Active callback slots (including canceled requests awaiting their terminal
// callback), and live requests waiting for a slot. No request data is exposed.
std::array<uint64_t, 2> httpRequestCounts();
} // namespace mln::ohos
