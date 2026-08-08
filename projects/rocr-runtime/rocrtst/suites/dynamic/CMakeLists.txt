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

include(GoogleTest)

# Declare a dynamic-driver test executable built from <name>.cc.
function(add_dynamic_test name)
  add_executable(${name} ${name}.cc)
  set_target_properties(${name}
    PROPERTIES
      CXX_STANDARD                17
      CXX_EXTENSIONS              OFF
      CXX_VISIBILITY_PRESET       hidden
      VISIBILITY_INLINES_HIDDEN   ON
      POSITION_INDEPENDENT_CODE   ON
  )
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(${name} PRIVATE hsa-runtime64::hsa-runtime64 GTest::gtest_main)
  gtest_discover_tests(${name})
endfunction()

add_dynamic_test(memory)
add_dynamic_test(dispatch)
