# FindFFmpeg.cmake
# Locates the FFmpeg libraries used by the editor and exposes one imported
# target: FFmpeg::FFmpeg (avformat, avcodec, avutil, swscale, swresample).
#
# Search order:
#   1. pkg-config (MSYS2, Linux distros, Homebrew)
#   2. plain find_path/find_library (vcpkg, manual installs via FFMPEG_ROOT)

set(_ve_ff_components avformat avcodec avutil swscale swresample)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_FFMPEG QUIET IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
endif()

if(PC_FFMPEG_FOUND)
    if(NOT TARGET FFmpeg::FFmpeg)
        add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
        target_link_libraries(FFmpeg::FFmpeg INTERFACE PkgConfig::PC_FFMPEG)
    endif()
    set(FFmpeg_FOUND TRUE)
    set(FFmpeg_VERSION "${PC_FFMPEG_libavcodec_VERSION}")
else()
    set(_ve_hints "${FFMPEG_ROOT}" "$ENV{FFMPEG_ROOT}")
    find_path(FFmpeg_INCLUDE_DIR libavcodec/avcodec.h
        HINTS ${_ve_hints} PATH_SUFFIXES include)
    set(FFmpeg_LIBRARIES)
    foreach(c IN LISTS _ve_ff_components)
        find_library(FFmpeg_${c}_LIBRARY NAMES ${c} lib${c}
            HINTS ${_ve_hints} PATH_SUFFIXES lib lib64)
        list(APPEND FFmpeg_LIBRARIES "${FFmpeg_${c}_LIBRARY}")
        list(APPEND _ve_required FFmpeg_${c}_LIBRARY)
    endforeach()

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(FFmpeg
        REQUIRED_VARS FFmpeg_INCLUDE_DIR ${_ve_required})

    if(FFmpeg_FOUND AND NOT TARGET FFmpeg::FFmpeg)
        add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
        target_include_directories(FFmpeg::FFmpeg INTERFACE "${FFmpeg_INCLUDE_DIR}")
        target_link_libraries(FFmpeg::FFmpeg INTERFACE ${FFmpeg_LIBRARIES})
        if(WIN32)
            # Needed when linking a static FFmpeg build.
            target_link_libraries(FFmpeg::FFmpeg INTERFACE bcrypt secur32 ws2_32 mfplat mfuuid strmiids ole32 user32)
        endif()
    endif()
endif()

if(FFmpeg_FOUND)
    message(STATUS "Found FFmpeg ${FFmpeg_VERSION}")
endif()
