cmake_minimum_required(VERSION 3.21)

project(fft_plugin VERSION 0.0.1 LANGUAGES C CXX)

option(FFT_PLUGIN_BUILD_JUCE_VST3 "Build the JUCE-based FFT VST3 wrapper target." ON)

add_library(fft_plugin_model INTERFACE)
target_include_directories(fft_plugin_model INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(fft_plugin_model INTERFACE cxx_std_20)

add_library(fft_plugin_core
    src/fft_instrument_catalog.cpp
    src/fft_file_playback_engine.cpp
    src/fft_midi_matching.cpp
    src/fft_document.cpp
    src/fft_pitch_tools.cpp
    src/fft_plugin_controller.cpp
    src/fft_plugin_processor_core.cpp
    src/processor/authoring_bridge.cpp
    src/processor/track_control.cpp
    src/processor/track_edit_helpers.cpp
    src/processor/track_edit_service.cpp
    src/processor/track_grouping_helpers.cpp
    src/processor/track_grouping_service.cpp
    src/processor/track_note_helpers.cpp
    src/processor/track_note_service.cpp
    src/processor/transport_control.cpp
    src/fft_plugin_state_codec.cpp
    src/fft_smd_presentation_utils.cpp
    src/fft_smd_authoring_codec.cpp
    src/fft_smd_authoring_model.cpp
    src/fft_smd_file.cpp
    src/fft_smd_global_lane_assigner.cpp
    src/fft_smd_lane_packer.cpp
    src/fft_smd_loop_roller.cpp
    src/fft_smd_song_metadata_builder.cpp
    src/fft_smd_inspector.cpp
    src/fft_smd_validation.cpp
    src/fft_smd_sequencer.cpp
    src/fft_spu_preview_core.cpp
    src/fft_spu_preview_adapter.cpp
    src/fft_waveset_file_service.cpp
    vendor/exmateria-spu-core/fft_adsr_envelope.cpp
    vendor/exmateria-spu-core/fft_pitch_tools.cpp
    vendor/exmateria-spu-core/fft_smd_sequencer_core.cpp
    vendor/exmateria-spu-core/fft_smd_sequencer_tools.cpp
    vendor/exmateria-spu-core/fft_spu_core_runtime.cpp
    vendor/exmateria-spu-core/fft_spu_core_state_tools.cpp
    vendor/exmateria-spu-core/fft_spu_lfo_tools.cpp
    vendor/exmateria-spu-core/fft_spu_mix_tools.cpp
    vendor/exmateria-spu-core/fft_spu_pitch_runtime.cpp
    vendor/exmateria-spu-core/fft_spu_reverb.cpp
    vendor/exmateria-spu-core/fft_spu_sample_runtime.cpp
    vendor/exmateria-spu-core/fft_spu_voice_runtime.cpp
    vendor/exmateria-spu-core/fft_spu_voice_tools.cpp
)
target_link_libraries(fft_plugin_core PUBLIC fft_plugin_model)
target_include_directories(fft_plugin_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/exmateria-spu-core
)
target_compile_features(fft_plugin_core PUBLIC cxx_std_20)
set_target_properties(fft_plugin_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(FFT_PLUGIN_BUILD_TEST_DRIVER "Build the CLI test drivers and ctest targets." ON)

if(FFT_PLUGIN_BUILD_TEST_DRIVER)
    enable_testing()

    # Fixture roots — overridable from the cmake invocation.
    # Drivers that depend on ROM-derived fixtures auto-skip when the file is
    # absent (driver returns an explicit skip code that ctest treats as PASS).
    set(FFT_FIXTURES_SYNTHETIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/synthetic"
        CACHE PATH "Directory holding the committed synthetic .fftauth fixture")
    set(FFT_FIXTURES_HASH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/expected_hashes.json"
        CACHE FILEPATH "JSON map of fixture-name -> SHA-256 used by the byte-identity gate")
    set(FFT_FIXTURES_PROJECT_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/../project-assets"
        CACHE PATH "Local-only ROM extract; tests skip cleanly when absent")
    # Individual fixture overrides — point at canonical locations outside the
    # repo when project-assets/ isn't populated. Empty = use default below.
    set(FFT_FIXTURE_MUSIC31_SMD "" CACHE FILEPATH
        "Override path to MUSIC_31.SMD (default: project-assets/fft-extract/SOUND/MUSIC_31.SMD)")
    set(FFT_FIXTURE_WAVESET_WD "" CACHE FILEPATH
        "Override path to WAVESET.WD (default: project-assets/fft-extract/SOUND/WAVESET.WD)")
    set(FFT_FIXTURE_AERITH_FFTAUTH "" CACHE FILEPATH
        "Override path to aerith.5.fftauth (default: project-assets/fft-authoring/aerith.5.fftauth)")

    set(_fft_drivers
        fft_plugin_state_driver
        fft_spu_parity_driver
        fft_smd_inspect_driver
        fft_smd_render_driver
        fft_file_playback_driver
        fft_plugin_processor_driver
        fft_smd_authoring_parity_driver
        fft_polytrack_roundtrip_driver
        fft_smd_export_regression_driver
        fft_generate_synthetic_fixture
        fft_smd_byte_identity_driver
        fft_tempo_edit_probe
    )
    foreach(_drv IN LISTS _fft_drivers)
        add_executable(${_drv} tools/${_drv}.cpp)
        target_link_libraries(${_drv} PRIVATE fft_plugin_core)
        target_compile_features(${_drv} PRIVATE cxx_std_20)
    endforeach()

    # ===== Pinned, ROM-free regression gates =====

    # Always-on byte-identity gate. Synthetic fixture is checked into the
    # repo; ROM fixtures are picked up automatically when present.
    if(FFT_FIXTURE_MUSIC31_SMD)
        set(_music31_smd "${FFT_FIXTURE_MUSIC31_SMD}")
    else()
        set(_music31_smd "${FFT_FIXTURES_PROJECT_ASSETS}/fft-extract/SOUND/MUSIC_31.SMD")
    endif()
    if(FFT_FIXTURE_WAVESET_WD)
        set(_waveset_wd "${FFT_FIXTURE_WAVESET_WD}")
    else()
        set(_waveset_wd "${FFT_FIXTURES_PROJECT_ASSETS}/fft-extract/SOUND/WAVESET.WD")
    endif()
    if(FFT_FIXTURE_AERITH_FFTAUTH)
        set(_aerith_fftauth "${FFT_FIXTURE_AERITH_FFTAUTH}")
    else()
        set(_aerith_fftauth "${FFT_FIXTURES_PROJECT_ASSETS}/fft-authoring/aerith.5.fftauth")
    endif()

    add_test(NAME byte_identity
        COMMAND fft_smd_byte_identity_driver
            --hashes "${FFT_FIXTURES_HASH_FILE}"
            --synthetic "${FFT_FIXTURES_SYNTHETIC_DIR}/tiny.fftauth"
            --music31 "${_music31_smd}"
            --aerith "${_aerith_fftauth}"
    )
    set_tests_properties(byte_identity PROPERTIES SKIP_RETURN_CODE 77)

    # ===== Drivers that work without ROM fixtures =====

    add_test(NAME plugin_state_driver COMMAND fft_plugin_state_driver)

    # ===== ROM-fixture-dependent drivers (skip when fixtures absent) =====

    if(EXISTS "${_music31_smd}" AND EXISTS "${_waveset_wd}")
        add_test(NAME smd_inspect_driver
            COMMAND fft_smd_inspect_driver "${_music31_smd}")
        add_test(NAME smd_authoring_parity
            COMMAND fft_smd_authoring_parity_driver "${_waveset_wd}" "${_music31_smd}" 2.0 512)
        # polytrack_roundtrip is currently expected to fail on real ROM data:
        # ungrouping a poly track inserts extra 0x91 loop opcodes that the raw
        # track doesn't have, surfacing a raw↔poly compiler asymmetry. The
        # hard asserts (added in Stage 0) catch that. Once a later stage fixes
        # the underlying issue, drop WILL_FAIL — until then ctest flips green
        # if the test exits non-zero, and red if it suddenly starts "passing"
        # without the fix landing.
        add_test(NAME polytrack_roundtrip
            COMMAND fft_polytrack_roundtrip_driver "${_music31_smd}" 1)
        set_tests_properties(polytrack_roundtrip PROPERTIES WILL_FAIL TRUE)
        add_test(NAME smd_render
            COMMAND fft_smd_render_driver "${_waveset_wd}" "${_music31_smd}")
        add_test(NAME smd_file_playback
            COMMAND fft_file_playback_driver "${_waveset_wd}" "${_music31_smd}")
        add_test(NAME plugin_processor
            COMMAND fft_plugin_processor_driver "${_waveset_wd}" "${_music31_smd}")
    endif()

    if(EXISTS "${_aerith_fftauth}")
        add_test(NAME smd_export_regression_aerith
            COMMAND fft_smd_export_regression_driver "${_aerith_fftauth}")
    endif()

    # First-time bootstrap of the regression gate. Generates the synthetic
    # fixture, then captures expected hashes from the current codec output.
    # Commit both outputs after running.
    add_custom_target(fft_bootstrap_byte_identity
        COMMAND ${CMAKE_COMMAND} -E make_directory "${FFT_FIXTURES_SYNTHETIC_DIR}"
        COMMAND $<TARGET_FILE:fft_generate_synthetic_fixture>
            "${FFT_FIXTURES_SYNTHETIC_DIR}/tiny.fftauth"
        COMMAND $<TARGET_FILE:fft_smd_byte_identity_driver>
            --update
            --hashes "${FFT_FIXTURES_HASH_FILE}"
            --synthetic "${FFT_FIXTURES_SYNTHETIC_DIR}/tiny.fftauth"
            --music31 "${_music31_smd}"
            --aerith "${_aerith_fftauth}"
        DEPENDS fft_generate_synthetic_fixture fft_smd_byte_identity_driver
        COMMENT "Generating synthetic fixture and capturing baseline hashes"
        VERBATIM
    )
endif()

if(FFT_PLUGIN_BUILD_JUCE_VST3)
    # Try a sibling JUCE checkout first (monorepo / vendored layout); fall
    # back to FetchContent for standalone consumers of the published repo.
    if(NOT TARGET juce::juce_audio_utils)
        set(_fft_juce_local "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/JUCE")
        if(EXISTS "${_fft_juce_local}/CMakeLists.txt")
            add_subdirectory("${_fft_juce_local}"
                             "${CMAKE_CURRENT_BINARY_DIR}/vendor/JUCE"
                             EXCLUDE_FROM_ALL)
        else()
            include(FetchContent)
            FetchContent_Declare(
                JUCE
                GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
                GIT_TAG        8.0.4
                GIT_SHALLOW    TRUE
            )
            FetchContent_MakeAvailable(JUCE)
        endif()
    endif()

    # JUCE 8 modules are INTERFACE targets — their .cpp files get pulled
    # into the consumer's compile unit. Setting INTERFACE defines on the
    # module makes those sources see the macros at compile time. Without
    # this, juce_gui_extra.cpp picks up its default GTK / WebBrowser path
    # on Linux even though we don't use WebBrowserComponent.
    foreach(_juce_module juce_gui_extra juce_core)
        if(TARGET ${_juce_module})
            target_compile_definitions(${_juce_module} INTERFACE
                JUCE_WEB_BROWSER=0
                JUCE_USE_CURL=0
            )
        endif()
    endforeach()

    juce_add_plugin(FFTSequencerJuce
        VERSION ${PROJECT_VERSION}
        COMPANY_NAME "timbermania"
        COMPANY_WEBSITE "https://github.com/timbermania/fft-monorepo"
        PLUGIN_MANUFACTURER_CODE Tmna
        PLUGIN_CODE Fsq1
        FORMATS VST3
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        PRODUCT_NAME "FFT SMD Editor"
    )

    juce_generate_juce_header(FFTSequencerJuce)

    target_sources(FFTSequencerJuce
        PRIVATE
            src/juce/FFTJuceAudioProcessor.cpp
            src/juce/FFTJuceAudioProcessor.h
            src/juce/FFTJuceAudioProcessorEditor.cpp
            src/juce/FFTJuceAudioProcessorEditor.h
            src/juce/FFTMatchLabComponent.cpp
            src/juce/FFTMatchLabComponent.h
            src/juce/FFTMidiImport.cpp
            src/juce/FFTMidiImport.h
            src/juce/FFTJucePaths.cpp
            src/juce/FFTJucePaths.h
            src/juce/FFTOpcodeStackLayout.cpp
            src/juce/FFTOpcodeStackLayout.h
            src/juce/FFTPianoDetailView.cpp
            src/juce/FFTPianoDetailView.h
            src/juce/FFTTrackLaneView.cpp
            src/juce/FFTTrackLaneView.h
    )

    target_include_directories(FFTSequencerJuce PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/exmateria-spu-core
    )

    target_compile_definitions(FFTSequencerJuce
        PUBLIC
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
    )

    target_link_libraries(FFTSequencerJuce
        PRIVATE
            fft_plugin_core
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
    )

    add_executable(fft_midi_import_probe
        tools/fft_midi_import_probe.cpp
        src/juce/FFTMidiImport.cpp
    )
    target_link_libraries(fft_midi_import_probe
        PRIVATE
            fft_plugin_core
            juce::juce_audio_utils
    )
    target_include_directories(fft_midi_import_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/juce
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/exmateria-spu-core
    )
    target_compile_features(fft_midi_import_probe PRIVATE cxx_std_20)

    add_executable(fft_authoring_probe
        tools/fft_authoring_probe.cpp
    )
    target_link_libraries(fft_authoring_probe PRIVATE fft_plugin_core)
    target_include_directories(fft_authoring_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/exmateria-spu-core
    )
    target_compile_features(fft_authoring_probe PRIVATE cxx_std_20)

    add_executable(fft_midi_tempo_render_probe
        tools/fft_midi_tempo_render_probe.cpp
        src/juce/FFTMidiImport.cpp
    )
    target_link_libraries(fft_midi_tempo_render_probe
        PRIVATE
            fft_plugin_core
            juce::juce_audio_utils
    )
    target_include_directories(fft_midi_tempo_render_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/juce
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/exmateria-spu-core
    )
    target_compile_features(fft_midi_tempo_render_probe PRIVATE cxx_std_20)
endif()
