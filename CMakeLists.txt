cmake_minimum_required(VERSION 3.24)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FALCONER_VERSION LIMIT_COUNT 1)
project(falconer VERSION ${FALCONER_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FALCONER_SANITIZER "" CACHE STRING "Sanitizer to build with: address, thread, or empty")
if(FALCONER_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(FALCONER_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
elseif(NOT FALCONER_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unknown FALCONER_SANITIZER: ${FALCONER_SANITIZER}")
endif()

add_subdirectory(backend)
add_subdirectory(cli)
