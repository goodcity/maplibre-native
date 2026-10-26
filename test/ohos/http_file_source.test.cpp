// Host-only regression for the real OHOS request state machine. Compile this
// translation unit instead of the platform HTTP source, link the host core,
// and supply the OHOS SDK include directory with -idirafter. The C functions
// below model net_http ownership and explicitly controlled terminal callbacks;
// no network, credentials or OHOS runtime are involved.
#include "../../platform/ohos/src/http_file_source.cpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

struct Exchange {
    Http_ResponseCallback response;
    Http_EventsHandler events;
    bool destroyed = false;
};

std::vector<Exchange> exchanges;
std::function<int()> onStart;
bool synchronousCancellation = false;
std::size_t destroyedRequests = 0;
std::size_t liveHeaders = 0;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void resetFixture() {
    check(mln::ohos::httpRequestCounts() == std::array<uint64_t, 2>{0, 0}, "Requests remain after previous case");
    check(liveHeaders == 0, "Headers remain after previous case");
    exchanges.clear();
    onStart = nullptr;
    synchronousCancellation = false;
    destroyedRequests = 0;
}

void destroyFixtureResponse(Http_Response** response) {
    delete *response;
    *response = nullptr;
}

void respond(std::size_t index, uint32_t length, Http_ResponseCode status = OH_HTTP_OK) {
    auto* response = new Http_Response{};
    response->body.length = length;
    // Deliberately absent: the production path must use binary data callbacks.
    response->body.buffer = nullptr;
    response->responseCode = status;
    response->destroyResponse = destroyFixtureResponse;
    exchanges.at(index).response(response, OH_HTTP_RESULT_OK);
}

using State = std::shared_ptr<mln::RequestState>;

State request(
    mln::util::RunLoop& loop, mln::FileSource::Callback callback = [](const mln::Response&) {}) {
    return std::make_shared<mln::RequestState>(
        loop, mln::Resource(mln::Resource::Kind::Tile, "https://fixture.invalid/tile"), "fixture", std::move(callback));
}

void cancelDuringStart(bool synchronous) {
    mln::util::RunLoop loop;
    bool delivered = false;
    auto state = request(loop, [&](const mln::Response&) { delivered = true; });
    synchronousCancellation = synchronous;
    onStart = [&] {
        state->cancel();
        return OH_HTTP_RESULT_OK;
    };
    state->begin();
    onStart = nullptr;
    const auto beforeTerminal = mln::ohos::httpRequestCounts();
    if (!synchronous) exchanges.front().events.onCanceled();
    loop.runOnce();
    check(beforeTerminal == std::array<uint64_t, 2>{synchronous ? 0u : 1u, 0},
          "Canceled start released its slot before the terminal callback");
    check(!delivered && destroyedRequests == 1, "Cancellation delivered a result or destroyed the request twice");
}

void cancelAfterStart() {
    mln::util::RunLoop loop;
    bool delivered = false;
    auto state = request(loop, [&](const mln::Response&) { delivered = true; });
    state->begin();
    state->cancel();
    state->cancel();
    const auto beforeTerminal = mln::ohos::httpRequestCounts();
    exchanges.front().events.onCanceled();
    loop.runOnce();
    check(beforeTerminal == std::array<uint64_t, 2>{1, 0}, "Ordinary cancellation released its slot early");
    check(!delivered && destroyedRequests == 1, "Repeated cancellation changed ownership");
}

void cancelFailedStart() {
    mln::util::RunLoop loop;
    bool delivered = false;
    auto state = request(loop, [&](const mln::Response&) { delivered = true; });
    onStart = [&] {
        state->cancel();
        return OH_HTTP_CONNECT_SERVER_FAILED;
    };
    state->begin();
    onStart = nullptr;
    loop.runOnce();
    check(mln::ohos::httpRequestCounts() == std::array<uint64_t, 2>{0, 0},
          "Failed start awaits a nonexistent callback");
    check(!delivered && destroyedRequests == 1, "Failed canceled start delivered a result or leaked its request");
}

void cancelBeforeStart() {
    mln::util::RunLoop loop;
    auto state = request(loop);
    state->cancel();
    state->begin();
    check(exchanges.empty() && destroyedRequests == 0, "Pre-canceled request started native work");
}

void queuedWorkWaitsForTerminal() {
    mln::util::RunLoop loop;
    std::vector<State> states;
    auto first = request(loop);
    onStart = [&] {
        first->cancel();
        return OH_HTTP_RESULT_OK;
    };
    first->begin();
    onStart = nullptr;
    states.push_back(first);
    for (std::size_t i = 0; i < 128; ++i) {
        auto state = request(loop);
        states.push_back(state);
        state->begin();
    }
    const auto initialCount = mln::ohos::httpRequestCounts();
    const auto initialStarts = exchanges.size();
    exchanges.front().events.onCanceled();
    const auto afterTerminal = mln::ohos::httpRequestCounts();
    const auto afterStarts = exchanges.size();
    for (const auto& state : states) state->cancel();
    for (std::size_t i = 0; i < exchanges.size(); ++i) exchanges[i].events.onCanceled();
    check(initialCount == std::array<uint64_t, 2>{128, 1} && initialStarts == 128,
          "Queued work reused a canceled slot before its terminal callback");
    check(afterTerminal == std::array<uint64_t, 2>{128, 0} && afterStarts == 129,
          "Terminal callback did not start exactly one pending request");
    check(destroyedRequests == 129, "Queued request destruction count differs");
}

