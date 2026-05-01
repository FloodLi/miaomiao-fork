cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.3.1")

# Fresh source archives do not include the local sdkconfig file. Without an
# explicit target, ESP-IDF defaults to esp32 and then S3-only GPIO/USB headers
# fail later during compilation.
if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epaper_uploader)
