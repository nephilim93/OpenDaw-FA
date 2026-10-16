cmake_minimum_required(VERSION 3.22)
project(OpenDaw VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Version from git tag ─────────────────────────────────────────────────────
# Runs early so the parsed version is available for JUCE's exe metadata.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --always
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OpenDaw_GIT_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_DESCRIBE_RESULT)
    if(NOT GIT_DESCRIBE_RESULT EQUAL 0)
        set(OpenDaw_GIT_RAW "dev")
    endif()
else()
    set(OpenDaw_GIT_RAW "dev")
endif()

# Strip leading "v" for display (v1.0.003 -> 1.0.003)
string(REGEX REPLACE "^v" "" OpenDaw_DISPLAY_VERSION "${OpenDaw_GIT_RAW}")

# Extract numeric major.minor.patch for Windows exe metadata
string(REGEX MATCH "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ver_match "${OpenDaw_GIT_RAW}")
if(_ver_match)
    math(EXPR _major "${CMAKE_MATCH_1}")
    math(EXPR _minor "${CMAKE_MATCH_2}")
    math(EXPR _patch "${CMAKE_MATCH_3}")
    set(OpenDaw_NUMERIC_VERSION "${_major}.${_minor}.${_patch}")
else()
    set(OpenDaw_NUMERIC_VERSION "0.0.0")
endif()

message(STATUS "OpenDaw display version : ${OpenDaw_DISPLAY_VERSION}")
message(STATUS "OpenDaw numeric version : ${OpenDaw_NUMERIC_VERSION}")

# ── Qt6 ──────────────────────────────────────────────────────────────────────
set(CMAKE_PREFIX_PATH "c:/qt/6.10.2/msvc2022_64" CACHE STRING "Qt prefix path")
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent PrintSupport)
qt_standard_project_setup()

# ── FFmpeg ────────────────────────────────────────────────────────────────────
# On Windows: vcpkg provides FindFFMPEG.cmake via toolchain file.
# On Linux/macOS: system packages provide pkg-config .pc files.
if(WIN32)
    find_package(FFMPEG REQUIRED)
    set(FFMPEG_LINK_TARGET ${FFMPEG_LIBRARIES})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    set(FFMPEG_LINK_TARGET PkgConfig::FFMPEG)
endif()

# ── Faust (optional) ─────────────────────────────────────────────────────────
# Modular Synth code modules compile Faust source at runtime with libfaust's
# LLVM backend. Off: patches with code modules still load but render silence.
option(OPENDAW_WITH_FAUST "Compile Faust code modules in the Modular Synth (needs libfaust)" OFF)
add_library(opendaw_faust INTERFACE)
if(OPENDAW_WITH_FAUST)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FAUST QUIET IMPORTED_TARGET faust)
    endif()
    if(FAUST_FOUND)
        target_link_libraries(opendaw_faust INTERFACE PkgConfig::FAUST)
    else()
        find_path(FAUST_INCLUDE_DIR faust/dsp/llvm-dsp.h
                  HINTS /opt/homebrew/include /usr/local/include)
        find_library(FAUST_LIBRARY NAMES faust libfaust
                     HINTS /opt/homebrew/lib /usr/local/lib)
        if(NOT FAUST_INCLUDE_DIR OR NOT FAUST_LIBRARY)
            message(FATAL_ERROR "OPENDAW_WITH_FAUST is ON but libfaust was not found. "
                    "Install it (macOS: brew install faust; Ubuntu/Debian: apt install "
                    "faust libfaust2t64) or set FAUST_INCLUDE_DIR and FAUST_LIBRARY.")
        endif()
        target_include_directories(opendaw_faust INTERFACE ${FAUST_INCLUDE_DIR})
        target_link_libraries(opendaw_faust INTERFACE ${FAUST_LIBRARY})
    endif()
    # Where stdfaust.lib lives, for import("stdfaust.lib") in code modules.
    find_path(OPENDAW_FAUST_LIBRARY_DIR stdfaust.lib
              HINTS /opt/homebrew/share/faust /usr/local/share/faust /usr/share/faust)
    target_compile_definitions(opendaw_faust INTERFACE OPENDAW_WITH_FAUST=1)
    if(OPENDAW_FAUST_LIBRARY_DIR)
        target_compile_definitions(opendaw_faust INTERFACE
            OPENDAW_FAUST_LIBRARY_DIR="${OPENDAW_FAUST_LIBRARY_DIR}")
    endif()
    message(STATUS "Faust code modules: enabled (libraries: ${OPENDAW_FAUST_LIBRARY_DIR})")
