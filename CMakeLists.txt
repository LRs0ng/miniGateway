cmake_minimum_required(VERSION 3.20)

project(miniGateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(JSON_BuildTests OFF CACHE BOOL "Disable nlohmann/json tests for the example" FORCE)
set(JSON_Install OFF CACHE BOOL "Disable nlohmann/json installation for the example" FORCE)
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json"
    "${CMAKE_CURRENT_BINARY_DIR}/nlohmann_json"
    EXCLUDE_FROM_ALL)

add_library(gateway_core
    src/core/acquisition.cpp
    src/core/model.cpp
    src/core/config.cpp
    src/core/normalizer.cpp
    src/core/processing.cpp
    src/core/runtime.cpp
    src/core/scheduler.cpp
    src/core/simulator.cpp
)

target_include_directories(gateway_core PUBLIC include)
target_link_libraries(gateway_core PUBLIC Threads::Threads)
target_link_libraries(gateway_core PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(gateway_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(gateway_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(miniGateway src/main.cpp)
target_link_libraries(miniGateway PRIVATE gateway_core)
target_link_libraries(gateway_core PRIVATE nlohmann_json::nlohmann_json)

enable_testing()
add_executable(gateway_tests tests/test_main.cpp)
target_link_libraries(gateway_tests PRIVATE gateway_core)
add_test(NAME gateway_tests COMMAND gateway_tests)
