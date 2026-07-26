# SolisM5 — DALY-BMS → SOLIS-Wechselrichter-Adapter auf M5Stack Core Basic.
# Build:  source ~/esp/esp-idf/export.sh && idf.py -C firmware build
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Version: version.txt + Git-Commit in Klammern. Landet als PROJECT_VER in der
# App-Description und geht damit an Home Assistant (sw_version) und aufs Display.
file(READ ${CMAKE_CURRENT_LIST_DIR}/version.txt SOLIS_VER_RAW)
string(STRIP "${SOLIS_VER_RAW}" SOLIS_VER)
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE SOLIS_GIT OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(SOLIS_GIT)
    set(PROJECT_VER "${SOLIS_VER} (${SOLIS_GIT})")
else()
    set(PROJECT_VER "${SOLIS_VER}")
endif()

project(solism5)
