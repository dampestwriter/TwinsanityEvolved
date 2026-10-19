# The title-bound rows of tests/render: the rendering pins over this title's frozen PCSX2
# baselines (title-screen draws, whole-frame replays, the attract-sequence correctness gate).
# The baselines are game bytes and live OUTSIDE the public tree: private fixtures
# (cmake/Ps2rPrivateFixtures.cmake) resolved at run time under
# $NATIVEEE_PRIVATE_FIXTURES/<repo-relative path>; the rows SKIP (77) without them.
include_guard(GLOBAL)
set(_render "${PS2R_TITLE_TESTS}/render")
set(PS2R_BASELINE_REL games/${PS2R_TITLE}/verify/baseline)
set(PS2R_BASELINE_ROOT {PRIVATE_FIXTURES}/${PS2R_BASELINE_REL})

# One title-screen draw from the PCSX2 dump through the full lowering (pipeline over GlDevice).
add_executable(test_title_logo_draw ${_render}/test_title_logo_draw.cpp)
target_link_libraries(test_title_logo_draw PRIVATE ps2r_title_renderer_profile ps2r_render_gl ps2r_platform_sdl ps2r_gs_xfer)
target_compile_options(test_title_logo_draw PRIVATE -Wall -Wextra -Werror -O2)
ps2r_add_private_fixture_test(NAME Render_TitleLogoDrawPin
    FIXTURES ${PS2R_BASELINE_REL}/title_screen/logo_atlas_page_f1_xfer243.bin
             ${PS2R_BASELINE_REL}/title_screen/logo_draw167_f1_verts.bin
    COMMAND test_title_logo_draw ${PS2R_BASELINE_ROOT}/title_screen)
set_tests_properties(Render_TitleLogoDrawPin PROPERTIES TIMEOUT 120)

# A whole dump frame (uploads + draws in stream order) through the full lowering, measured
# against the console's screenshot regions (the title-screen lowering gate; also an ablation tool).
add_executable(test_title_frame_replay ${_render}/test_title_frame_replay.cpp)
target_link_libraries(test_title_frame_replay PRIVATE ps2r_title_renderer_profile ps2r_render_gl ps2r_platform_sdl ps2r_gs_xfer)
target_compile_options(test_title_frame_replay PRIVATE -Wall -Wextra -Werror -O2)
ps2r_add_private_fixture_test(NAME Render_TitleFrameReplay
    FIXTURES ${PS2R_BASELINE_REL}/title_screen/pcsx2_start_screen_f1.gsf1
    COMMAND test_title_frame_replay
    ${PS2R_BASELINE_ROOT}/title_screen/pcsx2_start_screen_f1.gsf1 --scanout 70000 --gate)
# The same frame twice with a frame boundary between the passes, grabbed at the DISPFB base.
ps2r_add_private_fixture_test(NAME Render_TitleFrameReplayDisplay
    FIXTURES ${PS2R_BASELINE_REL}/title_screen/pcsx2_start_screen_f1.gsf1
    COMMAND test_title_frame_replay
    ${PS2R_BASELINE_ROOT}/title_screen/pcsx2_start_screen_f1.gsf1 --repeat 2 --scanout 0 --gate)
set_tests_properties(Render_TitleFrameReplay Render_TitleFrameReplayDisplay PROPERTIES TIMEOUT 120)

# The rendering correctness gate over the coldboot_attract dumps: each dump's frame 1 is
# exported at test time by tools/gsdump/gsf1_export.py, replayed through the full lowering and
# measured against the dump's own screenshot.
# The pin's judge (its REGIONS come from this title's tests/app/root_attract_cutscene_frames.py).
add_test(NAME Render_AttractReplaySelftest
    COMMAND ${Python3_EXECUTABLE} ${_render}/attract_replay_pin.py --selftest)
set_tests_properties(Render_AttractReplaySelftest PROPERTIES TIMEOUT 60)
set(PS2R_ATTRACT_DUMPS_REL ${PS2R_BASELINE_REL}/coldboot_attract/gsdumps)
set(PS2R_ATTRACT_DUMPS ${PS2R_BASELINE_ROOT}/coldboot_attract/gsdumps)
set(PS2R_ATTRACT_SCRATCH ${CMAKE_BINARY_DIR}/test_scratch/render/attract_replay)
foreach(dump attract_start_f2845 attract_mid1_f3300 attract_mid2_f3600 attract_end_f3945)
    ps2r_add_private_fixture_test(NAME Render_AttractReplay_${dump}
        FIXTURES ${PS2R_ATTRACT_DUMPS_REL}/${dump}.gs.zst ${PS2R_ATTRACT_DUMPS_REL}/${dump}_shot.ppm
        COMMAND ${Python3_EXECUTABLE} ${_render}/attract_replay_pin.py --kind cutscene
                --dump ${PS2R_ATTRACT_DUMPS}/${dump}.gs.zst --shot ${PS2R_ATTRACT_DUMPS}/${dump}_shot.ppm
                --exporter ${CMAKE_SOURCE_DIR}/tools/gsdump/gsf1_export.py
                --replay $<TARGET_FILE:test_title_frame_replay> --scratch ${PS2R_ATTRACT_SCRATCH}
                --tol 12 --caption-min 0.02 --black-max 8)
    set_tests_properties(Render_AttractReplay_${dump} PROPERTIES TIMEOUT 120)
endforeach()
ps2r_add_private_fixture_test(NAME Render_AttractReplay_title_return_f4200
    FIXTURES ${PS2R_ATTRACT_DUMPS_REL}/title_return_f4200.gs.zst ${PS2R_ATTRACT_DUMPS_REL}/title_return_f4200_shot.ppm
    COMMAND ${Python3_EXECUTABLE} ${_render}/attract_replay_pin.py --kind title
            --dump ${PS2R_ATTRACT_DUMPS}/title_return_f4200.gs.zst --shot ${PS2R_ATTRACT_DUMPS}/title_return_f4200_shot.ppm
            --exporter ${CMAKE_SOURCE_DIR}/tools/gsdump/gsf1_export.py
            --replay $<TARGET_FILE:test_title_frame_replay> --scratch ${PS2R_ATTRACT_SCRATCH}
            --logo-tol 8 --share-tol 0.15)
set_tests_properties(Render_AttractReplay_title_return_f4200 PROPERTIES TIMEOUT 120)
