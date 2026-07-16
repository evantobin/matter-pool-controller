cmake_minimum_required(VERSION 3.16)

# Set esp-matter SDK path
if(NOT DEFINED ENV{ESP_MATTER_PATH} AND EXISTS "$ENV{HOME}/esp/esp-matter")
    set(ENV{ESP_MATTER_PATH} "$ENV{HOME}/esp/esp-matter")
endif()
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "ESP_MATTER_PATH is not set. export ESP_MATTER_PATH=~/esp/esp-matter")
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# Pigweed environment root (required by gn)
set(ENV{_PW_ACTUAL_ENVIRONMENT_ROOT} "${MATTER_SDK_PATH}/.environment")

# Set device HAL path based on target
if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
    if("${IDF_TARGET}" STREQUAL "esp32s3" OR "${IDF_TARGET}" STREQUAL "")
        set(ENV{ESP_MATTER_DEVICE_PATH} ${ESP_MATTER_PATH}/device_hal/device/esp32s3_devkit_c)
    else()
        message(FATAL_ERROR "Unsupported IDF_TARGET: ${IDF_TARGET}")
    endif()
endif()

# Include IDF project first
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Include esp-matter helpers
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

# All component directories
set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${ESP_MATTER_PATH}/device_hal/device"
    ${extra_components_dirs_append})

project(pool_conductor)

# Compiler flags (required by Matter)
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
