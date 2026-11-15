cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{BREWLINK_VERSION} AND NOT "$ENV{BREWLINK_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{BREWLINK_VERSION}")
else()
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --match "v[0-9]*" --abbrev=0
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE _GIT_TAG_VER
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_GIT_TAG_VER MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+.*)$")
            set(PROJECT_VER "${CMAKE_MATCH_1}")
        endif()
    endif()
endif()

if(NOT DEFINED PROJECT_VER OR "${PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "0.2.0")
endif()
string(REGEX REPLACE "^v" "" PROJECT_VER "${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(BrewLink)

# Framework version constraint: ESP-IDF v6.1 is required
if(DEFINED IDF_VERSION_MAJOR AND DEFINED IDF_VERSION_MINOR)
    if(IDF_VERSION_MAJOR LESS 6 OR (IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR LESS 1))
        message(WARNING "BrewLink requires ESP-IDF v6.1 or newer. Detected ESP-IDF version: ${IDF_VER}")
    endif()
endif()
