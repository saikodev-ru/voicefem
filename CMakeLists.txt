cmake_minimum_required(VERSION 3.22)
project(VoiceFem VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static runtime: the .vst3 works without installing the VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------- neural engine
# Optional ONNX Runtime based voice-conversion module.
# Set VFEM_NEURAL=OFF to build the DSP-only plugin.
# Set VFEM_ORT_DIR=/path/to/prebuilt-onnxruntime to skip the download.
option(VFEM_NEURAL "Build with the ONNX neural voice engine" ON)
set(VFEM_ORT_VERSION "1.20.1")
set(VFEM_ORT_DIR "" CACHE PATH "Prebuilt onnxruntime dir (must contain include/ and lib/)")

if (VFEM_NEURAL AND NOT VFEM_ORT_DIR)
    include(ProcessorCount)
    if (WIN32)
        set(ORT_ZIP "onnxruntime-win-x64-${VFEM_ORT_VERSION}.zip")
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${VFEM_ORT_VERSION}/${ORT_ZIP}")
        set(ORT_LOCAL "${CMAKE_BINARY_DIR}/_ort/onnxruntime-win-x64-${VFEM_ORT_VERSION}")
        if (NOT EXISTS "${ORT_LOCAL}/include/onnxruntime_cxx_api.h")
            message(STATUS "Downloading onnxruntime ${VFEM_ORT_VERSION} (win-x64)...")
            file(DOWNLOAD "${ORT_URL}" "${CMAKE_BINARY_DIR}/_ort/ort.zip" SHOW_PROGRESS)
            file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/_ort/x")
            file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/_ort/ort.zip" DESTINATION "${CMAKE_BINARY_DIR}/_ort/x")
            file(RENAME "${CMAKE_BINARY_DIR}/_ort/x/onnxruntime-win-x64-${VFEM_ORT_VERSION}" "${ORT_LOCAL}")
        endif()
        set(VFEM_ORT_DIR "${ORT_LOCAL}")
    elseif (APPLE)
        set(ORT_TGZ "onnxruntime-osx-universal2-${VFEM_ORT_VERSION}.tgz")
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${VFEM_ORT_VERSION}/${ORT_TGZ}")
        set(ORT_LOCAL "${CMAKE_BINARY_DIR}/_ort/onnxruntime-osx-universal2-${VFEM_ORT_VERSION}")
        if (NOT EXISTS "${ORT_LOCAL}/include/onnxruntime_cxx_api.h")
            message(STATUS "Downloading onnxruntime ${VFEM_ORT_VERSION} (macOS universal2)...")
            file(DOWNLOAD "${ORT_URL}" "${CMAKE_BINARY_DIR}/_ort/ort.tgz" SHOW_PROGRESS)
            file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/_ort/x")
            file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/_ort/ort.tgz" DESTINATION "${CMAKE_BINARY_DIR}/_ort/x")
            file(RENAME "${CMAKE_BINARY_DIR}/_ort/x/onnxruntime-osx-universal2-${VFEM_ORT_VERSION}" "${ORT_LOCAL}")
        endif()
        set(VFEM_ORT_DIR "${ORT_LOCAL}")
    else()
        set(ORT_TGZ "onnxruntime-linux-x64-${VFEM_ORT_VERSION}.tgz")
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${VFEM_ORT_VERSION}/${ORT_TGZ}")
        set(ORT_LOCAL "${CMAKE_BINARY_DIR}/_ort/onnxruntime-linux-x64-${VFEM_ORT_VERSION}")
        if (NOT EXISTS "${ORT_LOCAL}/include/onnxruntime_cxx_api.h")
            message(STATUS "Downloading onnxruntime ${VFEM_ORT_VERSION} (linux-x64)...")
            file(DOWNLOAD "${ORT_URL}" "${CMAKE_BINARY_DIR}/_ort/ort.tgz" SHOW_PROGRESS)
            file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/_ort/x")
            file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/_ort/ort.tgz" DESTINATION "${CMAKE_BINARY_DIR}/_ort/x")
            file(RENAME "${CMAKE_BINARY_DIR}/_ort/x/onnxruntime-linux-x64-${VFEM_ORT_VERSION}" "${ORT_LOCAL}")
        endif()
        set(VFEM_ORT_DIR "${ORT_LOCAL}")
    endif()
endif()

juce_add_plugin(VoiceFem
    COMPANY_NAME "Personal"
    PLUGIN_MANUFACTURER_CODE Pers
    PLUGIN_CODE Vfem
    FORMATS VST3
    PRODUCT_NAME "VoiceFem"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Pitch\ Shift
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(VoiceFem PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/VoiceFemEngine.h
    Source/NeuralVC.h)

if (VFEM_NEURAL)
    target_sources(VoiceFem PRIVATE Source/OrtDelayLoad.cpp)
endif()

target_compile_definitions(VoiceFem PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_ALSA=0
    JUCE_JACK=0)

target_link_libraries(VoiceFem
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if (VFEM_NEURAL)
    target_compile_definitions(VoiceFem PRIVATE VFEM_NEURAL=1)
    target_include_directories(VoiceFem PRIVATE "${VFEM_ORT_DIR}/include")

    if (WIN32)
        add_library(ort SHARED IMPORTED GLOBAL)
        set_target_properties(ort PROPERTIES
            IMPORTED_LOCATION "${VFEM_ORT_DIR}/lib/onnxruntime.dll"
            IMPORTED_IMPLIB "${VFEM_ORT_DIR}/lib/onnxruntime.lib")
        target_link_libraries(VoiceFem PRIVATE ort)

        # place onnxruntime.dll inside the .vst3 bundle, next to the plugin dll.
        # NOTE: must be attached to the real VST3 target (VoiceFem_VST3);
        # 'VoiceFem' is JUCE's shared-code static lib and its TARGET_FILE_DIR
        # is NOT the bundle (that bug left the bundle without the ORT dll, so
        # hosts failed to load the plugin at all).
        if (TARGET VoiceFem_VST3)
            add_custom_command(TARGET VoiceFem_VST3 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VFEM_ORT_DIR}/lib/onnxruntime.dll"
                    "$<TARGET_FILE_DIR:VoiceFem_VST3>/onnxruntime.dll")
        else()
            add_custom_command(TARGET VoiceFem POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VFEM_ORT_DIR}/lib/onnxruntime.dll"
                    "$<TARGET_FILE_DIR:VoiceFem>/onnxruntime.dll")
        endif()

        # delay-load onnxruntime.dll and resolve it from the plugin's own
        # directory (see OrtDelayLoad.cpp): survives hosts that load VST3 dlls
        # without LOAD_WITH_ALTERED_SEARCH_PATH (the bundle dir is then NOT in
        # the DLL search path). Also lets the DSP engine run with no ORT dll
        # present at all.
        if (MSVC AND TARGET VoiceFem_VST3)
            target_link_libraries(VoiceFem_VST3 PRIVATE delayimp)
            target_link_options(VoiceFem_VST3 PRIVATE "/DELAYLOAD:onnxruntime.dll")
        endif()
    else()
        add_library(ort SHARED IMPORTED GLOBAL)
        if (APPLE)
            set_target_properties(ort PROPERTIES IMPORTED_LOCATION "${VFEM_ORT_DIR}/lib/libonnxruntime.dylib")
            set_target_properties(VoiceFem PROPERTIES
                BUILD_RPATH "${VFEM_ORT_DIR}/lib"
                INSTALL_RPATH "@loader_path;${VFEM_ORT_DIR}/lib")
        else()
            set_target_properties(ort PROPERTIES IMPORTED_LOCATION "${VFEM_ORT_DIR}/lib/libonnxruntime.so")
            # bundle the runtime next to the plugin .so and find it there
            if (TARGET VoiceFem_VST3)
                set_target_properties(VoiceFem_VST3 PROPERTIES
                    BUILD_RPATH "$ORIGIN"
                    INSTALL_RPATH "$ORIGIN")
                add_custom_command(TARGET VoiceFem_VST3 POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${VFEM_ORT_DIR}/lib/libonnxruntime.so.1.20.1"
                        "$<TARGET_FILE_DIR:VoiceFem_VST3>/")
            endif()
        endif()
        target_link_libraries(VoiceFem PRIVATE ort dl)
    endif()
endif()
