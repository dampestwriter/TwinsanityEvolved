add_executable(test_service_scheduler test_scheduler.cpp)
target_link_libraries(test_service_scheduler PRIVATE ps2r_title_hle)
target_compile_options(test_service_scheduler PRIVATE -Wall -Wextra -Werror)
add_test(NAME ServiceScheduler COMMAND test_service_scheduler)
set_tests_properties(ServiceScheduler PROPERTIES TIMEOUT 60)

add_executable(test_service_startup test_service_startup.cpp)
target_link_libraries(test_service_startup PRIVATE ps2r_title_hle)
target_compile_options(test_service_startup PRIVATE -Wall -Wextra -Werror)
add_test(NAME ServiceStartup COMMAND test_service_startup)
set_tests_properties(ServiceStartup PROPERTIES TIMEOUT 60)
# StreamTransport (service-only startup): the title's STREAM
# batches answered without a module -- the reply layout, owner routing, the
# per-opcode unowned census, malformed refusal, the cold-wait progress word.
add_executable(test_stream_transport test_stream_transport.cpp)
target_link_libraries(test_stream_transport PRIVATE ps2r_title_hle)
target_compile_options(test_stream_transport PRIVATE -Wall -Wextra -Werror)
add_test(NAME StreamTransport COMMAND test_stream_transport)
set_tests_properties(StreamTransport PROPERTIES TIMEOUT 60)

add_executable(test_service_timeline test_service_timeline.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/service_timeline.cpp)
target_link_libraries(test_service_timeline PRIVATE ps2r_title_hle ps2r_iop)
target_compile_definitions(test_service_timeline PRIVATE PS2R_IOP_ORACLE=1)   # the oracle arm's adapter (app/host/iop_arm.h)
target_compile_options(test_service_timeline PRIVATE -Wall -Wextra -Werror)
add_test(NAME ServiceTimeline COMMAND test_service_timeline)
set_tests_properties(ServiceTimeline PROPERTIES TIMEOUT 60)

add_executable(test_file_schedule test_file_schedule.cpp service_time_case.cpp)
target_link_libraries(test_file_schedule PRIVATE ps2r_title_hle ps2r_cdvd)
target_compile_options(test_file_schedule PRIVATE -Wall -Wextra -Werror)
add_test(NAME FileScheduleSelfOracle COMMAND test_file_schedule)
set_tests_properties(FileScheduleSelfOracle PROPERTIES TIMEOUT 60)

add_executable(test_stream_library ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/tests/cdvd/test_stream_library.cpp)
target_link_libraries(test_stream_library PRIVATE ps2r_title_hle ps2r_cdvd ps2r_ee)
target_compile_options(test_stream_library PRIVATE -Wall -Wextra -Werror)
target_sources(test_stream_library PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/cdvd/stream_library_fixture.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/tests/cdvd/stream_persistence_case.cpp)
add_test(NAME StreamLibrary COMMAND test_stream_library)
set_tests_properties(StreamLibrary PROPERTIES TIMEOUT 60)

set(STREAM_ORACLE_DISC "${PS2R_OG_ROOT}/game_files/Crash_TwinSanity.rep/Crash Twinsanity.iso")
if(TARGET ps2r_title_recomp AND TARGET ps2r_title_stream_irx)   # title test: StreamFileSelfOracle links the generated EE code and the recompiled STREAM.IRX
add_executable(test_stream_file_oracle test_stream_file_oracle.cpp music_oracle_case.cpp bank_timing_oracle_case.cpp
    bank_pacing_oracle_case.cpp ${CMAKE_SOURCE_DIR}/tests/sound/pcm_source_fixture.cpp)
target_link_libraries(test_stream_file_oracle PRIVATE ps2r_title_hle ps2r_title_recomp
    ps2r_title_stream_irx ps2r_vu ps2r_gs_sync ps2r_cdvd ps2r_sif ps2r_ee ps2r_iop)
target_compile_options(test_stream_file_oracle PRIVATE -Wall -Wextra -Werror)
target_sources(test_stream_file_oracle PRIVATE stream_oracle_helpers.cpp
    stream_file_oracle_case.cpp bank_oracle_case.cpp sample_control_oracle_case.cpp)
if(EXISTS "${STREAM_ORACLE_DISC}")
    add_test(NAME StreamFileSelfOracle COMMAND test_stream_file_oracle "${STREAM_ORACLE_DISC}"
        "${PS2R_OG_ROOT}" "${CMAKE_SOURCE_DIR}/games/${PS2R_TITLE}/hle/sdk_bindings.tsv")
else()
    add_test(NAME StreamFileSelfOracle COMMAND test_stream_file_oracle)