endif()

# ── JUCE ─────────────────────────────────────────────────────────────────────
set(JUCE_MODULES_ONLY OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/JUCE)

# ── Tracktion Engine (detects JUCE automatically) ────────────────────────────
set(TE_ADD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/tracktion_engine)

# ── Source files ─────────────────────────────────────────────────────────────
set(OpenDaw_SOURCES
    src/main.cpp
    src/app/OpenDawApplication.h    src/app/OpenDawApplication.cpp
    src/app/JuceQtBridge.h          src/app/JuceQtBridge.cpp
    src/engine/AudioEngine.h        src/engine/AudioEngine.cpp
    src/engine/EditManager.h        src/engine/EditManager.cpp
    src/engine/midifx/VelocityFxPlugin.h src/engine/midifx/VelocityFxPlugin.cpp
    src/engine/modular/ModularSynthPlugin.h src/engine/modular/ModularSynthPlugin.cpp
    src/modular/Module.h src/modular/ModuleRegistry.h src/modular/Modules.cpp
    src/modular/ModuleHelpers.h src/modular/ModuleLibrary.h
    src/modular/ModulesSequencing.cpp src/modular/ModulesRandom.cpp src/modular/ModulesFx.cpp
    src/modular/ModulesExtra.cpp src/modular/ModulesVoices.cpp
    src/modular/FaustStarters.h src/modular/FaustStarters.cpp
    src/modular/Patch.h src/modular/Patch.cpp
    src/modular/CompiledPatch.h src/modular/CompiledPatch.cpp
    src/modular/FaustModule.cpp
    src/engine/PluginScanner.h      src/engine/PluginScanner.cpp
    src/ui/MainWindow.h             src/ui/MainWindow.cpp
    src/ui/timeline/TimelineView.h  src/ui/timeline/TimelineView.cpp
    src/ui/timeline/TimeRuler.h     src/ui/timeline/TimeRuler.cpp
    src/ui/timeline/TrackLane.h     src/ui/timeline/TrackLane.cpp
    src/ui/timeline/ClipItem.h      src/ui/timeline/ClipItem.cpp
    src/ui/timeline/GridSnapper.h   src/ui/timeline/GridSnapper.cpp
    src/ui/timeline/TrackHeaderWidget.h src/ui/timeline/TrackHeaderWidget.cpp
    src/ui/timeline/EnvelopeUtils.h
    src/ui/timeline/AutomationLaneItem.h  src/ui/timeline/AutomationLaneItem.cpp
    src/ui/timeline/AutomationPointItem.h src/ui/timeline/AutomationPointItem.cpp
    src/ui/timeline/AutomationLaneHeader.h src/ui/timeline/AutomationLaneHeader.cpp
    src/ui/timeline/MarkerTempoLane.h     src/ui/timeline/MarkerTempoLane.cpp
    src/ui/timeline/FolderHeaderWidget.h  src/ui/timeline/FolderHeaderWidget.cpp
    src/ui/mixer/MixerView.h        src/ui/mixer/MixerView.cpp
    src/ui/mixer/ChannelStrip.h     src/ui/mixer/ChannelStrip.cpp
    src/ui/transport/TransportBar.h src/ui/transport/TransportBar.cpp
    src/ui/controls/ParamKnobMapping.h    src/ui/controls/ParamKnobMapping.cpp
    src/ui/controls/RotaryKnob.h    src/ui/controls/RotaryKnob.cpp
    src/ui/controls/VolumeFader.h   src/ui/controls/VolumeFader.cpp
    src/ui/controls/LevelMeter.h    src/ui/controls/LevelMeter.cpp
    src/ui/controls/WaveformWidget.h src/ui/controls/WaveformWidget.cpp
    src/ui/dialogs/ExportDialog.h        src/ui/dialogs/ExportDialog.cpp
    src/ui/dialogs/RecoveryDialog.h      src/ui/dialogs/RecoveryDialog.cpp
    src/ui/dialogs/AudioSettingsPage.h src/ui/dialogs/AudioSettingsPage.cpp
    src/ui/dialogs/SettingsDialog.h src/ui/dialogs/SettingsDialog.cpp
    src/utils/AppPaths.h
    src/ui/effects/EffectChainWidget.h   src/ui/effects/EffectChainWidget.cpp
    src/ui/effects/DelayEffectWidget.h   src/ui/effects/DelayEffectWidget.cpp
    src/ui/effects/EffectSelectorDialog.h src/ui/effects/EffectSelectorDialog.cpp
    src/ui/effects/VstSelectorDialog.h    src/ui/effects/VstSelectorDialog.cpp
    src/ui/effects/PluginEditorWindow.h   src/ui/effects/PluginEditorWindow.cpp
    src/ui/audioclip/WaveformSelection.h
    src/ui/audioclip/AudioFileUndoManager.h  src/ui/audioclip/AudioFileUndoManager.cpp
    src/ui/audioclip/AudioFileOperations.h   src/ui/audioclip/AudioFileOperations.cpp
    src/ui/audioclip/AudioClipEditor.h       src/ui/audioclip/AudioClipEditor.cpp
    src/ui/audioclip/AudioWaveformView.h     src/ui/audioclip/AudioWaveformView.cpp
    src/ui/pianoroll/PianoRollEditor.h    src/ui/pianoroll/PianoRollEditor.cpp
    src/ui/pianoroll/PianoKeyboard.h      src/ui/pianoroll/PianoKeyboard.cpp
    src/ui/pianoroll/NoteGrid.h           src/ui/pianoroll/NoteGrid.cpp
    src/ui/tools/EditToolState.h          src/ui/tools/EditToolState.cpp
    src/ui/pianoroll/NoteItem.h           src/ui/pianoroll/NoteItem.cpp
    src/ui/pianoroll/VelocityLane.h       src/ui/pianoroll/VelocityLane.cpp
    src/ui/pianoroll/CcLane.h             src/ui/pianoroll/CcLane.cpp
    src/ui/pianoroll/ChannelColors.h
    src/ui/pianoroll/PianoRollRuler.h     src/ui/pianoroll/PianoRollRuler.cpp
    src/ui/routing/RoutingView.h          src/ui/routing/RoutingView.cpp
    src/ui/routing/RoutingNode.h          src/ui/routing/RoutingNode.cpp
    src/ui/routing/CableItem.h            src/ui/routing/CableItem.cpp
    src/ui/modular/ModularEditorView.h    src/ui/modular/ModularEditorView.cpp
    src/ui/modular/ModulePanelItem.h      src/ui/modular/ModulePanelItem.cpp
    src/ui/modular/FaustCodeDialog.h      src/ui/modular/FaustCodeDialog.cpp
    src/ui/modular/ModularEditorLink.h    src/ui/modular/ModularEditorLink.cpp
    src/ui/drums/DrumsView.h              src/ui/drums/DrumsView.cpp
    src/ui/drums/DrumPadWidget.h          src/ui/drums/DrumPadWidget.cpp
    src/ui/drums/DrumPadEditor.h          src/ui/drums/DrumPadEditor.cpp
    src/ui/drums/DrumsViewLink.h          src/ui/drums/DrumsViewLink.cpp
    src/ui/drums/GmDrumNames.h            src/ui/drums/GmDrumNames.cpp
    src/ui/drums/SampleBrowser.h          src/ui/drums/SampleBrowser.cpp
    src/ui/sheetmusic/SheetMusicView.h    src/ui/sheetmusic/SheetMusicView.cpp
    src/ui/sheetmusic/NotationModel.h     src/ui/sheetmusic/NotationModel.cpp
    src/ui/sheetmusic/ScoreScene.h        src/ui/sheetmusic/ScoreScene.cpp
    src/ui/browser/FileBrowserPanel.h    src/ui/browser/FileBrowserPanel.cpp
    src/ui/SplashScreen.h           src/ui/SplashScreen.cpp
    src/utils/WaveformCache.h       src/utils/WaveformCache.cpp
    src/utils/ThemeManager.h        src/utils/ThemeManager.cpp
    src/utils/IconFont.h
    src/music/Theory.h             src/music/Theory.cpp
    src/music/Patterns.h           src/music/Patterns.cpp
    src/music/DrumPatterns.h       src/music/DrumPatterns.cpp
    src/music/Quantize.h           src/music/Quantize.cpp
    src/drums/DrumKit.h            src/drums/DrumKit.cpp
    src/drums/DrumEngine.h         src/drums/DrumEngine.cpp
    src/drums/SampleLibrary.h      src/drums/SampleLibrary.cpp
    src/drums/KitPresets.h         src/drums/KitPresets.cpp
    src/drums/KitFromFolder.h      src/drums/KitFromFolder.cpp
    src/engine/drums/DrumMachinePlugin.h src/engine/drums/DrumMachinePlugin.cpp
    src/ai/AiTypes.h
    src/ai/AiAudioAnalysis.h      src/ai/AiAudioAnalysis.cpp
    src/ai/AiToolDefs.h            src/ai/AiToolDefs.cpp
    src/ai/AiToolExecutor.h        src/ai/AiToolExecutor.cpp
    src/ai/AiService.h             src/ai/AiService.cpp
    src/ai/AiOpenAiCompat.h src/ai/AiOpenAiCompat.cpp
    src/ai/AiModularTools.cpp
    src/ai/AiDrumTools.cpp
    src/ai/AiChatStore.h           src/ai/AiChatStore.cpp
    src/ai/AiChatWidget.h          src/ai/AiChatWidget.cpp
    src/ai/AiQuickPrompt.h         src/ai/AiQuickPrompt.cpp
    src/utils/FFmpegUtils.h        src/utils/FFmpegUtils.cpp
    src/video/VideoThumbnailCache.h src/video/VideoThumbnailCache.cpp
    src/ui/timeline/VideoTrackLane.h src/ui/timeline/VideoTrackLane.cpp
    src/ui/video/VideoPlayerWidget.h src/ui/video/VideoPlayerWidget.cpp
)

