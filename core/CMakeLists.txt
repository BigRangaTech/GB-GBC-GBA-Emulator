add_library(gbemu_core
  apu.cpp
  cpu.cpp
  core.cpp
  mmu.cpp
  ppu.cpp
  ppu_backend.cpp
)

target_include_directories(gbemu_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gbemu_core
  PUBLIC
    gbemu_common
)

target_compile_features(gbemu_core PUBLIC cxx_std_20)
