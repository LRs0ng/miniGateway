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
        WINDOWS_EXPORT_ALL_SYMBOLS ON)
    gateway_enable_warnings(${target})
endfunction()

add_library(gateway_core SHARED
    src/core/config.cpp
    src/core/model.cpp
    src/core/normalizer.cpp
    src/core/plugin_registry.cpp
    src/core/processing.cpp
    src/core/runtime.cpp
    src/core/scheduler.cpp
)
target_include_directories(gateway_core PUBLIC include)
target_link_libraries(gateway_core PUBLIC Threads::Threads)
target_link_libraries(gateway_core PRIVATE nlohmann_json::nlohmann_json)
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

add_gateway_plugin(gateway_threshold_processor
    src/processor/threshold_processor.cpp)
target_include_directories(gateway_threshold_processor PRIVATE src/processor)

add_gateway_plugin(gateway_window_average_processor
    src/processor/window_average_processor.cpp)
target_include_directories(gateway_window_average_processor PRIVATE src/processor)

add_gateway_plugin(gateway_inference_processor
    src/processor/inference_processor.cpp)
target_include_directories(gateway_inference_processor PRIVATE src/processor)

set(GATEWAY_LINKED_PLUGINS
    gateway_poll_simulator
    gateway_push_simulator
    gateway_print_event_publisher
    gateway_threshold_processor
    gateway_window_average_processor
    gateway_inference_processor)

add_executable(miniGateway
    src/main.cpp
    src/bootstrap/linked_plugins.cpp)
target_link_libraries(miniGateway PRIVATE
    gateway_core
    ${GATEWAY_LINKED_PLUGINS})
gateway_enable_warnings(miniGateway)

add_custom_command(TARGET miniGateway POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
        "$<TARGET_FILE_DIR:miniGateway>/config.json"
    COMMENT "Copying gateway configuration next to the executable"
    VERBATIM)