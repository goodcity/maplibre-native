// Standalone host regression: clang++ -std=c++17 -fsanitize=address,undefined
// -Iplatform/ohos/src test/ohos/http_response_body.test.cpp -o /tmp/http-body-test
#include "http_response_body.hpp"

#include <cassert>
#include <string>

int main() {
    using mln::ohos::HttpResponseBody;
    const char binary[] = {'P', 'N', 'G', '\0', '\xff', 'I', '\0', 'D'};
    HttpResponseBody body;
    body.append(binary, 4);
    body.append(nullptr, 0);
    body.append(binary + 4, 4);
    const auto received = body.take(sizeof(binary));
    assert(received && *received == std::string(binary, sizeof(binary)));
    assert(!body.take(sizeof(binary))); // Terminal responses cannot be reused.

    HttpResponseBody empty;
    assert(empty.take(0) == std::string{}); // Valid empty 200 body.
    HttpResponseBody missing;
    assert(!missing.take(100)); // No unsafe response.buffer fallback.
    HttpResponseBody partial;
    partial.append(binary, 4);
    assert(!partial.take(8));
    HttpResponseBody extra;
    extra.append(binary, 8);
    assert(!extra.take(4));

    HttpResponseBody canceled;
    canceled.append(binary, 4);
    canceled.discard();
    canceled.append(binary + 4, 4); // Late chunks stay discarded.
    assert(!canceled.take(4));
    assert(!canceled.take(0));

    HttpResponseBody invalid;
    invalid.append(nullptr, 1);
    assert(!invalid.take(0));
    HttpResponseBody tooLarge;
    tooLarge.append(binary, HttpResponseBody::maxBytes + 1); // Must reject before reading.
    assert(!tooLarge.take(0));
    HttpResponseBody totalLimit;
    const std::string first(HttpResponseBody::maxBytes - 1, '\0');
    totalLimit.append(first.data(), first.size());
    totalLimit.append(binary, 2);
    assert(!totalLimit.take(HttpResponseBody::maxBytes + 1));
}
