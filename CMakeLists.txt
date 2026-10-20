cmake_minimum_required(VERSION 3.25)

project(VideoEditor
    VERSION 0.1.0
    DESCRIPTION "Native non-linear video editor (Qt 6 + FFmpeg) - case study"
    LANGUAGES C CXX)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW) # FFMPEG_ROOT is a search root
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

option(VE_BUILD_APP "Build the Qt Widgets application" ON)
option(VE_BUILD_CLI "Build ve-cli (probe / export / playback self-tests without UI)" ON)
option(VE_BUILD_TESTS "Build the tests" ON)
option(VE_DEPLOY_ON_BUILD "Copy the Qt/FFmpeg runtime next to the executables after each build (Windows)" ON)

if(MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
        /utf-8)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra>)
endif()

find_package(Threads REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
find_package(Qt6 6.7 REQUIRED COMPONENTS Core)
if(VE_BUILD_APP)
    find_package(Qt6 6.7 REQUIRED COMPONENTS Gui Widgets ShaderTools)
    # QRhi (<rhi/qrhi.h>) has limited compatibility guarantees and comes with the GuiPrivate target,
    # which ties the build to this exact Qt version. That is expected here.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
endif()

# Layers (spec §4): core <- media <- audio <- engine <- ui <- app. `core` depends on nothing above it.
set(VE_FFMPEG_LIBS FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample)

# ---------------------------------------------------------------------------------------------
# third_party
# ---------------------------------------------------------------------------------------------
# miniaudio (public domain or MIT-0): audio device output and master clock
add_library(miniaudio STATIC third_party/miniaudio/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC third_party/miniaudio)
target_compile_definitions(miniaudio PUBLIC
    MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION
    MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)
if(APPLE)
    target_link_libraries(miniaudio PUBLIC
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(UNIX)
    target_link_libraries(miniaudio PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------------------------
# core: time (flicks), model, edits, commands/transactions, JSON project files. Pure C++20, no Qt.
# ---------------------------------------------------------------------------------------------
add_library(ve_core STATIC
    core/Id.cpp
    core/Time.cpp
    core/Json.cpp
    core/Model.cpp
    core/Edit.cpp
    core/Commands.cpp
    core/Snapshot.cpp
    core/ProjectFile.cpp
    core/Log.cpp)
target_include_directories(ve_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------------------------
# media: probe, decoders (hardware accelerated), MP4/H.264 writer, test pattern. FFmpeg, no Qt.
# ---------------------------------------------------------------------------------------------
add_library(ve_media STATIC
    media/FFmpegUtil.cpp
    media/VideoFrame.cpp
    media/MediaProbe.cpp
    media/VideoDecoder.cpp
    media/AudioDecoder.cpp
    media/EncoderThread.cpp
    media/Mp4Writer.cpp
    media/TestPattern.cpp)
target_link_libraries(ve_media PUBLIC ve_core Threads::Threads PRIVATE ${VE_FFMPEG_LIBS})
if(WIN32)
    target_link_libraries(ve_media PRIVATE ole32) # COM apartment for Media Foundation encoders
endif()

# ---------------------------------------------------------------------------------------------
# audio: device output (miniaudio) and the timeline mixer. No Qt.
# ---------------------------------------------------------------------------------------------
add_library(ve_audio STATIC
    audio/AudioOutput.cpp
    audio/AudioMixer.cpp)
target_link_libraries(ve_audio PUBLIC ve_media PRIVATE miniaudio)

# ---------------------------------------------------------------------------------------------
# engine: playback (audio master clock, decode-ahead, scrubbing) and export. No Qt.
# ---------------------------------------------------------------------------------------------
add_library(ve_engine STATIC
    engine/PlaybackEngine.cpp
    engine/Exporter.cpp)
target_link_libraries(ve_engine PUBLIC ve_core ve_media ve_audio PRIVATE ${VE_FFMPEG_LIBS})

# ---------------------------------------------------------------------------------------------
# ui + app: Qt Widgets with the QRhi viewer
# ---------------------------------------------------------------------------------------------
if(VE_BUILD_APP)
    # The widgets live in a library so the GUI tests drive exactly what the app ships.
    add_library(ve_ui STATIC
        ui/MainWindow.cpp ui/MainWindow.h
        ui/ViewerWidget.cpp ui/ViewerWidget.h
        ui/TimelineWidget.cpp ui/TimelineWidget.h
        ui/MediaPoolWidget.cpp ui/MediaPoolWidget.h
        ui/ExportDialog.cpp ui/ExportDialog.h
        ui/LabelColors.cpp ui/LabelColors.h
        ui/SourceViewerPanel.cpp ui/SourceViewerPanel.h
        ui/InspectorWidget.cpp ui/InspectorWidget.h
        ui/EditIndexWidget.cpp ui/EditIndexWidget.h)
    set_target_properties(ve_ui PROPERTIES AUTOMOC ON)
    target_link_libraries(ve_ui PUBLIC ve_engine Qt6::Widgets Qt6::GuiPrivate)

    # GLSL 440 -> .qsb (SPIR-V + HLSL + MSL + GLSL), loaded at runtime by QRhi as :/shaders/<name>.qsb
    qt_add_shaders(ve_ui "viewer_shaders"
        PREFIX "/shaders"
        BASE gpu/shaders
        FILES
            gpu/shaders/quad.vert
            gpu/shaders/yuv_to_rgb.frag
            gpu/shaders/display.frag)

    qt_add_executable(videoeditor app/main.cpp)
    set_target_properties(videoeditor PROPERTIES
        WIN32_EXECUTABLE ON
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.videoeditor.app"
        MACOSX_BUNDLE_BUNDLE_NAME "VideoEditor"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    target_link_libraries(videoeditor PRIVATE ve_ui)

    if(WIN32 AND VE_DEPLOY_ON_BUILD)
        add_custom_command(TARGET videoeditor POST_BUILD
            COMMAND Qt6::windeployqt --no-translations --no-compiler-runtime --no-opengl-sw
                    --verbose 0 "$<TARGET_FILE:videoeditor>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:videoeditor> "$<TARGET_FILE_DIR:videoeditor>"
            COMMAND_EXPAND_LISTS
            COMMENT "Deploying Qt and FFmpeg next to videoeditor.exe")
    endif()

    # cmake --install: Qt deploy script (windeployqt / macdeployqt) plus the FFmpeg libraries.
    install(TARGETS videoeditor
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    qt_generate_deploy_app_script(
        TARGET videoeditor
        OUTPUT_SCRIPT ve_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${ve_deploy_script})
    if(WIN32)
        install(FILES $<TARGET_RUNTIME_DLLS:videoeditor> DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# tools/ve-cli: headless probe / export / playback self-tests (Qt Core only for UTF-8 arguments)
# ---------------------------------------------------------------------------------------------
if(VE_BUILD_CLI)
    add_executable(ve-cli tools/ve-cli/main.cpp)
    target_link_libraries(ve-cli PRIVATE ve_engine Qt6::Core)
    if(WIN32 AND VE_DEPLOY_ON_BUILD)
        add_custom_command(TARGET ve-cli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:ve-cli> "$<TARGET_FILE_DIR:ve-cli>"
            COMMAND_EXPAND_LISTS)
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# Tests: Catch2 for the core (no Qt, no FFmpeg), Qt Test for the GUI smoke test
# ---------------------------------------------------------------------------------------------
if(VE_BUILD_TESTS)
    enable_testing()

    add_library(catch2 STATIC third_party/catch2/catch_amalgamated.cpp)
    target_include_directories(catch2 SYSTEM PUBLIC third_party/catch2)

    add_executable(ve_core_tests
        tests/core/test_time.cpp
        tests/core/test_json.cpp
        tests/core/test_edit.cpp
        tests/core/test_three_point.cpp
        tests/core/test_trims.cpp
        tests/core/test_compound.cpp
        tests/core/test_tracks_markers.cpp
        tests/core/test_organization.cpp
        tests/core/test_gaps_and_order.cpp
        tests/core/test_golden.cpp
        tests/core/test_commands.cpp
        tests/core/test_project_file.cpp
        tests/core/test_property.cpp)
    target_link_libraries(ve_core_tests PRIVATE ve_core catch2)
    # Golden states (before/after JSON of every edit); VE_UPDATE_GOLDEN=1 rewrites them.
    target_compile_definitions(ve_core_tests PRIVATE VE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/core/golden")
    add_test(NAME ve_core_tests COMMAND ve_core_tests --order rand --warn NoAssertions)

    if(VE_BUILD_APP)
        find_package(Qt6 REQUIRED COMPONENTS Test)
        qt_add_executable(ve_ui_tests tests/ui/ui_tests.cpp)
        set_target_properties(ve_ui_tests PROPERTIES AUTOMOC ON)
        target_link_libraries(ve_ui_tests PRIVATE ve_ui Qt6::Test)
        add_test(NAME ve_ui_tests COMMAND ve_ui_tests)
        if(WIN32 AND VE_DEPLOY_ON_BUILD)
            add_custom_command(TARGET ve_ui_tests POST_BUILD
                COMMAND Qt6::windeployqt --no-translations --no-compiler-runtime --no-opengl-sw
                        --verbose 0 "$<TARGET_FILE:ve_ui_tests>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:ve_ui_tests> "$<TARGET_FILE_DIR:ve_ui_tests>"
                COMMAND_EXPAND_LISTS)
        endif()
    endif()
endif()