endif()
set_tests_properties(StreamFileSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
else()
    message(STATUS "tests/services: StreamFileSelfOracle links the generated EE code and the recompiled STREAM.IRX SKIPPED (title inputs absent)")
endif()

add_executable(test_owned_audio test_audio.cpp)
target_link_libraries(test_owned_audio PRIVATE ps2r_title_audio ps2r_sound)
target_compile_options(test_owned_audio PRIVATE -Wall -Wextra -Werror)
add_test(NAME AudioSampleSelfOracle COMMAND test_owned_audio)
set_tests_properties(AudioSampleSelfOracle PROPERTIES TIMEOUT 60)

if(TARGET ps2r_title_recomp)   # title test: SoundOutput links the generated EE code
add_executable(test_sound_output test_sound_output.cpp ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/sound_output.cpp)
target_link_libraries(test_sound_output PRIVATE ps2r_title_hle ps2r_title_recomp ps2r_vu ps2r_gs_sync
    ps2r_sound ps2r_ee)
target_compile_options(test_sound_output PRIVATE -Wall -Wextra -Werror)
add_test(NAME SoundOutput COMMAND test_sound_output)
set_tests_properties(SoundOutput PROPERTIES TIMEOUT 60)
else()
    message(STATUS "tests/services: SoundOutput links the generated EE code SKIPPED (title inputs absent)")
endif()

if(TARGET ps2r_title_stream_irx)   # title test: AudioRampSelfOracle links the recompiled STREAM.IRX
add_executable(test_audio_ramp_oracle test_audio_ramp_oracle.cpp)
target_link_libraries(test_audio_ramp_oracle PRIVATE ps2r_title_audio ps2r_title_stream_irx ps2r_iop)
target_compile_options(test_audio_ramp_oracle PRIVATE -Wall -Wextra -Werror)
add_test(NAME AudioRampSelfOracle COMMAND test_audio_ramp_oracle "${PS2R_OG_ROOT}/game_files/STREAM.IRX")
set_tests_properties(AudioRampSelfOracle PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
else()
    message(STATUS "tests/services: AudioRampSelfOracle links the recompiled STREAM.IRX SKIPPED (title inputs absent)")
endif()

add_executable(test_sound_controls test_sound_controls.cpp)
target_sources(test_sound_controls PRIVATE control_state_case.cpp)
target_link_libraries(test_sound_controls PRIVATE ps2r_title_hle ps2r_ee)
target_compile_options(test_sound_controls PRIVATE -Wall -Wextra -Werror)
add_test(NAME SoundControls COMMAND test_sound_controls)
set_tests_properties(SoundControls PROPERTIES TIMEOUT 60)

add_executable(test_music_asset test_music_asset.cpp)
target_link_libraries(test_music_asset PRIVATE ps2r_title_audio)
target_compile_options(test_music_asset PRIVATE -Wall -Wextra -Werror)
add_test(NAME MusicAsset COMMAND test_music_asset)
set_tests_properties(MusicAsset PROPERTIES TIMEOUT 60)

add_executable(test_stream_buffers test_stream_buffers.cpp)
target_link_libraries(test_stream_buffers PRIVATE ps2r_title_hle)
target_compile_options(test_stream_buffers PRIVATE -Wall -Wextra -Werror)
add_test(NAME StreamBuffers COMMAND test_stream_buffers)
set_tests_properties(StreamBuffers PROPERTIES TIMEOUT 60)

add_executable(test_audio_state test_audio_state.cpp)
target_link_libraries(test_audio_state PRIVATE ps2r_title_audio)
target_compile_options(test_audio_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME AudioState COMMAND test_audio_state)
set_tests_properties(AudioState PROPERTIES TIMEOUT 60)

add_executable(test_bank_state test_bank_state.cpp ${CMAKE_SOURCE_DIR}/tests/cdvd/stream_library_fixture.cpp)
target_link_libraries(test_bank_state PRIVATE ps2r_title_hle ps2r_cdvd ps2r_ee)
target_compile_options(test_bank_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME BankState COMMAND test_bank_state)
set_tests_properties(BankState PROPERTIES TIMEOUT 60)

add_executable(test_buffer_state test_buffer_state.cpp)
target_link_libraries(test_buffer_state PRIVATE ps2r_title_hle)
target_compile_options(test_buffer_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME BufferState COMMAND test_buffer_state)
set_tests_properties(BufferState PROPERTIES TIMEOUT 60)
if(TARGET test_stream_file_oracle)   # a title test (gated above)
    target_sources(test_stream_file_oracle PRIVATE buffer_state_case.cpp service_state_case.cpp)
    target_sources(test_stream_file_oracle PRIVATE ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/session_services.cpp)
endif()
target_sources(test_stream_library PRIVATE ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/session_services.cpp)

add_executable(test_service_trace test_service_trace.cpp)
target_link_libraries(test_service_trace PRIVATE ps2r_title_hle)
target_compile_options(test_service_trace PRIVATE -Wall -Wextra -Werror)
add_test(NAME ServiceTrace COMMAND test_service_trace)
set_tests_properties(ServiceTrace PROPERTIES TIMEOUT 60)

add_executable(test_file_lifetime test_file_lifetime.cpp
    ${CMAKE_SOURCE_DIR}/tests/cdvd/stream_library_fixture.cpp)
target_link_libraries(test_file_lifetime PRIVATE ps2r_title_hle ps2r_cdvd ps2r_ee)
target_compile_options(test_file_lifetime PRIVATE -Wall -Wextra -Werror)
add_test(NAME FileLifetime COMMAND test_file_lifetime)
set_tests_properties(FileLifetime PROPERTIES TIMEOUT 60)

add_executable(test_files test_files.cpp files_fixture.cpp
    ${CMAKE_SOURCE_DIR}/tests/cdvd/stream_library_fixture.cpp)
target_link_libraries(test_files PRIVATE ps2r_title_hle ps2r_cdvd ps2r_ipu)
target_compile_options(test_files PRIVATE -Wall -Wextra -Werror)
add_test(NAME FilesSelfOracle COMMAND test_files)
set_tests_properties(FilesSelfOracle PROPERTIES TIMEOUT 60)

if(TARGET ps2r_title_recomp)   # title test: FileServices links the generated EE code
add_executable(test_file_services test_file_services.cpp files_fixture.cpp
    ${CMAKE_SOURCE_DIR}/tests/cdvd/stream_library_fixture.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/file_services.cpp)
target_link_libraries(test_file_services PRIVATE ps2r_title_hle ps2r_title_recomp ps2r_vu ps2r_gs_sync
    ps2r_cdvd ps2r_ipu ps2r_ee)
target_compile_options(test_file_services PRIVATE -Wall -Wextra -Werror)
add_test(NAME FileServices COMMAND test_file_services)
set_tests_properties(FileServices PROPERTIES TIMEOUT 60)
else()
    message(STATUS "tests/services: FileServices links the generated EE code SKIPPED (title inputs absent)")
endif()

if(TARGET ps2r_title_recomp)   # title test: ServiceRestore links the generated EE code
add_executable(test_service_restore test_service_restore.cpp
    ${CMAKE_SOURCE_DIR}/tests/cdvd/stream_library_fixture.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/service_restore.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/service_import.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/session_services.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/file_services.cpp)
target_link_libraries(test_service_restore PRIVATE ps2r_title_hle ps2r_title_recomp ps2r_vu ps2r_gs_sync
    ps2r_cdvd ps2r_ipu ps2r_ee ps2r_session_clock)
target_compile_options(test_service_restore PRIVATE -Wall -Wextra -Werror)
add_test(NAME ServiceRestore COMMAND test_service_restore)
set_tests_properties(ServiceRestore PROPERTIES TIMEOUT 90)
else()
    message(STATUS "tests/services: ServiceRestore links the generated EE code SKIPPED (title inputs absent)")
endif()

# The explicit legacy import over every inventoried frozen anchor (no game process).
if(TARGET ps2r_title_recomp)   # title test: ServiceImportAnchors links the generated EE code
add_executable(test_service_import_anchors test_service_import_anchors.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/service_import.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/session_services.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/file_services.cpp)
target_link_libraries(test_service_import_anchors PRIVATE ps2r_title_hle ps2r_title_recomp ps2r_vu ps2r_gs_sync
    ps2r_cdvd ps2r_ipu ps2r_ee)
target_compile_options(test_service_import_anchors PRIVATE -Wall -Wextra -Werror)
# The frozen coldboot probe dir is a private fixture (RAM images): resolved under $NATIVEEE_PRIVATE_FIXTURES.
if(EXISTS "${STREAM_ORACLE_DISC}")
    ps2r_add_private_fixture_test(NAME ServiceImportAnchors
        FIXTURES games/${PS2R_TITLE}/verify/baseline/coldboot/probe/old_b60.ps2st
        COMMAND test_service_import_anchors "${STREAM_ORACLE_DISC}"
        "${CMAKE_SOURCE_DIR}/games/${PS2R_TITLE}/verify/restore_mhash_expected.tsv" "${PS2R_OG_ROOT}"
        "{PRIVATE_FIXTURES}/games/${PS2R_TITLE}/verify/baseline/coldboot/probe")
else()
    add_test(NAME ServiceImportAnchors COMMAND test_service_import_anchors)
endif()
set_tests_properties(ServiceImportAnchors PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
else()
    message(STATUS "tests/services: ServiceImportAnchors links the generated EE code SKIPPED (title inputs absent)")
endif()

# The startup owner's reading of the contract above, as the root composes it.
add_executable(test_startup_ownership ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/tests/app/test_startup_ownership.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/startup_ownership.cpp)
target_link_libraries(test_startup_ownership PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_startup_ownership PRIVATE -Wall -Wextra -Werror)
add_test(NAME StartupOwnership COMMAND test_startup_ownership)
set_tests_properties(StartupOwnership PROPERTIES TIMEOUT 60)

# The F8 live restore's admission and route: the gate's
# four named refusals with a fake composition per term, the route under the
# all-on compiled defaults (live) and its fallback/setting arms.
add_executable(test_live_restore_gate ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/tests/app/test_live_restore_gate.cpp
    ${CMAKE_SOURCE_DIR}/app/host/live_restore_gate.cpp
    ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/startup_ownership.cpp)
target_link_libraries(test_live_restore_gate PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_live_restore_gate PRIVATE -Wall -Wextra -Werror)
add_test(NAME LiveRestoreGate COMMAND test_live_restore_gate)
set_tests_properties(LiveRestoreGate PROPERTIES TIMEOUT 60)
