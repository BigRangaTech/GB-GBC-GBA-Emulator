add_library(gbemu_common
  common.cpp
)

target_include_directories(gbemu_common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(gbemu_common PUBLIC cxx_std_20)
