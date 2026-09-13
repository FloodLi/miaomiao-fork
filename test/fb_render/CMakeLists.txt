cmake_minimum_required(VERSION 3.16)
project(fb_render_tests C)
enable_testing()
add_executable(fb_render_tests test_fb_render.c ../../main/fb_render.c)
target_include_directories(fb_render_tests PRIVATE include ../epd/include ../image_quant/include ../../main)
target_compile_features(fb_render_tests PRIVATE c_std_11)
target_compile_options(fb_render_tests PRIVATE -Wall -Wextra -Wno-unused-parameter
    -Werror=return-type -Werror=implicit-function-declaration -UNDEBUG -ffunction-sections -fdata-sections
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -g)
target_link_options(fb_render_tests PRIVATE -Wl,--gc-sections -fsanitize=address,undefined -no-pie)
add_test(NAME fb_render COMMAND fb_render_tests)
