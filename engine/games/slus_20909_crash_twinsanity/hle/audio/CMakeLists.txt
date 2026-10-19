add_library(ps2r_title_audio STATIC audio.cpp sample_voice.cpp volume_ramp.cpp music_asset.cpp)
target_include_directories(ps2r_title_audio PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(ps2r_title_audio PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ps2r_title_audio PUBLIC ps2r_sound)
target_sources(ps2r_title_audio PRIVATE audio_state.cpp ../scheduler/state_bytes.cpp)
target_link_libraries(ps2r_title_hle PUBLIC ps2r_title_audio)
# The ABI adapter consumes both file ownership and pure Audio. Keeping it in
# hle avoids a static-library cycle from the reusable playback algorithms.
target_sources(ps2r_title_hle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sound_banks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sound_controls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/music_library.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/music_buffers.cpp)
target_sources(ps2r_title_hle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bank_state.cpp)
target_sources(ps2r_title_hle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/control_state.cpp)
target_sources(ps2r_title_hle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/music_state.cpp)
target_sources(ps2r_title_hle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/service_state.cpp)
target_sources(ps2r_title_hle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sound_effects.cpp)
