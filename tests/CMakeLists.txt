if(WIN32)
    if(NOT W32_PTHREADS_INCLUDE_DIR OR NOT W32_PTHREADS_LIB)
        message(STATUS
            "Skipping native tests: w32-pthreads paths were not provided")
        return()
    endif()
    set(TEST_THREAD_LIBS ${W32_PTHREADS_LIB})
else()
    find_package(Threads REQUIRED)
    set(TEST_THREAD_LIBS Threads::Threads)
endif()

add_executable(core-tests
    test-core.c
    ../src/audio-buffer.c
    ../src/clock-tracker.c
)

target_include_directories(core-tests PRIVATE
    ../src
    stubs
)

target_link_libraries(core-tests PRIVATE ${TEST_THREAD_LIBS})

if(MSVC)
    target_compile_options(core-tests PRIVATE
        /std:c11
        /experimental:c11atomics
        /W4 /WX
    )
    if(W32_PTHREADS_INCLUDE_DIR)
        target_include_directories(core-tests PRIVATE
            ${W32_PTHREADS_INCLUDE_DIR}
        )
    endif()
    if(W32_PTHREADS_LIB)
        target_link_libraries(core-tests PRIVATE ${W32_PTHREADS_LIB})
    endif()
else()
    target_compile_options(core-tests PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
    )
endif()

if(ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(core-tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(core-tests PRIVATE
        -fsanitize=address,undefined
    )
endif()

if(ENABLE_THREAD_SANITIZER AND NOT MSVC)
    target_compile_options(core-tests PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer
    )
    target_link_options(core-tests PRIVATE -fsanitize=thread)
endif()

add_test(NAME core-tests COMMAND core-tests)
set_tests_properties(core-tests PROPERTIES TIMEOUT 60)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME timing-model
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test-timing-model.py
    )
    set_tests_properties(timing-model PROPERTIES TIMEOUT 30)

    add_test(NAME soak-analyzer
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test-analyze-soak.py
    )
    set_tests_properties(soak-analyzer PROPERTIES TIMEOUT 30)
endif()

if(NOT WIN32)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TEST_FFMPEG QUIET IMPORTED_TARGET
            libavformat
            libavcodec
            libavutil
            libavdevice
        )
    endif()
    find_program(TEST_FFMPEG_EXECUTABLE ffmpeg)

    if(TEST_FFMPEG_FOUND AND TEST_FFMPEG_EXECUTABLE)
        add_executable(decoder-tests
            test-decoder.c
            ../src/stream-decoder.c
        )
        target_include_directories(decoder-tests PRIVATE
            ../src
            stubs
        )
        target_link_libraries(decoder-tests PRIVATE
            ${TEST_THREAD_LIBS}
            PkgConfig::TEST_FFMPEG
        )
        target_compile_options(decoder-tests PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
        )

        if(ENABLE_SANITIZERS)
            target_compile_options(decoder-tests PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer
            )
            target_link_options(decoder-tests PRIVATE
                -fsanitize=address,undefined
            )
        endif()

        if(ENABLE_THREAD_SANITIZER)
            target_compile_definitions(decoder-tests PRIVATE
                SMOOTH_MEDIA_TSAN_SERIAL_FFMPEG=1
            )
            target_compile_options(decoder-tests PRIVATE
                -fsanitize=thread -fno-omit-frame-pointer
            )
            target_link_options(decoder-tests PRIVATE -fsanitize=thread)
        endif()

        set(TEST_MEDIA "${CMAKE_CURRENT_BINARY_DIR}/decoder-test.mkv")
        set(TEST_CORRUPT_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/decoder-corrupt-test.ts")
        add_test(NAME generate-decoder-media
            COMMAND ${TEST_FFMPEG_EXECUTABLE}
                -hide_banner
                -loglevel error
                -y
                -f lavfi
                -i testsrc2=size=320x180:rate=30
                -f lavfi
                -i sine=frequency=997:sample_rate=48000
                -t 2
                -c:v mpeg4
                -bf 2
                -c:a aac
                ${TEST_MEDIA}
        )
        set_tests_properties(generate-decoder-media PROPERTIES
            FIXTURES_SETUP decoder_media
            TIMEOUT 30
        )

        # Deliberately damage one second of encoded video, then leave several
        # clean GOPs afterward. The decoder integration test must reach those
        # later frames instead of turning one corrupt packet into a transport
        # disconnect.
        add_test(NAME generate-corrupt-decoder-media
            COMMAND ${TEST_FFMPEG_EXECUTABLE}
                -hide_banner
                -loglevel error
                -y
                -f lavfi
                -i testsrc2=size=320x180:rate=30
                -f lavfi
                -i sine=frequency=997:sample_rate=48000
                -t 6
                -c:v mpeg2video
                -g 30
                -bf 0
                -bsf:v
                "noise=amount=if(between(pts*tb\\,2\\,3)\\,1\\,0)"
                -c:a mp2
                -f mpegts
                ${TEST_CORRUPT_MEDIA}
        )
        set_tests_properties(generate-corrupt-decoder-media PROPERTIES
            FIXTURES_SETUP corrupt_decoder_media
            TIMEOUT 30
        )

        add_test(NAME decoder-tests
            COMMAND decoder-tests ${TEST_MEDIA} ${TEST_CORRUPT_MEDIA}
        )
        set_tests_properties(decoder-tests PROPERTIES
            FIXTURES_REQUIRED "decoder_media;corrupt_decoder_media"
            TIMEOUT 15
        )
    endif()
endif()
