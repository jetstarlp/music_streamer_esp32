# UI demo for the Waveshare ESP32-S3-Touch-AMOLED-1.64: display + ui with fake
# data (tour through all screens, then interactive). Build: idf.py build
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/.."
    "${CMAKE_CURRENT_LIST_DIR}/../../display"
    "${CMAKE_CURRENT_LIST_DIR}/../../board")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(ui_demo)
