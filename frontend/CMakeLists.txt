find_package(SDL2 REQUIRED)
find_package(SDL2_image QUIET)

add_executable(gbemu
  main.cpp
)

target_link_libraries(gbemu
  PRIVATE
    gbemu_core
    gbemu_common
    SDL2::SDL2
)

if (SDL2_image_FOUND)
  target_link_libraries(gbemu PRIVATE SDL2_image::SDL2_image)
  target_compile_definitions(gbemu PRIVATE GBEMU_HAS_SDL_IMAGE)
endif()

target_compile_features(gbemu PRIVATE cxx_std_20)
