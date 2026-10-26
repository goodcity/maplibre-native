# OHOS fence diagnostic host tests

Run from the repository root, with build output outside the checkout:

```sh
cmake -S platform/ohos/test -B /tmp/maplibre-ohos-fence-tests
cmake --build /tmp/maplibre-ohos-fence-tests
ctest --test-dir /tmp/maplibre-ohos-fence-tests --output-on-failure
```

Assertions remain enabled in every configuration. These tests cover descriptor
classification, temporary duplicate cleanup, query failures and numeric status
aggregation, plus recording epochs, bounded storage and concurrent lifecycle
ordering. They do not establish device ioctl support, GPU completion, fence
ownership or resource stability; those require device evidence.
