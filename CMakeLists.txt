cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(BrewLink)

# Framework version constraint: ESP-IDF v6.1 is required
if(DEFINED IDF_VERSION_MAJOR AND DEFINED IDF_VERSION_MINOR)
    if(IDF_VERSION_MAJOR LESS 6 OR (IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR LESS 1))
        message(WARNING "BrewLink requires ESP-IDF v6.1 or newer. Detected ESP-IDF version: ${IDF_VER}")
    endif()
endif()
