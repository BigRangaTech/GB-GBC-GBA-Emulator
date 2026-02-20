add_executable(gbemu_tests
  tests.cpp
)

target_link_libraries(gbemu_tests
  PRIVATE
    gbemu_core
    gbemu_common
)

target_compile_features(gbemu_tests PRIVATE cxx_std_20)

add_test(NAME gbemu_tests COMMAND gbemu_tests)
