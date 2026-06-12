# Gather test files
set(TEST_SRCS
    test_core.c
    test_sip.c
    test_rtp.c
    test_ws.c
    test_event.c
    test_config_audio.c
    test_audio_resampler.c
    test_audio_vad.c
    test_audio_transcode.c
)

foreach(test_src IN LISTS TEST_SRCS)
    # Get test name without extension
    get_filename_component(test_name ${test_src} NAME_WE)
    
    add_executable(${test_name} ${test_src})
    
    # Link against our static library
    target_link_libraries(${test_name} PRIVATE orbit_lib wslay pthread ${URING_LIBRARIES})
    
    # Ensure assertions are enabled in tests even in Release mode to prevent unused variable errors
    target_compile_options(${test_name} PRIVATE "-UNDEBUG")
    
    if("${test_name}" STREQUAL "test_core" OR "${test_name}" STREQUAL "test_event")
        # 🚨 THE ENFORCER: Wrap malloc and free
        target_link_options(${test_name} PRIVATE
            "-Wl,--wrap=malloc"
            "-Wl,--wrap=free"
            "-Wl,--wrap=calloc"
            "-Wl,--wrap=realloc"
        )
    endif()

    if("${test_name}" STREQUAL "test_sip")
        target_link_options(${test_name} PRIVATE "-Wl,--wrap=server_is_draining")
    endif()
    
    # Register with CTest
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
