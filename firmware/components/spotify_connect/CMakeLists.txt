# spotify_connect - Spotify Connect receiver built on cspot + bell.
#
#   include/spotify_connect.h   public C API
#   src/                        our C++ glue (session task, Zeroconf, NVS, events)
#   third_party/cspot           git submodule philippe44/cspot (bell is a nested
#                               submodule at third_party/cspot/cspot/bell)
#   generated/protobuf          checked-in nanopb output for cspot's .proto files
#   cmake/cspot_bell.cmake      builds the third-party code as two static libs
#
# The CMakeLists.txt files of cspot and bell are deliberately NOT used, see
# cmake/cspot_bell.cmake for the reasons.

set(CSPOT_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/cspot/cspot")
set(BELL_DIR  "${CSPOT_DIR}/bell")

# ESP-IDF runs this file twice: once in script mode to collect the component
# requirements, then for real. Only complain about a missing submodule once.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT EXISTS "${CSPOT_DIR}/src/SpircHandler.cpp" OR NOT EXISTS "${BELL_DIR}/external/nanopb/pb.h")
        message(FATAL_ERROR
            "spotify_connect: the cspot/bell sources are missing.\n"
            "Run in the repository root:  git submodule update --init --recursive")
    endif()
endif()

idf_component_register(
    SRCS
        "src/spotify_connect.cpp"
        "src/sc_zeroconf.cpp"
        "src/sc_credentials.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        esp_common                  # esp_err.h in the public header
    PRIV_REQUIRES
        audio_out
        esp_http_server             # Zeroconf "addUser" endpoint
        mdns                        # managed component, see idf_component.yml
        nvs_flash                   # reusable credentials
        json                        # cJSON (also used by cspot/bell)
        mbedtls                     # used by bell (TLS, crypto)
        lwip                        # sockets
        pthread                     # std::thread/mutex, bell::Task
        esp_timer
        esp_hw_support              # esp_mac.h
        esp_system
        freertos
        log
)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/cspot_bell.cmake")
target_link_libraries(${COMPONENT_LIB} PRIVATE cspot)
