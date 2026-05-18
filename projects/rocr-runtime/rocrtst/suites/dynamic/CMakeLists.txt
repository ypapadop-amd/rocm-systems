cmake_minimum_required(VERSION 3.20)

project(dynamic_tests)

include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

find_package(hsa-runtime64 CONFIG REQUIRED NAMES hsa_runtime64 hsa-runtime64)

# memory test
add_executable(memory memory.cc)
set_target_properties(memory
  PROPERTIES
    CXX_STANDARD                17
    CXX_EXTENSIONS              OFF
    CXX_VISIBILITY_PRESET       hidden
    VISIBILITY_INLINES_HIDDEN   ON
    POSITION_INDEPENDENT_CODE   ON
)
target_compile_options(memory PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(memory PRIVATE hsa-runtime64::hsa-runtime64 GTest::gtest_main)

# dispatch test
add_executable(dispatch dispatch.cc)
set_target_properties(dispatch
  PROPERTIES
    CXX_STANDARD                17
    CXX_EXTENSIONS              OFF
    CXX_VISIBILITY_PRESET       hidden
    VISIBILITY_INLINES_HIDDEN   ON
    POSITION_INDEPENDENT_CODE   ON
)
target_compile_options(dispatch PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dispatch PRIVATE hsa-runtime64::hsa-runtime64 GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(memory)
gtest_discover_tests(dispatch)
