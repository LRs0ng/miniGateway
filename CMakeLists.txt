cmake_minimum_required(VERSION 3.20)

project(miniGateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(JSON_BuildTests OFF CACHE BOOL "Disable nlohmann/json tests" FORCE)
set(JSON_Install OFF CACHE BOOL "Disable nlohmann/json installation" FORCE)
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json"
    "${CMAKE_CURRENT_BINARY_DIR}/nlohmann_json"
    EXCLUDE_FROM_ALL)

option(GATEWAY_ENABLE_MQTT
    "Build the Paho MQTTAsync event publisher plugin" OFF)

if(WIN32)
    set(GATEWAY_DEFAULT_CONFIG
        "${CMAKE_CURRENT_SOURCE_DIR}/configs/defconfig_windows.json")
elseif(UNIX AND NOT APPLE)
    set(GATEWAY_DEFAULT_CONFIG
        "${CMAKE_CURRENT_SOURCE_DIR}/configs/defconfig_linux.json")
else()
    message(FATAL_ERROR "Only Windows and Linux are supported")
endif()
set(GATEWAY_CONFIG "${GATEWAY_DEFAULT_CONFIG}"
    CACHE STRING "Gateway JSON configuration copied to the runtime directory")
get_filename_component(GATEWAY_CONFIG_SOURCE
    "${GATEWAY_CONFIG}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${GATEWAY_CONFIG_SOURCE}")
    message(FATAL_ERROR
        "GATEWAY_CONFIG does not exist: ${GATEWAY_CONFIG_SOURCE}")
endif()
message(STATUS "Gateway configuration: ${GATEWAY_CONFIG_SOURCE}")

set(GATEWAY_PAHO_MQTT_C_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mqtt"
    CACHE PATH "Paho MQTT C source root")

function(gateway_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion)
    endif()
endfunction()

function(gateway_shared_target target)
    set_target_properties(${target} PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    if(WIN32)
        # Keep the runtime filename identical to the target name on both MSVC
        # and MinGW. The JSON configuration contains the complete `.dll` name
        # and the loader does not infer a `lib` prefix.
        set_target_properties(${target} PROPERTIES PREFIX "")
    endif()
    if(UNIX AND NOT APPLE)
        # Keep the executable and its startup-loaded plugins relocatable in
        # a build/deployment directory that contains gateway_core.so.
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH_USE_ORIGIN ON
            INSTALL_RPATH "$ORIGIN")
    endif()
    gateway_enable_warnings(${target})
endfunction()

function(gateway_executable_target target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    if(UNIX AND NOT APPLE)
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH_USE_ORIGIN ON
            INSTALL_RPATH "$ORIGIN")
    endif()
    gateway_enable_warnings(${target})
endfunction()

add_library(gateway_core STATIC
    src/core/config.cpp
    src/core/control.cpp
    src/core/model.cpp
    src/core/normalizer.cpp
    src/core/plugin_registry.cpp
    src/core/processing.cpp
    src/core/runtime.cpp
    src/core/scheduler.cpp
)

