# Music Streamer firmware - top level ESP-IDF project file.
# Target: ESP32-S3 (Waveshare ESP32-S3-Touch-AMOLED-1.64, ESP32-S3R8, 16 MB flash).
# Tested with ESP-IDF v5.5.5. See ../docs/BUILD.md.
cmake_minimum_required(VERSION 3.16)

# Local, untracked overrides (WiFi credentials, Spotify client id/secret) can be
# put into sdkconfig.secrets (see sdkconfig.secrets.example). The file is listed
# in .gitignore and only read when it exists. Note that SDKCONFIG_DEFAULTS is
# only applied when "sdkconfig" is (re)generated, i.e. on a clean tree.
set(_ms_defaults "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secrets")
    list(APPEND _ms_defaults "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secrets")
endif()
if(NOT DEFINED SDKCONFIG_DEFAULTS AND NOT DEFINED ENV{SDKCONFIG_DEFAULTS})
    set(SDKCONFIG_DEFAULTS "${_ms_defaults}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only build "main" and what it (transitively) REQUIRES instead of every
# component shipped with ESP-IDF. Components therefore have to declare their
# dependencies properly in idf_component_register(... REQUIRES/PRIV_REQUIRES).
idf_build_set_property(MINIMAL_BUILD ON)

project(music_streamer)
