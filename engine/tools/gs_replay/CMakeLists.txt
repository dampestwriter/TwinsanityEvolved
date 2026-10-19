# tools/gs_replay -- the corpus replay oracle (replay / census / diff / classdiff)
# over the NEW GS HLE layers and the GL backend. A composition root like app/host:
# it may link engine/* and the title's renderer profile. Output grammar is the
# corpus-pinned one; tests/gs/replay compares it against the frozen OLD baselines.
#
# The tool links the configured title's renderer profile (ps2r_title_renderer_profile), so it
# exists only when a title that ships one is configured; consumers gate on if(TARGET gs_replay).
if(TARGET ps2r_title_renderer_profile)
    add_executable(gs_replay
        gs_replay.cpp
        replay_driver.cpp
        census.cpp
        gsrep_file.cpp
        replay_diff.cpp
        skip_filter.cpp)
    target_include_directories(gs_replay PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(gs_replay PRIVATE -Wall -Wextra -Werror -O2)
    # The title header the tool includes: games/<title>/renderer_profile/renderer_profile.h declares
    # ps2r::title::RendererProfile.
    target_compile_definitions(gs_replay PRIVATE
        PS2R_TITLE_RENDERER_PROFILE_H="games/${PS2R_TITLE}/renderer_profile/renderer_profile.h")
    target_link_libraries(gs_replay PRIVATE ps2r_title_renderer_profile ps2r_render_gl ps2r_platform_sdl
                                            ps2r_gsdl_reader)
else()
    message(STATUS "tools/gs_replay: no title renderer profile -- gs_replay not built (its corpus tests are skipped)")
endif()