set_target_properties(gateway_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(gateway_core PUBLIC include)
target_link_libraries(gateway_core PUBLIC Threads::Threads)
target_link_libraries(gateway_core PRIVATE nlohmann_json::nlohmann_json)
if(CMAKE_DL_LIBS)
    target_link_libraries(gateway_core PRIVATE ${CMAKE_DL_LIBS})
endif()
gateway_shared_target(gateway_core)

function(add_gateway_plugin target source)
    add_library(${target} SHARED ${source})
    target_link_libraries(${target}
        PUBLIC gateway_core
        PRIVATE nlohmann_json::nlohmann_json)
    gateway_shared_target(${target})
endfunction()

add_gateway_plugin(gateway_poll_simulator
    tests/simulation/poll_simulator.cpp)
target_include_directories(gateway_poll_simulator PUBLIC tests/simulation)

add_gateway_plugin(gateway_push_simulator
    tests/simulation/push_simulator.cpp)
target_include_directories(gateway_push_simulator PUBLIC tests/simulation)

add_gateway_plugin(gateway_print_event_publisher
    tests/simulation/print_event_publisher.cpp)
target_include_directories(gateway_print_event_publisher PUBLIC tests/simulation)

add_gateway_plugin(gateway_periodic_control_source
    tests/simulation/periodic_control_source.cpp)

add_gateway_plugin(gateway_threshold_processor
    src/processor/threshold_processor.cpp)
target_include_directories(gateway_threshold_processor PRIVATE src/processor)

add_gateway_plugin(gateway_window_average_processor
    src/processor/window_average_processor.cpp)
target_include_directories(gateway_window_average_processor PRIVATE src/processor)

add_gateway_plugin(gateway_inference_processor
    src/processor/inference_processor.cpp)
target_include_directories(gateway_inference_processor PRIVATE src/processor)

if(GATEWAY_ENABLE_MQTT)
    # The gateway itself is C++ only.  Enable the C compiler only when the
    # vendored Paho C sources are part of this build.
    enable_language(C)
    set(_gateway_c_standard_was_set FALSE)
    set(_gateway_c_standard_value "")
    if(DEFINED CMAKE_C_STANDARD)
        set(_gateway_c_standard_was_set TRUE)
        set(_gateway_c_standard_value "${CMAKE_C_STANDARD}")
    endif()
    set(_gateway_c_standard_required_was_set FALSE)
    set(_gateway_c_standard_required_value "")
    if(DEFINED CMAKE_C_STANDARD_REQUIRED)
        set(_gateway_c_standard_required_was_set TRUE)
        set(_gateway_c_standard_required_value "${CMAKE_C_STANDARD_REQUIRED}")
    endif()
    set(_gateway_c_extensions_was_set FALSE)
    set(_gateway_c_extensions_value "")
    if(DEFINED CMAKE_C_EXTENSIONS)
        set(_gateway_c_extensions_was_set TRUE)
        set(_gateway_c_extensions_value "${CMAKE_C_EXTENSIONS}")
    endif()
    set(CMAKE_C_STANDARD 99)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)

    if(NOT EXISTS "${GATEWAY_PAHO_MQTT_C_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "Paho MQTT C source was not found: ${GATEWAY_PAHO_MQTT_C_ROOT}")
    endif()

    set(PAHO_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(PAHO_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(PAHO_WITH_SSL OFF CACHE BOOL "" FORCE)
    set(PAHO_WITH_LIBRESSL OFF CACHE BOOL "" FORCE)
    set(PAHO_WITH_LIBUUID OFF CACHE BOOL "" FORCE)
    set(PAHO_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
    set(PAHO_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(PAHO_BUILD_DEB_PACKAGE OFF CACHE BOOL "" FORCE)
    set(PAHO_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(PAHO_ENABLE_CPACK OFF CACHE BOOL "" FORCE)
    set(PAHO_HIGH_PERFORMANCE ON CACHE BOOL "" FORCE)
    set(PAHO_USE_SELECT OFF CACHE BOOL "" FORCE)
    set(PAHO_NO_TCP_NODELAY OFF CACHE BOOL "" FORCE)
    if(NOT WIN32)
        set(PAHO_WITH_UNIX_SOCKETS OFF CACHE BOOL "" FORCE)
    endif()

    add_subdirectory(
        "${GATEWAY_PAHO_MQTT_C_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/paho_mqtt_c"
        EXCLUDE_FROM_ALL)

    if(_gateway_c_standard_was_set)
        set(CMAKE_C_STANDARD "${_gateway_c_standard_value}")
    else()
        unset(CMAKE_C_STANDARD)
    endif()
    if(_gateway_c_standard_required_was_set)
        set(CMAKE_C_STANDARD_REQUIRED "${_gateway_c_standard_required_value}")
    else()
        unset(CMAKE_C_STANDARD_REQUIRED)
    endif()
    if(_gateway_c_extensions_was_set)
        set(CMAKE_C_EXTENSIONS "${_gateway_c_extensions_value}")
    else()
        unset(CMAKE_C_EXTENSIONS)
    endif()
    unset(_gateway_c_standard_was_set)
    unset(_gateway_c_standard_value)
    unset(_gateway_c_standard_required_was_set)
    unset(_gateway_c_standard_required_value)
    unset(_gateway_c_extensions_was_set)
    unset(_gateway_c_extensions_value)

    # MQTTAsync.c and MQTTAsyncUtils.c are direct sources of this static
    # target. PIC is required when Linux links it into the publisher plugin.
    set_target_properties(paho-mqtt3a-static PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    if(MINGW)
        # Paho 1.3.16 passes DWORD* status storage to legacy int* helpers.
        # Both are 32-bit on Windows; GCC 16 promotes the old diagnostic to
        # an error, so keep it as a third-party warning for this target only.
        target_compile_options(paho-mqtt3a-static PRIVATE
            -Wno-error=incompatible-pointer-types)
    endif()

    add_gateway_plugin(gateway_mqtt_event_publisher
        src/publisher/mqtt/mqtt_event_publisher.cpp)
    target_link_libraries(gateway_mqtt_event_publisher PRIVATE
        eclipse-paho-mqtt-c::paho-mqtt3a-static)
endif()

set(GATEWAY_RUNTIME_PLUGIN_TARGETS
    gateway_poll_simulator
    gateway_push_simulator
    gateway_print_event_publisher
    gateway_periodic_control_source
    gateway_threshold_processor
    gateway_window_average_processor
    gateway_inference_processor)
if(GATEWAY_ENABLE_MQTT)
    list(APPEND GATEWAY_RUNTIME_PLUGIN_TARGETS gateway_mqtt_event_publisher)
endif()

add_executable(miniGateway
    src/main.cpp)
target_link_libraries(miniGateway PRIVATE gateway_core)
add_dependencies(miniGateway ${GATEWAY_RUNTIME_PLUGIN_TARGETS})
gateway_executable_target(miniGateway)

set(GATEWAY_GENERATED_CONFIG
    "${CMAKE_CURRENT_BINARY_DIR}/config.json")
configure_file(
    "${GATEWAY_CONFIG_SOURCE}"
    "${GATEWAY_GENERATED_CONFIG}"
    COPYONLY)

add_custom_command(TARGET miniGateway POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${GATEWAY_GENERATED_CONFIG}"
        "$<TARGET_FILE_DIR:miniGateway>/config.json"
    COMMENT "Copying gateway configuration next to the executable"
    VERBATIM)