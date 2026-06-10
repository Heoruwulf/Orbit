# Gather all source files
file(GLOB_RECURSE SRC_FILES
    "core/*.c"
    "sip/*.c"
    "rtp/*.c"
    "ws/*.c"
)

# Create a static library for the core logic
add_library(orbit_lib STATIC ${SRC_FILES})
target_link_libraries(orbit_lib PUBLIC wslay yyjson)

# Dynamic version header generation target
add_custom_target(generate_version_header
    BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/include/orbit/version.h"
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DBINARY_DIR=${CMAKE_BINARY_DIR}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_SOURCE_DIR}/scripts/generate_version.cmake
    COMMENT "Generating/updating orbit/version.h from git description"
)
add_dependencies(orbit_lib generate_version_header)

# Include directories
target_include_directories(orbit_lib
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Main executable
add_executable(orbit main.c)
target_link_libraries(orbit PRIVATE orbit_lib ${URING_LIBRARIES})
