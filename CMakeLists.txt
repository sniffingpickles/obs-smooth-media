cmake_minimum_required(VERSION 3.16)

project(obs-smooth-media VERSION 1.0.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find OBS libraries
# For Windows: set OBS_DIR to the OBS Studio install or SDK path
# e.g. cmake -DOBS_DIR="C:/obs-studio" ..
if(WIN32)
    if(NOT OBS_DIR)
        # Default: look for portable OBS in common locations
        set(OBS_DIR "C:/obs-studio" CACHE PATH "OBS Studio install directory")
    endif()

    # OBS headers
    if(EXISTS "${OBS_DIR}/include")
        set(OBS_INCLUDE_DIR "${OBS_DIR}/include")
    elseif(EXISTS "${OBS_DIR}/libobs")
        set(OBS_INCLUDE_DIR "${OBS_DIR}")
    endif()

    # OBS libraries
    find_library(OBS_LIB obs PATHS "${OBS_DIR}/lib" "${OBS_DIR}/bin/64bit" "${OBS_DIR}/build_x64/libobs/Release")
    find_library(OBS_FRONTEND_LIB obs-frontend-api
        PATHS "${OBS_DIR}/lib" "${OBS_DIR}/bin/64bit" "${OBS_DIR}/build_x64/UI/obs-frontend-api/Release")
else()
    # Linux/macOS: use pkg-config or find_package
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(OBS QUIET libobs)
    endif()
    if(NOT OBS_FOUND)
        find_path(OBS_INCLUDE_DIR obs-module.h
            PATHS /usr/include/obs /usr/local/include/obs
                  "${OBS_DIR}/include" "${OBS_DIR}/libobs")
        find_library(OBS_LIB obs PATHS /usr/lib /usr/local/lib "${OBS_DIR}/lib")
    endif()
endif()

# Find FFmpeg
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(AVFORMAT QUIET libavformat)
    pkg_check_modules(AVCODEC QUIET libavcodec)
    pkg_check_modules(AVUTIL QUIET libavutil)
    pkg_check_modules(SWRESAMPLE QUIET libswresample)
    pkg_check_modules(SWSCALE QUIET libswscale)
    pkg_check_modules(AVDEVICE QUIET libavdevice)
endif()

if(NOT AVFORMAT_FOUND)
    # Manual FFmpeg search for Windows
    if(NOT FFMPEG_DIR)
        set(FFMPEG_DIR "" CACHE PATH "FFmpeg directory (contains include/ and lib/)")
    endif()

    find_path(AVFORMAT_INCLUDE_DIRS libavformat/avformat.h
        PATHS "${FFMPEG_DIR}/include" "${OBS_DIR}/include")
    find_library(AVFORMAT_LINK avformat
        PATHS "${FFMPEG_DIR}/lib" "${FFMPEG_DIR}/bin" "${OBS_DIR}/bin/64bit")
    find_library(AVCODEC_LINK avcodec
        PATHS "${FFMPEG_DIR}/lib" "${FFMPEG_DIR}/bin" "${OBS_DIR}/bin/64bit")
    find_library(AVUTIL_LINK avutil
        PATHS "${FFMPEG_DIR}/lib" "${FFMPEG_DIR}/bin" "${OBS_DIR}/bin/64bit")
    find_library(SWRESAMPLE_LINK swresample
        PATHS "${FFMPEG_DIR}/lib" "${FFMPEG_DIR}/bin" "${OBS_DIR}/bin/64bit")
    find_library(SWSCALE_LINK swscale
        PATHS "${FFMPEG_DIR}/lib" "${FFMPEG_DIR}/bin" "${OBS_DIR}/bin/64bit")
    find_library(AVDEVICE_LINK avdevice
        PATHS "${FFMPEG_DIR}/lib" "${FFMPEG_DIR}/bin" "${OBS_DIR}/bin/64bit")

    set(AVFORMAT_LIBRARIES ${AVFORMAT_LINK})
    set(AVCODEC_LIBRARIES ${AVCODEC_LINK})
    set(AVUTIL_LIBRARIES ${AVUTIL_LINK})
    set(SWRESAMPLE_LIBRARIES ${SWRESAMPLE_LINK})
    set(SWSCALE_LIBRARIES ${SWSCALE_LINK})
    set(AVDEVICE_LIBRARIES ${AVDEVICE_LINK})
    set(AVFORMAT_INCLUDE_DIRS ${AVFORMAT_INCLUDE_DIRS})
endif()

# Plugin sources
set(PLUGIN_SOURCES
    src/plugin-main.c
    src/smooth-media-source.c
    src/stream-decoder.c
    src/audio-buffer.c
    src/clock-tracker.c
)

set(PLUGIN_HEADERS
    src/smooth-media-source.h
    src/stream-decoder.h
    src/audio-buffer.h
    src/clock-tracker.h
)

add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${OBS_INCLUDE_DIR}
    ${OBS_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OBS_LIB}
    ${OBS_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${AVDEVICE_LIBRARIES}
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
    # Remove 'lib' prefix on Windows
    set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
endif()

# Install to OBS plugin directory
if(WIN32)
    set(OBS_PLUGIN_DIR "${OBS_DIR}/obs-plugins/64bit" CACHE PATH "OBS plugin install directory")
    set(OBS_DATA_DIR "${OBS_DIR}/data/obs-plugins/${PROJECT_NAME}" CACHE PATH "OBS plugin data directory")
else()
    set(OBS_PLUGIN_DIR "$ENV{HOME}/.config/obs-studio/plugins/${PROJECT_NAME}/bin/64bit" CACHE PATH "OBS plugin install directory")
    set(OBS_DATA_DIR "$ENV{HOME}/.config/obs-studio/plugins/${PROJECT_NAME}/data" CACHE PATH "OBS plugin data directory")
endif()

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION "${OBS_PLUGIN_DIR}")
install(DIRECTORY data/ DESTINATION "${OBS_DATA_DIR}" OPTIONAL)
