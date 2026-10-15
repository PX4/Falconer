find_package(Boost 1.83 REQUIRED CONFIG)
find_package(Threads REQUIRED)

include(FetchContent)
# FTXUI v7.0.3 (MIT), pinned by commit.
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG f921fad208912747c17d129a8ef75ec7624b6eec
    GIT_SHALLOW FALSE
    SYSTEM
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

file(STRINGS "${PROJECT_SOURCE_DIR}/sitl/image" FCTL_SITL_IMAGE LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/sitl/image")

add_executable(fctl
    src/client.cpp
    src/interpreter.cpp
    src/launcher.cpp
    src/main.cpp
    src/output.cpp
    src/sitl.cpp
    src/top.cpp
)
target_compile_definitions(fctl PRIVATE
    FALCONER_VERSION="${PROJECT_VERSION}"
    FCTL_SITL_IMAGE="${FCTL_SITL_IMAGE}"
    FCTL_DEFAULT_BACKEND="$<TARGET_FILE:falconer-backend>"
)
target_compile_options(fctl PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(fctl PRIVATE
    falconer_auth
    falconer_contract
    Boost::headers
    Threads::Threads
    ftxui::component
    ftxui::dom
    ftxui::screen
)
add_dependencies(fctl falconer-backend)
