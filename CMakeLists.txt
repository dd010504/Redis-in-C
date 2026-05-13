cmake_minimum_required(VERSION 3.16)
project(redis-in-c C)

# C11 strict mode; required for designated initializers, _Static_assert, etc.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Emit compile_commands.json next to the build/ directory so clangd, ccls,
# and other LSP tooling can resolve includes and flags without guessing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a Debug-friendly build with optimizations on; override with
# -DCMAKE_BUILD_TYPE=Release at configure time when benchmarking.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Treat unused/uninitialized/shadowed variables as warnings everywhere.
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wconversion
    -Wsign-conversion
)

add_executable(redis-in-c
    src/main.c
    src/hashtable.c
    src/networking.c
)

target_include_directories(redis-in-c PRIVATE src)
