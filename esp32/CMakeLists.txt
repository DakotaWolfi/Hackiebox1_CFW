# ESP32-S3 box: the custom firmware as an
# ESP-IDF app image for the box's own bootloader and partition table.
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

set(CFW_VERSION "0.1.0" CACHE STRING "version, CFW_<major.minor.patch>")
# project name and version of the app description.
set(PROJECT_VER "CFW_${CFW_VERSION}")
# The box's own bootloader stays; only the app image is built and delivered.
set(EXCLUDE_COMPONENTS fatfs)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(toniebox-cfw-esp32)

# the build number is the build date and time of the app
# description; the image gets the one of cfw_build.h after every link.
idf_build_get_property(python PYTHON)
add_custom_command(TARGET gen_project_binary POST_BUILD
    COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/../tools/esp32img.py stamp
            ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin ${CMAKE_BINARY_DIR}/cfw_build.h
    VERBATIM)
