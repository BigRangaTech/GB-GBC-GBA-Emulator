find_package(SDL2 REQUIRED)
find_package(SDL2_image QUIET)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)

add_executable(gbemu
  main.cpp
  vk_frontend.cpp
  vulkan_renderer.cpp
)

target_link_libraries(gbemu
  PRIVATE
    gbemu_core
    gbemu_common
    SDL2::SDL2
    Vulkan::Vulkan
    Threads::Threads
)

if (SDL2_image_FOUND)
  target_link_libraries(gbemu PRIVATE SDL2_image::SDL2_image)
  target_compile_definitions(gbemu PRIVATE GBEMU_HAS_SDL_IMAGE)
endif()

target_compile_features(gbemu PRIVATE cxx_std_20)

add_executable(gbemu_vk
  vk_main.cpp
  vk_frontend.cpp
  vulkan_renderer.cpp
)

target_link_libraries(gbemu_vk
  PRIVATE
    gbemu_core
    gbemu_common
    SDL2::SDL2
    Vulkan::Vulkan
    Threads::Threads
)

target_compile_features(gbemu_vk PRIVATE cxx_std_20)
