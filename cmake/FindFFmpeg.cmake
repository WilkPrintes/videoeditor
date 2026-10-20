# FindFFmpeg
# ----------
# Creates FFmpeg::<component> imported targets (avcodec, avformat, avutil, swscale,
# swresample, avfilter, avdevice).
#
# Search order:
#   1. FFMPEG_ROOT (CMake or environment variable): a prefix with include/, lib/ and,
#      on Windows, bin/*.dll -- e.g. the BtbN "lgpl-shared" zip.
#   2. pkg-config (Homebrew / Linux distributions).
#
# On Windows the targets are SHARED IMPORTED with both the import library and the DLL,
# so $<TARGET_RUNTIME_DLLS:...> can copy the DLLs next to the executable.

set(FFMPEG_ROOT "${FFMPEG_ROOT}" CACHE PATH "FFmpeg prefix with include/, lib/ (and bin/ on Windows)")
if(NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
    set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()

if(NOT FFmpeg_FIND_COMPONENTS)
    set(FFmpeg_FIND_COMPONENTS avcodec avformat avutil swscale swresample)
endif()

if(NOT FFMPEG_ROOT)
    find_package(PkgConfig QUIET)
endif()

foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(TARGET FFmpeg::${_comp})
        set(FFmpeg_${_comp}_FOUND TRUE)
        continue()
    endif()

    if(FFMPEG_ROOT)
        find_path(FFmpeg_${_comp}_INCLUDE_DIR "lib${_comp}/${_comp}.h"
            PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
        find_library(FFmpeg_${_comp}_LIBRARY NAMES ${_comp}
            PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
        set(_dll "")
        if(WIN32)
            file(GLOB _dll "${FFMPEG_ROOT}/bin/${_comp}-*.dll")
        endif()

        if(FFmpeg_${_comp}_INCLUDE_DIR AND FFmpeg_${_comp}_LIBRARY)
            if(_dll)
                list(GET _dll 0 _dll)
                add_library(FFmpeg::${_comp} SHARED IMPORTED)
                set_target_properties(FFmpeg::${_comp} PROPERTIES
                    IMPORTED_IMPLIB "${FFmpeg_${_comp}_LIBRARY}"
                    IMPORTED_LOCATION "${_dll}"
                    INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${_comp}_INCLUDE_DIR}")
            else()
                add_library(FFmpeg::${_comp} UNKNOWN IMPORTED)
                set_target_properties(FFmpeg::${_comp} PROPERTIES
                    IMPORTED_LOCATION "${FFmpeg_${_comp}_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${_comp}_INCLUDE_DIR}")
            endif()
            set(FFmpeg_${_comp}_FOUND TRUE)
        endif()
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(PC_FFmpeg_${_comp} QUIET IMPORTED_TARGET lib${_comp})
        if(PC_FFmpeg_${_comp}_FOUND)
            add_library(FFmpeg::${_comp} INTERFACE IMPORTED)
            target_link_libraries(FFmpeg::${_comp} INTERFACE PkgConfig::PC_FFmpeg_${_comp})
            set(FFmpeg_${_comp}_FOUND TRUE)
            set(FFmpeg_${_comp}_VERSION "${PC_FFmpeg_${_comp}_VERSION}")
        endif()
    endif()
endforeach()

set(_ffmpeg_required_var FFmpeg_avutil_FOUND)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS ${_ffmpeg_required_var}
    HANDLE_COMPONENTS)
