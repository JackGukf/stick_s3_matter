# Matter extended color light for the M5Stack StickS3 (ESP32-S3-PICO-1-N8R8).
#
# Derived from $ESP_MATTER_PATH/examples/light, with the device_hal board layer
# dropped: the StickS3 has no LED, so the light output is the on-board ST7789P3
# LCD (see components/stick_s3_light).
cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Please set ESP_MATTER_PATH to the path of the esp-matter repo")
endif(NOT DEFINED ENV{ESP_MATTER_PATH})

# The build string the panel shows: version and release candidate come from
# version.txt (edit that one line per build), the short commit is appended
# automatically so it never has to be typed and can never be wrong. Reconfigure
# is triggered by either input changing.
#
# This is set before project() so ESP-IDF uses it verbatim rather than falling
# back to `git describe`, which appends "-dirty" on any uncommitted change.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" STICK_S3_RELEASE LIMIT_COUNT 1)
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}" rev-parse --short=7 HEAD
    OUTPUT_VARIABLE STICK_S3_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE STICK_S3_GIT_RESULT)
if(STICK_S3_GIT_RESULT EQUAL 0)
    set(PROJECT_VER "${STICK_S3_RELEASE}-${STICK_S3_COMMIT}")
else()
    set(PROJECT_VER "${STICK_S3_RELEASE}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/version.txt"
    "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD")

set(PROJECT_VER_NUMBER 1)

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# This should be done before using the IDF_TARGET variable.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)

set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components")

project(stick_s3_light)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
