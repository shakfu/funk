# CLAP plugin framework, from midi-langs' projects/clap (with midi_clock and
# midi_open from its projects/common). Each plugin adds itself with
# midi_langs_add_clap.
set(MIDI_LANGS_CLAP_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
set(THIRDPARTY ${funk_SOURCE_DIR}/thirdparty)

add_subdirectory(${THIRDPARTY}/pugl ${PROJECT_BINARY_DIR}/thirdparty/pugl)
add_subdirectory(${THIRDPARTY}/imgui ${PROJECT_BINARY_DIR}/thirdparty/imgui)

# Script time and MIDI output, real or through a plugin's sink.
add_library(midi_open STATIC midi_open.c midi_clock.c)
target_include_directories(midi_open PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(midi_open PUBLIC libremidi)

# The parts that do not depend on the language or on pugl.
add_library(midi_langs_clap STATIC timeline.cpp editor_logic.cpp udp_server.cpp)
target_include_directories(midi_langs_clap PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${THIRDPARTY}/clap/include
)
target_compile_features(midi_langs_clap PUBLIC cxx_std_17)
set_target_properties(midi_langs_clap PROPERTIES CXX_VISIBILITY_PRESET hidden)
if(WIN32)
    target_link_libraries(midi_langs_clap PUBLIC ws2_32)
endif()

# midi_langs_add_clap(<target> NAME <file name> CLASS_PREFIX <ObjC prefix>
#                     SOURCES <adapter sources> LIBRARIES <interpreter libraries>)
# Builds <build>/clap/<NAME>.clap. The adapter defines kLanguage (language.hpp).
function(midi_langs_add_clap target)
    cmake_parse_arguments(ARG "" "NAME;CLASS_PREFIX" "SOURCES;LIBRARIES" ${ARGN})
    # A pugl per plugin: Objective-C classes share one namespace in the host process.
    midi_langs_add_pugl(${target}_pugl ${ARG_CLASS_PREFIX})

    add_library(${target} MODULE
        ${MIDI_LANGS_CLAP_DIR}/plugin.cpp
        ${MIDI_LANGS_CLAP_DIR}/editor_imgui.cpp
        ${ARG_SOURCES}
    )
    target_link_libraries(${target} PRIVATE midi_langs_clap ${target}_pugl imgui_gl3 ${ARG_LIBRARIES})
    target_compile_definitions(${target} PRIVATE MIDI_LANGS_VERSION="${PROJECT_VERSION}")
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${ARG_NAME}
        CXX_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/clap
    )
    if(APPLE)
        target_compile_definitions(${target} PRIVATE GL_SILENCE_DEPRECATION)
        target_link_options(${target} PRIVATE
            -exported_symbols_list ${funk_SOURCE_DIR}/thirdparty/clap/src/macos-symbols.txt)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION clap
            MACOSX_BUNDLE_GUI_IDENTIFIER com.github.shakfu.funk.${ARG_NAME}
            MACOSX_BUNDLE_BUNDLE_NAME ${ARG_NAME}
            MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
            MACOSX_BUNDLE_INFO_PLIST ${funk_SOURCE_DIR}/thirdparty/clap/src/plugins.plist.in)
    else()
        set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".clap")
        if(WIN32)
            target_compile_definitions(${target} PRIVATE NOMINMAX) # <windows.h> via pugl/gl.h
        else()
            # Exports clap_entry only: the interpreter, libremidi and libstdc++'s
            # weak template instances stay out of the host's symbols.
            target_link_options(${target} PRIVATE
                -Wl,--exclude-libs,ALL
                -Wl,--version-script=${funk_SOURCE_DIR}/thirdparty/clap/src/linux-my_plug.version)
        endif()
    endif()
endfunction()

# Interactive host: a plugin's editor in a window, its MIDI to a MIDI port (`make clap-dev`).
midi_langs_add_pugl(clap_dev_host_pugl MidiLangsDevHost)
add_executable(clap_dev_host dev_host.cpp)
target_link_libraries(clap_dev_host PRIVATE clap_dev_host_pugl midi_open ${CMAKE_DL_LIBS})
target_include_directories(clap_dev_host PRIVATE ${THIRDPARTY}/clap/include)
target_compile_features(clap_dev_host PRIVATE cxx_std_17)

# Headless host for the tests in tests/CMakeLists.txt.
add_executable(test_clap_host test_clap_host.cpp)
target_include_directories(test_clap_host PRIVATE ${THIRDPARTY}/clap/include)
target_compile_features(test_clap_host PRIVATE cxx_std_17)
target_link_libraries(test_clap_host PRIVATE ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(test_clap_host PRIVATE ws2_32)
endif()

add_executable(test_clap_units test_clap_units.cpp)
target_link_libraries(test_clap_units PRIVATE midi_langs_clap)
