cmake_minimum_required(VERSION 3.20)

# Exclusively use clang
set(CMAKE_C_COMPILER "clang")

project(orbit
    VERSION 1.0.0
    DESCRIPTION "Zero-allocation, io_uring based SIP/RTP to Websocket bridge"
    LANGUAGES C
)

# Enforce C23 standard manually to avoid -std=c2x mapping in older CMake
# Strict ISO C is disabled since we need GNU extensions like _GNU_SOURCE for io_uring

# Export compile commands for VS Code IntelliSense/clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict compiler warnings
add_compile_options(
    -std=gnu23
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow
    -Wconversion
    -Wno-unused-parameter
)

add_compile_definitions(LOG_DEBUG)
add_compile_definitions(_GNU_SOURCE)

# Find Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED liburing)

option(ENABLE_EVENT_REDIS "Enable Redis event provider" ON)
option(ENABLE_EVENT_KAFKA "Enable Kafka event provider" ON)
option(ENABLE_EVENT_UDP "Enable UDP event provider" ON)

if(ENABLE_EVENT_UDP)
    add_compile_definitions(ORBIT_WITH_UDP)
endif()

if(ENABLE_EVENT_REDIS)
    pkg_check_modules(HIREDIS hiredis)
    if(HIREDIS_FOUND)
        add_compile_definitions(ORBIT_WITH_REDIS)
    else()
        message(STATUS "hiredis not found. Redis event provider disabled.")
        set(ENABLE_EVENT_REDIS OFF CACHE BOOL "Enable Redis event provider" FORCE)
    endif()
endif()

if(ENABLE_EVENT_KAFKA)
    pkg_check_modules(RDKAFKA rdkafka)
    if(RDKAFKA_FOUND)
        add_compile_definitions(ORBIT_WITH_KAFKA)
    else()
        message(STATUS "rdkafka not found. Kafka event provider disabled.")
        set(ENABLE_EVENT_KAFKA OFF CACHE BOOL "Enable Kafka event provider" FORCE)
    endif()
endif()

include(FetchContent)
set(WSLAY_MAKE_DOCS OFF CACHE BOOL "Disable wslay docs" FORCE)
set(WSLAY_EXAMPLES OFF CACHE BOOL "Disable wslay examples" FORCE)
set(WSLAY_TESTS OFF CACHE BOOL "Disable wslay tests" FORCE)
FetchContent_Declare(
    wslay
    GIT_REPOSITORY https://github.com/tatsuhiro-t/wslay.git
    GIT_TAG master
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/wslay
)
FetchContent_MakeAvailable(wslay)

set(YYJSON_BUILD_TESTS OFF CACHE BOOL "Disable yyjson tests" FORCE)
FetchContent_Declare(
    yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG 0.12.0
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson
)
FetchContent_MakeAvailable(yyjson)

# Enable testing
enable_testing()

# Subdirectories
add_subdirectory(src)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_subdirectory(tests)
endif()
