cmake_minimum_required(VERSION 3.16)

if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mosaico_recovery_bridge)

include(cmake/recovery_image.cmake)
