# tools/vu1_replay -- the VU1 corpus oracle: one binary that proves the tiers
# alone (--mode / --compare) and the title's VU1 HLE library against the
# interpreter (--hle / --catalog). Built at -O2 with the same pins as ps2r_vu;
# the tool itself does no float math.

# The oracle's GIF-side helpers (decode / normalise / diff, the typed-op
# projection), shared with tests/vu.
add_library(ps2r_vu1_replay_lib STATIC
    gs_packet_diff.cpp
    gs_direct_ops_check.cpp
    xgk_capture_sink.cpp)
target_include_directories(ps2r_vu1_replay_lib PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(ps2r_vu1_replay_lib PRIVATE -Wall -Wextra -Werror -O2 -fno-strict-aliasing -fwrapv)
target_link_libraries(ps2r_vu1_replay_lib PUBLIC ps2r_vu ps2r_gs_ir)

# ps2r_vu carries ps2r_gif, ps2r_ee, ps2r_core and ps2r_gs_format as PUBLIC
# links; naming them again here makes ld warn about duplicate libraries.

# The tool links the configured title's VU1 program library (ps2r_title_vu1_library), so it exists
# only when a title that ships one is configured; consumers gate on if(TARGET vu1_replay).
if(TARGET ps2r_title_vu1_library)
    add_executable(vu1_replay
        vu1_replay.cpp)
    target_include_directories(vu1_replay PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(vu1_replay PRIVATE -Wall -Wextra -Werror -O2 -fno-strict-aliasing -fwrapv)
    # The title header the tool includes: games/<title>/vu1_library/vu1_library.h declares
    # Vu1Library::Get / Signature.
    target_compile_definitions(vu1_replay PRIVATE
        PS2R_TITLE_VU1_LIBRARY_H="games/${PS2R_TITLE}/vu1_library/vu1_library.h")
    target_link_libraries(vu1_replay PRIVATE ps2r_vu1_replay_lib ps2r_title_vu1_library)
else()
    message(STATUS "tools/vu1_replay: no title VU1 library -- vu1_replay not built (its corpus tests are skipped)")
endif()
