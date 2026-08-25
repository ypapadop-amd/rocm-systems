add_library(dynaccel SHARED dynaccel.c)
set_target_properties(dynaccel
  PROPERTIES
    C_STANDARD                11
    C_EXTENSIONS              ON      # _GNU_SOURCE, memfd_create
    POSITION_INDEPENDENT_CODE ON
)
target_compile_options(dynaccel PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(dynaccel PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${ROCR_SOURCE_DIR}/runtime/hsa-runtime      # core/driver/dynamic/rocr_dynamic_driver.h
  ${ROCR_SOURCE_DIR}/runtime/hsa-runtime/inc  # hsa.h, hsa_ext_amd.h; amd_hsa_signal.h needed starting Task 6
  ${ROCR_SOURCE_DIR}/libhsakmt/include        # hsakmt/hsakmttypes.h, pulled in by rocr_dynamic_driver.h
)
target_link_libraries(dynaccel PRIVATE Threads::Threads)
