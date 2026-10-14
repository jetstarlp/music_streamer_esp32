# Headless PC simulator for the Music Streamer UI.
#
#   cmake -S firmware/components/ui/simulator -B build-sim && cmake --build build-sim -j
#   ./build-sim/ui_sim docs/ui           # renders all screens + runs touch tests
#
# LVGL: the same v9.3.0 tree as the firmware. Taken from -DLVGL_DIR=..., else
# from a firmware build's managed_components, else cloned from GitHub (tag
# v9.3.0) into the build directory.
cmake_minimum_required(VERSION 3.16)
project(ui_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(UI_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(LVGL_DIR "" CACHE PATH "LVGL v9.3.0 source tree")
if(NOT LVGL_DIR)
    foreach(cand ${UI_DIR}/test_app/managed_components/lvgl ${UI_DIR}/../../managed_components/lvgl)
        if(EXISTS ${cand}/lvgl.h)
            set(LVGL_DIR ${cand})
            break()
        endif()
    endforeach()
endif()
if(NOT LVGL_DIR)
    set(LVGL_DIR ${CMAKE_BINARY_DIR}/lvgl-v9.3.0)
    if(NOT EXISTS ${LVGL_DIR}/lvgl.h)
        message(STATUS "Cloning LVGL v9.3.0 into ${LVGL_DIR}")
        execute_process(COMMAND git clone --quiet --depth 1 --branch v9.3.0
                                https://github.com/lvgl/lvgl.git ${LVGL_DIR}
                        RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "git clone of LVGL failed; pass -DLVGL_DIR=<lvgl v9.3.0>")
        endif()
    endif()
endif()
message(STATUS "LVGL: ${LVGL_DIR}")

file(GLOB_RECURSE LVGL_SRCS ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRCS} ${CMAKE_CURRENT_LIST_DIR}/sim_mem.c)
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w)

file(GLOB UI_SRCS ${UI_DIR}/src/*.c ${UI_DIR}/assets/*.c)
add_library(ui STATIC ${UI_SRCS})
target_include_directories(ui PUBLIC ${UI_DIR}/include PRIVATE ${UI_DIR}/src ${UI_DIR}/assets)
target_link_libraries(ui PUBLIC lvgl)
target_compile_options(ui PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)

find_package(Threads REQUIRED)
add_executable(ui_sim sim_main.c)
target_include_directories(ui_sim PRIVATE ${UI_DIR}/src ${UI_DIR}/assets third_party)
target_link_libraries(ui_sim PRIVATE ui lvgl Threads::Threads m)
target_compile_options(ui_sim PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME ui_sim COMMAND ui_sim ${CMAKE_BINARY_DIR}/shots)
