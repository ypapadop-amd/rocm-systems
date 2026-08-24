cmake_minimum_required(VERSION 3.20)

project(dynamic_tests)

enable_testing()

set(ROCR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." CACHE PATH
    "Path to the rocr-runtime source tree")
find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

find_package(hsa-runtime64 CONFIG REQUIRED NAMES hsa_runtime64 hsa-runtime64)

include(GoogleTest)

add_subdirectory(dynaccel)

# Declare a dynamic-driver test executable built from <name>.cc, registered twice:
# once with no driver present, once with libdynaccel.so preloaded.
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
  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dynaccel
    ${ROCR_SOURCE_DIR}/runtime/hsa-runtime)  # dynaccel_packet.h's "inc/hsa.h"
  target_link_libraries(${name} PRIVATE hsa-runtime64::hsa-runtime64 GTest::gtest_main)

  add_test(NAME ${name}_nodriver
           COMMAND ${name} --gtest_filter=*NoDynamicDriver*)

  add_test(NAME ${name}_dynaccel
           COMMAND ${name} --gtest_filter=-*NoDynamicDriver*)
  set_tests_properties(${name}_dynaccel PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:dynaccel>;HSA_ENABLE_INTERRUPT=0")
endfunction()

add_dynamic_test(memory)
add_dynamic_test(dispatch)
