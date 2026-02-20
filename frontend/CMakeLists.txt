find_package(SDL2 REQUIRED)

add_executable(gbemu
  main.cpp
)

target_link_libraries(gbemu
  PRIVATE
    gbemu_core
    gbemu_common
    SDL2::SDL2
)

target_compile_features(gbemu PRIVATE cxx_std_20)
