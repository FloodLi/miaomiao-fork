cmake_minimum_required(VERSION 3.16)
project(image_quant_tests C)
enable_testing()
add_executable(image_quant_tests test_image_quant.c ../../main/lodepng.c)
target_include_directories(image_quant_tests PRIVATE include ../../main)
target_compile_features(image_quant_tests PRIVATE c_std_11)
target_compile_options(image_quant_tests PRIVATE -Wall -Wextra -Werror=return-type
    -Werror=implicit-function-declaration -UNDEBUG -ffunction-sections -fdata-sections)
target_link_options(image_quant_tests PRIVATE -Wl,--gc-sections)
option(IMAGE_QUANT_SANITIZE "Enable address and undefined behavior sanitizers" ON)
if(IMAGE_QUANT_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(image_quant_tests PRIVATE -fsanitize=address,undefined
        -fno-omit-frame-pointer -fno-pie -g)
    target_link_options(image_quant_tests PRIVATE -fsanitize=address,undefined -no-pie)
endif()
add_test(NAME image_quant COMMAND image_quant_tests)
set_tests_properties(image_quant PROPERTIES ENVIRONMENT
    "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
