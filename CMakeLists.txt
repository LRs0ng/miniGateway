cmake_minimum_required(VERSION 3.20)

project(miniGateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(gateway_core
    src/core/acquisition.cpp
    src/core/model.cpp
    src/core/normalizer.cpp
    src/core/processing.cpp
    src/core/runtime.cpp
    src/core/scheduler.cpp
    src/core/simulator.cpp
)

target_include_directories(gateway_core PUBLIC include)
target_link_libraries(gateway_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(gateway_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(gateway_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(miniGateway src/main.cpp)
target_link_libraries(miniGateway PRIVATE gateway_core)

enable_testing()
add_executable(gateway_tests tests/test_main.cpp)
target_link_libraries(gateway_tests PRIVATE gateway_core)
add_test(NAME gateway_tests COMMAND gateway_tests)
