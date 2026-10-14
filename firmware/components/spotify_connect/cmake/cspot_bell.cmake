# Builds cspot and bell (from the git submodule) as two static libraries, "cspot"
# and "bell", inside the spotify_connect component.
#
# Why not add_subdirectory(third_party/cspot/cspot) like squeezelite-esp32 does?
#  1. Upstream runs the nanopb generator at build time. That needs protoc and
#     the Python "protobuf" package in the ESP-IDF Python environment, and the
#     bundled generator (nanopb 0.4.6-dev) only works with protobuf 3.20.x and
#     setuptools < 81 - a fragile setup, especially on Windows. We compile the
#     checked-in output in generated/protobuf instead (tools/gen_protos.py).
#  2. Upstream bell hard-wires idf::driver and the registry name
#     idf::espressif__mdns and globs sinks/codecs we do not want.
#  3. No patch to the build files of either repository is needed.
#
# The source lists below are globs over the submodule, so moving the submodule
# to a newer commit normally needs no change here. The feature selection mirrors
# squeezelite-esp32's components/spotify/CMakeLists.txt:
#     BELL_DISABLE_CODECS, BELL_DISABLE_SINKS, BELL_DISABLE_FMT,
#     BELL_DISABLE_REGEX, BELL_ONLY_CJSON, BELL_DISABLE_MQTT,
#     BELL_DISABLE_WEBSERVER, cJSON from ESP-IDF, Vorbis = bundled tremor.
#
# Expects CSPOT_DIR and BELL_DIR to be set, and COMPONENT_LIB to exist.

set(_generated_dir "${CMAKE_CURRENT_LIST_DIR}/../generated")

# --- make sure the checked-in protobuf code matches the submodule -------------
function(_sc_check_generated_protos)
    set(manifest "${_generated_dir}/protobuf/SOURCES.sha256")
    if(NOT EXISTS "${manifest}")
        message(FATAL_ERROR "spotify_connect: ${manifest} is missing, run tools/gen_protos.py")
    endif()
    file(STRINGS "${manifest}" lines REGEX "^[0-9a-f]+  ")
    file(GLOB inputs RELATIVE "${CSPOT_DIR}/protobuf" "${CSPOT_DIR}/protobuf/*.proto" "${CSPOT_DIR}/protobuf/*.options")
    list(LENGTH lines n_manifest)
    list(LENGTH inputs n_inputs)
    set(stale FALSE)
    if(NOT n_manifest EQUAL n_inputs)
        set(stale TRUE)
    endif()
    foreach(line IN LISTS lines)
        string(REGEX MATCH "^([0-9a-f]+)  (.+)$" _ "${line}")
        set(want "${CMAKE_MATCH_1}")
        set(name "${CMAKE_MATCH_2}")
        if(NOT EXISTS "${CSPOT_DIR}/protobuf/${name}")
            set(stale TRUE)
            continue()
        endif()
        # hash with CRLF folded to LF so a Windows checkout with autocrlf matches
        file(READ "${CSPOT_DIR}/protobuf/${name}" content)
        string(REPLACE "\r\n" "\n" content "${content}")
        string(SHA256 have "${content}")
        if(NOT have STREQUAL want)
            set(stale TRUE)
        endif()
    endforeach()
    if(stale)
        message(FATAL_ERROR
            "spotify_connect: generated/protobuf does not match the .proto/.options files of the "
            "cspot submodule (was the submodule updated?).\n"
            "Regenerate with components/spotify_connect/tools/gen_protos.py (see docs/BUILD.md).")
    endif()
endfunction()
_sc_check_generated_protos()

# --- bell ----------------------------------------------------------------------
file(GLOB _bell_sources
    "${BELL_DIR}/external/nanopb/*.c"
    "${BELL_DIR}/main/utilities/*.cpp" "${BELL_DIR}/main/utilities/*.c"
    "${BELL_DIR}/main/io/*.cpp"        "${BELL_DIR}/main/io/*.c"
    "${BELL_DIR}/main/platform/esp/*.cpp"
    "${BELL_DIR}/external/tremor/*.c"
)
list(REMOVE_ITEM _bell_sources
    # BELL_DISABLE_MQTT / BELL_DISABLE_WEBSERVER / BELL_DISABLE_CODECS
    "${BELL_DIR}/main/io/BellMQTTClient.cpp"
    "${BELL_DIR}/main/io/BellHTTPServer.cpp"
    "${BELL_DIR}/main/io/MGStreamAdapter.cpp"
    "${BELL_DIR}/main/io/EncodedAudioStream.cpp"
    # tremor's demo program
    "${BELL_DIR}/external/tremor/ivorbisfile_example.c"
    # mDNS registration is done directly with the ESP-IDF mdns component in
    # src/sc_zeroconf.cpp; this keeps bell independent of the mdns version.
    "${BELL_DIR}/main/platform/esp/MDNSService.cpp"
)

file(GLOB _cspot_sources "${CSPOT_DIR}/src/*.cpp" "${CSPOT_DIR}/src/*.c")
file(GLOB _cspot_proto_sources "${_generated_dir}/protobuf/*.pb.c")

# The (very few) source-level changes we need, without touching the submodule.
# May replace entries of _bell_sources/_cspot_sources by patched copies.
include("${CMAKE_CURRENT_LIST_DIR}/../patches/source_overlays.cmake")

add_library(bell STATIC ${_bell_sources})

target_include_directories(bell PUBLIC
    "${BELL_DIR}/main/io/include"
    "${BELL_DIR}/main/utilities/include"
    "${BELL_DIR}/main/platform"
    "${BELL_DIR}/main/audio-codec/include"
    "${BELL_DIR}/main/audio-dsp/include"
    "${BELL_DIR}/main/audio-sinks/include"
)
target_include_directories(bell SYSTEM PUBLIC
    "${BELL_DIR}/external/nanopb"
    "${BELL_DIR}/external/tremor"
)
target_compile_definitions(bell PUBLIC
    PB_ENABLE_MALLOC            # nanopb: FT_POINTER fields (set by upstream)
    PB_FIELD_32BIT              # nanopb: set by upstream cspot
    BELL_DISABLE_CODECS
    BELL_DISABLE_FMT
    BELL_DISABLE_REGEX
    BELL_ONLY_CJSON
)
target_compile_definitions(bell PRIVATE USE_DEFAULT_STDLIB=1 TARGET_OS_IPHONE=0)
# Third-party code: do not let its warnings fail or flood our build.
# The C++ standard is whatever ESP-IDF selects (gnu++2b in v5.5); upstream asks
# for C++20, of which that is a superset.
target_compile_options(bell PRIVATE -w)
# ESP-IDF components bell compiles against. PUBLIC: cspot and our glue code
# include bell headers that pull in FreeRTOS/mbedTLS/cJSON headers.
target_link_libraries(bell PUBLIC
    idf::mbedtls idf::pthread idf::lwip idf::freertos idf::json
    idf::esp_system idf::esp_hw_support idf::esp_common idf::esp_timer idf::log
)

# --- cspot ---------------------------------------------------------------------
add_library(cspot STATIC ${_cspot_sources} ${_cspot_proto_sources})
target_include_directories(cspot PUBLIC
    "${CSPOT_DIR}/include"
    "${_generated_dir}"         # "protobuf/xxx.pb.h"
)
target_compile_options(cspot PRIVATE -w)
target_link_libraries(cspot PUBLIC bell)
