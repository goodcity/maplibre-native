cmake_minimum_required(VERSION 3.16)
project(ohos_fence_diagnostics_tests LANGUAGES CXX)

# Standalone host tests; no OHOS runtime, map, GPU or vendor build required.
enable_testing()
find_package(Threads REQUIRED)
foreach(name IN ITEMS fence_diagnostic fence_lifecycle)
    add_executable(test_${name} test_${name}.cpp)
    target_compile_features(test_${name} PRIVATE cxx_std_17)
    target_include_directories(test_${name} PRIVATE ../sample/entry/src/main/cpp)
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(test_${name} PRIVATE Threads::Threads)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
