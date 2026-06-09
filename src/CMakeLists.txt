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

# Include directories
target_include_directories(orbit_lib
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Main executable
add_executable(orbit main.c)
target_link_libraries(orbit PRIVATE orbit_lib ${URING_LIBRARIES})
