find_package(MAVSDK REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(Boost 1.83 REQUIRED CONFIG)
# On macOS other frameworks (GStreamer) ship old OpenSSL headers that CMake
# can pick up ahead of Homebrew's. Prefer Homebrew's openssl@3 when present.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
    execute_process(
        COMMAND brew --prefix openssl@3
        OUTPUT_VARIABLE _brew_openssl
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_brew_openssl)
        set(OPENSSL_ROOT_DIR "${_brew_openssl}")
    endif()
endif()
find_package(OpenSSL 3 REQUIRED)
find_package(Threads REQUIRED)

# Contract (generated from proto/)
add_library(falconer_contract STATIC "${PROJECT_SOURCE_DIR}/proto/falconer/v1/contract.proto")
protobuf_generate(
    TARGET falconer_contract
    IMPORT_DIRS "${PROJECT_SOURCE_DIR}/proto"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen"
)
target_include_directories(falconer_contract PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_link_libraries(falconer_contract PUBLIC protobuf::libprotobuf)

# Session authentication, shared with fctl
add_library(falconer_auth STATIC src/auth.cpp)
target_include_directories(falconer_auth PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(falconer_auth PUBLIC OpenSSL::Crypto)

add_executable(falconer-backend
    src/connection_layer.cpp
    src/contract_server.cpp
    src/coordinator.cpp
    src/log.cpp
    src/main.cpp
    src/mavlink_frame.cpp
    src/vehicle_adapter.cpp
)
target_compile_definitions(falconer-backend PRIVATE FALCONER_VERSION="${PROJECT_VERSION}")
target_compile_options(falconer-backend PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(falconer-backend PRIVATE
    falconer_auth
    falconer_contract
    MAVSDK::mavsdk
    Boost::headers
    Threads::Threads
)
