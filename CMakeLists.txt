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
# .git/HEAD changes on checkout, but a commit on the current branch only
# rewrites that branch's ref file -- depend on both, or the panel keeps showing
# the previous commit until something else forces a reconfigure.
set(STICK_S3_VER_DEPS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD")
    list(APPEND STICK_S3_VER_DEPS "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD" STICK_S3_GIT_HEAD)
    string(STRIP "${STICK_S3_GIT_HEAD}" STICK_S3_GIT_HEAD)
    # A packed ref has no file of its own; nothing to watch, and listing a
    # missing file would reconfigure on every build.
    if(STICK_S3_GIT_HEAD MATCHES "^ref: (.+)$"
       AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git/${CMAKE_MATCH_1}")
        list(APPEND STICK_S3_VER_DEPS "${CMAKE_CURRENT_LIST_DIR}/.git/${CMAKE_MATCH_1}")
    endif()
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${STICK_S3_VER_DEPS})

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

# The panel string and the number an OTA provider compares against come from two
# different places -- version.txt and CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER --
# and if they drift, OTA fails silently or offers the wrong image. Encode one
# from the other and refuse to build on a mismatch.
if(STICK_S3_RELEASE MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    math(EXPR STICK_S3_EXPECTED_VER
         "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2} * 100 + ${CMAKE_MATCH_3}")
    if(NOT "${CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER}" STREQUAL "${STICK_S3_EXPECTED_VER}")
        message(FATAL_ERROR
            "version.txt says ${STICK_S3_RELEASE} (software version "
            "${STICK_S3_EXPECTED_VER}) but CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER "
            "is ${CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER}. Set it in "
            "sdkconfig.defaults and run 'rm sdkconfig && idf.py build' -- "
            "sdkconfig.defaults only seeds a fresh sdkconfig.")
    endif()
else()
    message(WARNING "version.txt (${STICK_S3_RELEASE}) is not vMAJOR.MINOR.PATCH; "
                    "cannot cross-check CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER")
endif()

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
