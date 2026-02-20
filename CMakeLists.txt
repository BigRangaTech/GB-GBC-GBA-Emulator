cmake_minimum_required(VERSION 3.20)

project(gbemu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GBEMU_BUILD_TESTS "Build tests" ON)

add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(frontend)

if(GBEMU_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
