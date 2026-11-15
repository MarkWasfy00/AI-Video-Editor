cmake_minimum_required(VERSION 3.21)

project(VideoEditor
    VERSION 0.1.0
    DESCRIPTION "Basic GPU-accelerated video editor (Qt 6, FFmpeg, OpenGL, Vulkan, SQLite)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(VE_ENABLE_VULKAN "Build the Vulkan preview renderer (needs Vulkan headers + glslc/glslangValidator)" ON)
option(VE_ENABLE_AUDIO  "Build audio playback (needs Qt6 Multimedia)" ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network)
find_package(FFmpeg REQUIRED)
find_package(SQLite3 REQUIRED)

if(VE_ENABLE_AUDIO)
    find_package(Qt6 QUIET COMPONENTS Multimedia)
    if(NOT Qt6Multimedia_FOUND)
        message(WARNING "Qt6 Multimedia not found - audio playback disabled (export still has audio)")
        set(VE_ENABLE_AUDIO OFF)
    endif()
endif()

set(VE_VULKAN_ACTIVE OFF)
if(VE_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    find_program(VE_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    find_program(VE_GLSLANG NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    if(DEFINED QT_FEATURE_vulkan AND NOT QT_FEATURE_vulkan)
        message(WARNING "This Qt build has no Vulkan support - Vulkan renderer disabled")
    elseif(NOT Vulkan_INCLUDE_DIRS AND NOT TARGET Vulkan::Headers)
        message(WARNING "Vulkan headers not found - Vulkan renderer disabled")
    elseif(NOT VE_GLSLC AND NOT VE_GLSLANG)
        message(WARNING "glslc / glslangValidator not found - Vulkan renderer disabled")
    else()
        set(VE_VULKAN_ACTIVE ON)
    endif()
endif()

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(VE_SOURCES
    src/main.cpp

    src/core/Time.h
    src/core/Project.h            src/core/Project.cpp
    src/core/ProjectStore.h       src/core/ProjectStore.cpp
    src/core/Commands.h           src/core/Commands.cpp

    src/media/FFmpegUtil.h
    src/media/MediaProbe.h        src/media/MediaProbe.cpp
    src/media/VideoDecoder.h      src/media/VideoDecoder.cpp
    src/media/AudioDecoder.h      src/media/AudioDecoder.cpp
    src/media/AudioMixer.h        src/media/AudioMixer.cpp
    src/media/FrameSource.h       src/media/FrameSource.cpp
    src/media/Exporter.h          src/media/Exporter.cpp
    src/media/Waveform.h          src/media/Waveform.cpp
    src/media/StemSeparator.h     src/media/StemSeparator.cpp
    src/render/CaptionRenderer.h  src/render/CaptionRenderer.cpp
    src/media/HwAccel.h           src/media/HwAccel.cpp

    src/render/Composite.h        src/render/Composite.cpp
    src/render/PreviewView.h
    src/render/PreviewEngine.h    src/render/PreviewEngine.cpp
    src/render/GLCompositor.h     src/render/GLCompositor.cpp
    src/render/GLPreviewWidget.h  src/render/GLPreviewWidget.cpp
    src/render/OffscreenRenderer.h src/render/OffscreenRenderer.cpp
    src/render/GpuInfo.h          src/render/GpuInfo.cpp

    src/api/EditorApi.h           src/api/EditorApi.cpp
    src/api/McpServer.h           src/api/McpServer.cpp
    src/api/McpTransports.h       src/api/McpTransports.cpp

    src/app/MainWindow.h          src/app/MainWindow.cpp
    src/app/MediaBin.h            src/app/MediaBin.cpp
    src/app/TimelineWidget.h      src/app/TimelineWidget.cpp
    src/app/Inspector.h           src/app/Inspector.cpp
    src/app/ExportDialog.h        src/app/ExportDialog.cpp
    src/app/PlaybackController.h  src/app/PlaybackController.cpp
    src/app/GpuSettingsDialog.h   src/app/GpuSettingsDialog.cpp
    src/app/MixerPanel.h          src/app/MixerPanel.cpp
    src/app/ClaudePanel.h         src/app/ClaudePanel.cpp
    src/app/TranscriptPanel.h     src/app/TranscriptPanel.cpp
    src/app/CaptionPanel.h        src/app/CaptionPanel.cpp
)

if(VE_ENABLE_AUDIO)
    list(APPEND VE_SOURCES src/audio/AudioPlayer.h src/audio/AudioPlayer.cpp)
endif()

if(VE_VULKAN_ACTIVE)
    list(APPEND VE_SOURCES src/render/VulkanPreview.h src/render/VulkanPreview.cpp)
endif()

list(REMOVE_ITEM VE_SOURCES src/main.cpp)

# All editor code lives in a static library shared by the app and the tests.
add_library(ve_lib STATIC ${VE_SOURCES})
target_compile_definitions(ve_lib PRIVATE VE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

qt_add_resources(ve_lib "gl_shaders"
    PREFIX "/"
    FILES
        shaders/gl/composite.vert
        shaders/gl/composite.frag
)

target_include_directories(ve_lib PUBLIC src)

target_link_libraries(ve_lib PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Network
    FFmpeg::FFmpeg
)
# CMake >= 4.1 renamed the imported SQLite target.
if(TARGET SQLite3::SQLite3)
    target_link_libraries(ve_lib PUBLIC SQLite3::SQLite3)
else()
    target_link_libraries(ve_lib PUBLIC SQLite::SQLite3)
endif()

if(VE_ENABLE_AUDIO)
    target_link_libraries(ve_lib PUBLIC Qt6::Multimedia)
    target_compile_definitions(ve_lib PUBLIC VE_HAVE_AUDIO=1)
endif()

# ---------------------------------------------------------------------------
# Vulkan: compile GLSL 450 -> SPIR-V at build time, embed as uint32 arrays
# ---------------------------------------------------------------------------
if(VE_VULKAN_ACTIVE)
    set(VE_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
    file(MAKE_DIRECTORY "${VE_SPV_DIR}")
    set(VE_SPV_OUTPUTS)
    foreach(stage vert frag)
        set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vk/composite.${stage}")
        set(out "${VE_SPV_DIR}/composite.${stage}.inc")
        if(VE_GLSLC)
            add_custom_command(OUTPUT "${out}"
                COMMAND "${VE_GLSLC}" --target-env=vulkan1.0 -mfmt=c -o "${out}" "${src}"
                DEPENDS "${src}"
                COMMENT "glslc composite.${stage}"
                VERBATIM)
        else()
            add_custom_command(OUTPUT "${out}"
                COMMAND "${VE_GLSLANG}" -V --target-env vulkan1.0 -x -o "${out}" "${src}"
                DEPENDS "${src}"
                COMMENT "glslangValidator composite.${stage}"
                VERBATIM)
        endif()
        list(APPEND VE_SPV_OUTPUTS "${out}")
    endforeach()
    add_custom_target(ve_shaders DEPENDS ${VE_SPV_OUTPUTS})
    add_dependencies(ve_lib ve_shaders)
    target_include_directories(ve_lib PUBLIC "${VE_SPV_DIR}")
    target_compile_definitions(ve_lib PUBLIC VE_HAVE_VULKAN=1)
    if(TARGET Vulkan::Headers)
        target_link_libraries(ve_lib PUBLIC Vulkan::Headers)
    else()
        target_include_directories(ve_lib PUBLIC ${Vulkan_INCLUDE_DIRS})
    endif()
endif()

if(MSVC)
    target_compile_options(ve_lib PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
    target_compile_definitions(ve_lib PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ve_lib PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

qt_add_executable(video-editor WIN32 MACOSX_BUNDLE src/main.cpp)
target_link_libraries(video-editor PRIVATE ve_lib)

# Headless MCP server (stdio / HTTP) for the Claude Agent SDK. Console app so
# stdin/stdout carry the JSON-RPC stream.
qt_add_executable(video-editor-mcp src/mcp_main.cpp)
target_link_libraries(video-editor-mcp PRIVATE ve_lib)
set_target_properties(video-editor-mcp PROPERTIES OUTPUT_NAME "VideoEditorMCP")

option(VE_BUILD_TESTS "Build the smoke test" ON)
if(VE_BUILD_TESTS)
    enable_testing()
    qt_add_executable(ve_smoke_test tests/smoke_test.cpp)
    target_link_libraries(ve_smoke_test PRIVATE ve_lib)
    add_test(NAME smoke COMMAND ve_smoke_test "${CMAKE_CURRENT_SOURCE_DIR}/testdata" "${CMAKE_CURRENT_BINARY_DIR}/test-output")

    # End-to-end MCP test over stdio (every tool must be exercised).
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME mcp_e2e
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_e2e.py"
                    --exe "$<TARGET_FILE:video-editor-mcp>"
                    --data "${CMAKE_CURRENT_SOURCE_DIR}/testdata"
                    --out "${CMAKE_CURRENT_BINARY_DIR}/mcp-output")
        # AI voice/music separation (skips itself when the environment is not installed)
        add_test(NAME separation
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/separation_test.py"
                    --exe "$<TARGET_FILE:video-editor-mcp>" --data "${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    endif()

    # Claude panel bridge (Node + Claude Agent SDK). Needs `npm install` in
    # claude-bridge/; makes no request to Claude.
    find_program(NODE_EXECUTABLE node)
    if(NODE_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/claude-bridge/node_modules/@anthropic-ai/claude-agent-sdk")
        add_test(NAME claude_bridge
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_bridge_smoke.mjs"
                    "${CMAKE_CURRENT_SOURCE_DIR}/claude-bridge/bridge.mjs")
    endif()
endif()

set_target_properties(video-editor PROPERTIES
    OUTPUT_NAME "VideoEditor"
    MACOSX_BUNDLE_BUNDLE_NAME "VideoEditor"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.example.videoeditor")

install(TARGETS video-editor
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

message(STATUS "VideoEditor: Vulkan renderer = ${VE_VULKAN_ACTIVE}, audio playback = ${VE_ENABLE_AUDIO}")