void lateDataAndSuccessAfterCancellation() {
    mln::util::RunLoop loop;
    bool delivered = false;
    auto state = request(loop, [&](const mln::Response&) { delivered = true; });
    state->begin();
    state->cancel();
    exchanges.front().events.onDataReceive("late", 4);
    respond(0, 4);
    loop.runOnce();
    check(!delivered && destroyedRequests == 1, "Canceled request accepted a late response");
}

void responseBody(bool complete) {
    mln::util::RunLoop loop;
    std::size_t delivered = 0;
    bool bodyMatches = false;
    bool connectionError = false;
    const char binary[] = {'P', '\0', '\xff', 'G'};
    auto state = request(loop, [&](const mln::Response& response) {
        ++delivered;
        bodyMatches = response.data && *response.data == std::string(binary, sizeof(binary));
        connectionError = response.error && response.error->reason == mln::Response::Error::Reason::Connection;
    });
    state->begin();
    exchanges.front().events.onDataReceive(binary, 2);
    if (complete) exchanges.front().events.onDataReceive(binary + 2, 2);
    respond(0, sizeof(binary));
    loop.runOnce();
    check(delivered == 1, "Response was not delivered exactly once");
    check(complete ? bodyMatches && !connectionError : !bodyMatches && connectionError,
          "Binary response completeness was reported incorrectly");
}

void cancelBeforeQueuedDelivery() {
    mln::util::RunLoop loop;
    bool delivered = false;
    auto state = request(loop, [&](const mln::Response&) { delivered = true; });
    state->begin();
    respond(0, 0);
    state->cancel();
    loop.runOnce();
    check(!delivered && destroyedRequests == 1, "Cancellation failed to suppress queued result delivery");
}

} // namespace

extern "C" {

Http_Request* OH_Http_CreateRequest(const char* url) {
    auto* result = new Http_Request{};
    result->url = strdup(url);
    return result;
}

Http_Headers* OH_Http_CreateHeaders() {
    ++liveHeaders;
    return reinterpret_cast<Http_Headers*>(new int(0));
}

void OH_Http_DestroyHeaders(Http_Headers** headers) {
    if (!*headers) return;
    --liveHeaders;
    delete reinterpret_cast<int*>(*headers);
    *headers = nullptr;
}

uint32_t OH_Http_SetHeaderValue(Http_Headers*, const char*, const char*) {
    return OH_HTTP_RESULT_OK;
}
Http_HeaderEntry* OH_Http_GetHeaderEntries(Http_Headers*) {
    return nullptr;
}
void OH_Http_DestroyHeaderEntries(Http_HeaderEntry**) {}

int OH_Http_Request(Http_Request* nativeRequest, Http_ResponseCallback response, Http_EventsHandler events) {
    exchanges.push_back({response, events});
    nativeRequest->requestId = static_cast<uint32_t>(exchanges.size());
    return onStart ? onStart() : OH_HTTP_RESULT_OK;
}

void OH_Http_Destroy(Http_Request** nativeRequest) {
    if (!*nativeRequest) return;
    const auto index = (*nativeRequest)->requestId - 1;
    const auto canceled = exchanges.at(index).events.onCanceled;
    exchanges.at(index).destroyed = true;
    ++destroyedRequests;
    free((*nativeRequest)->url);
    delete *nativeRequest;
    *nativeRequest = nullptr;
    if (synchronousCancellation) canceled();
}

} // extern "C"

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"cancel-during-start",
         [] {
             cancelDuringStart(false);
         }},
        {"synchronous-cancel-callback",
         [] {
             cancelDuringStart(true);
         }},
        {"cancel-after-start", cancelAfterStart},
        {"cancel-failed-start", cancelFailedStart},
        {"cancel-before-start", cancelBeforeStart},
        {"pending-waits-for-terminal", queuedWorkWaitsForTerminal},
        {"late-data-and-success", lateDataAndSuccessAfterCancellation},
        {"binary-response",
         [] {
             responseBody(true);
         }},
        {"incomplete-response",
         [] {
             responseBody(false);
         }},
        {"cancel-before-delivery", cancelBeforeQueuedDelivery},
    };
    std::size_t failed = 0;
    for (const auto& [name, test] : cases) {
        try {
            resetFixture();
            test();
            check(mln::ohos::httpRequestCounts() == std::array<uint64_t, 2>{0, 0}, "Case left pending requests");
            check(liveHeaders == 0, "Case left live headers");
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << "cases=" << cases.size() << ", failed=" << failed << '\n';
    return failed ? 1 : 0;
}