# ── Create the OpenDaw JUCE GUI App target ───────────────────────────────────
juce_add_gui_app(OpenDaw
    VERSION "${OpenDaw_NUMERIC_VERSION}"
    PRODUCT_NAME "OpenDaw"
    COMPANY_NAME "OpenDaw"
    BUNDLE_ID "com.OpenDaw.app"
    ICON_BIG "${CMAKE_SOURCE_DIR}/resources/icon.png"
    ICON_SMALL "${CMAKE_SOURCE_DIR}/resources/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "OpenDaw needs microphone access for audio recording")

juce_generate_juce_header(OpenDaw)

target_sources(OpenDaw PRIVATE ${OpenDaw_SOURCES})

# ── macOS development signing ────────────────────────────────────────────────
# Local builds are ad-hoc signed, so macOS treats every rebuild as a new app
# and asks for microphone permission again. Set this to a code-signing
# identity (name or SHA-1 from `security find-identity -v -p codesigning`)
# to sign each build with it so permissions persist across rebuilds.
# Empty by default; release builds are signed by the release workflow.
set(OPENDAW_MACOS_SIGN_IDENTITY "" CACHE STRING
    "macOS code-signing identity for local builds (empty = ad-hoc)")
if(APPLE AND OPENDAW_MACOS_SIGN_IDENTITY)
    add_custom_command(TARGET OpenDaw POST_BUILD
        COMMAND codesign --force --sign "${OPENDAW_MACOS_SIGN_IDENTITY}"
                --identifier "com.OpenDaw.app" "$<TARGET_BUNDLE_DIR:OpenDaw>"
        COMMENT "Signing OpenDaw.app for local development"
        VERBATIM)
endif()

# ── Windows application icon ─────────────────────────────────────────────────
# JUCE's generated RC expects icon.ico next to OpenDaw_resources.rc
if(WIN32)
    file(COPY "${CMAKE_SOURCE_DIR}/resources/OpenDaw.ico"
         DESTINATION "${CMAKE_BINARY_DIR}/OpenDaw_artefacts/JuceLibraryCode")
    file(RENAME
         "${CMAKE_BINARY_DIR}/OpenDaw_artefacts/JuceLibraryCode/OpenDaw.ico"
         "${CMAKE_BINARY_DIR}/OpenDaw_artefacts/JuceLibraryCode/icon.ico")
endif()
qt_add_resources(OpenDaw "OpenDaw_resources" PREFIX "/" BASE resources FILES
    resources/splash.png
    resources/icon.png
    resources/fontaudio.ttf
    resources/MaterialSymbolsOutlined.ttf
    resources/mixerStripBG.png
    resources/Bravura.otf)
target_include_directories(OpenDaw PRIVATE src ${CMAKE_SOURCE_DIR}/libs/vst2sdk ${FFMPEG_INCLUDE_DIRS})
target_link_directories(OpenDaw PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_compile_definitions(OpenDaw PRIVATE
    JUCE_PLUGINHOST_VST=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    DONT_SET_USING_JUCE_NAMESPACE=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1)

target_link_libraries(OpenDaw PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::PrintSupport
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_events
    juce::juce_recommended_warning_flags
    opendaw_faust
    ${FFMPEG_LINK_TARGET})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(OpenDaw PRIVATE -Wno-unknown-pragmas)
    # GCC 14+ reports errors in uninstantiated template bodies. Tracktion's bundled
    # nanorange.hpp has one (iota_view::iterator uses x.value instead of x.value_).
    target_compile_options(OpenDaw PRIVATE -Wno-template-body)
endif()

if(MSVC)
    target_compile_options(OpenDaw PRIVATE /EHsc /bigobj)
endif()

target_compile_definitions(OpenDaw PRIVATE
    OpenDaw_VERSION="${OpenDaw_DISPLAY_VERSION}")

if(WIN32)
    target_link_libraries(OpenDaw PRIVATE winmm)
endif()

# ── Modular patching eval runner (optional) ─────────────────────────────────
# A headless program that runs the AI assistant on the eval cases in
# .claude/hillclimb/modular-patching and grades the patches it builds.
option(OPENDAW_BUILD_MODULAR_EVAL "Build the modular patching eval runner" OFF)
if(OPENDAW_BUILD_MODULAR_EVAL)
    set(OpenDawModularEval_SOURCES ${OpenDaw_SOURCES})
    list(REMOVE_ITEM OpenDawModularEval_SOURCES src/main.cpp)
    juce_add_console_app(OpenDawModularEval PRODUCT_NAME "OpenDawModularEval")
    target_sources(OpenDawModularEval PRIVATE ${OpenDawModularEval_SOURCES}
                   tools/modular-eval/ModularEvalMain.cpp)
    get_target_property(_od_defs OpenDaw COMPILE_DEFINITIONS)
    target_compile_definitions(OpenDawModularEval PRIVATE ${_od_defs})
    target_include_directories(OpenDawModularEval PRIVATE src ${CMAKE_SOURCE_DIR}/libs/vst2sdk ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(OpenDawModularEval PRIVATE ${FFMPEG_LIBRARY_DIRS})
    get_target_property(_od_libs OpenDaw LINK_LIBRARIES)
    target_link_libraries(OpenDawModularEval PRIVATE ${_od_libs})
endif()

# ── Modular engine tests (optional) ─────────────────────────────────────────
# Headless checks of the modular synth engine. It only needs Qt Core, so the
# tests link its sources directly: no audio device, GUI or API key.
# Run with: ctest --test-dir build --output-on-failure
option(OPENDAW_BUILD_TESTS "Build the modular engine test suite" OFF)
if(OPENDAW_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Core)
    file(GLOB OpenDawModularTests_SOURCES CONFIGURE_DEPENDS
         tests/modular/*.cpp src/modular/*.cpp tests/drums/*.cpp src/drums/*.cpp)
    add_executable(opendaw_modular_tests ${OpenDawModularTests_SOURCES} src/music/Theory.cpp
                   src/music/DrumPatterns.cpp)
    target_include_directories(opendaw_modular_tests PRIVATE src)
    target_link_libraries(opendaw_modular_tests PRIVATE Qt6::Core opendaw_faust)
    foreach(_group patch pitch envelope feedback voices sequencing random fx extra catalog faust synthvoices drums drumpatterns)
        add_test(NAME modular.${_group} COMMAND opendaw_modular_tests ${_group})
    endforeach()
endif()

# ── Editor UI tests (optional) ──────────────────────────────────────────────
# Drive the Modular patch editor and the Drums tab against a real Edit with
# Qt's offscreen platform. They need the whole engine, so each compiles the
# app sources like the eval runner does; kept apart from the light suite above.
# Run with: ctest --test-dir build -R "modular.editor|drums.ui" --output-on-failure
option(OPENDAW_BUILD_EDITOR_TESTS "Build the Modular editor and Drums tab UI tests" OFF)
if(OPENDAW_BUILD_EDITOR_TESTS)
    enable_testing()
    set(OpenDawEditorTest_SOURCES ${OpenDaw_SOURCES})
    list(REMOVE_ITEM OpenDawEditorTest_SOURCES src/main.cpp)
    get_target_property(_od_defs OpenDaw COMPILE_DEFINITIONS)
    get_target_property(_od_opts OpenDaw COMPILE_OPTIONS)
    get_target_property(_od_libs OpenDaw LINK_LIBRARIES)

    function(opendaw_add_ui_test target product test_name test_source)
        juce_add_console_app(${target} PRODUCT_NAME "${product}")
        target_sources(${target} PRIVATE ${OpenDawEditorTest_SOURCES} ${test_source})
        target_compile_definitions(${target} PRIVATE ${_od_defs})
        if(_od_opts)
            target_compile_options(${target} PRIVATE ${_od_opts})
        endif()
        target_include_directories(${target} PRIVATE src ${CMAKE_SOURCE_DIR}/libs/vst2sdk ${FFMPEG_INCLUDE_DIRS})
        target_link_directories(${target} PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(${target} PRIVATE ${_od_libs})
        add_test(NAME ${test_name} COMMAND ${target})
        set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endfunction()

    opendaw_add_ui_test(opendaw_modular_editor_test "OpenDawModularEditorTest" modular.editor
                        tests/modular_editor/ModularEditorTest.cpp)
    opendaw_add_ui_test(opendaw_drums_ui_test "OpenDawDrumsUiTest" drums.ui
                        tests/drums_ui/DrumsViewTest.cpp)
endif()
