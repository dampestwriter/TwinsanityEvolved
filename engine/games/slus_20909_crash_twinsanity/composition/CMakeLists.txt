# games/slus_20909_crash_twinsanity/composition -- this title's PLAY COMPOSITION: the wiring
# that boots the recompiled EE code, the per-subsystem HLE, the ServiceStartup order, the
# launcher's first-run flow and the censuses. It defines TitleMain() (engine/title/
# title_composition.h); app/host/main.cpp hands it argc/argv when ps2r_title_composition exists.
#
# The same sources build two libraries:
#   ps2r_title_composition         -- the PLAY arm, service-only (PS2R_IOP_ORACLE=0): no IOP
#                                     kernel / module, no recompiled STREAM.IRX, no libsd model,
#                                     no SIF DMA model -> nativeee
#   ps2r_title_composition_oracle  -- the ORACLE arm (PS2R_IOP_ORACLE=1 through ps2r_iop_models):
#                                     the retained IOP-booting startup compiled in -> nativeee_oracle
#                                     (tests only; exists only when the recompiled STREAM.IRX does)
#
# Both need the recompiled EE code (ps2r_title_recomp), i.e. a title ELF (cmake/TitleInputs.cmake);
# without one the directory registers nothing and nativeee reports "no play composition".
# ps2r_input_save: the title's save/input HLE bound to the session clock (the product's save
# surface; was app/gui/input_save.cmake). Needs no recompiled code: the title tests link it too.
add_library(ps2r_input_save STATIC input_save.cpp)
target_include_directories(ps2r_input_save PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_features(ps2r_input_save PUBLIC cxx_std_17)
target_compile_options(ps2r_input_save PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ps2r_input_save PRIVATE ps2r_session_clock ps2r_title_hle ps2r_core)

if(NOT TARGET ps2r_title_recomp)
    message(STATUS "games/slus_20909_crash_twinsanity/composition: SKIPPED -- the play composition needs the title's recompiled EE code (give -DNATIVEEE_DISC or -DNATIVEEE_TITLE_ELF)")
    return()
endif()

set(PS2R_TITLE_COMPOSITION_SOURCES
    title_main.cpp
    # app/host's arm-dependent census (iop_arm.h): compiled here once per arm, not in ps2r_app_host.
    ${CMAKE_SOURCE_DIR}/app/host/subsystem_census.cpp
    menu_composition.cpp
    fixes_composition.cpp
    ee_bootstrap.cpp
    ee_bootstrap_devices.cpp
    ee_bootstrap_services.cpp
    ee_bootstrap_play.cpp
    ee_bootstrap_image.cpp
    ee_bootstrap_sections.cpp
    ee_bootstrap_census.cpp
    ee_bootstrap_leg.cpp
    input_sample.cpp
    display_rows.cpp
    video_stack.cpp
    video_census.cpp
    frame_census.cpp
    session_capture.cpp
    savestate_trigger.cpp
    movie_checkpoint.cpp
    movie_checkpoint_prepare.cpp
    movie_checkpoint_legacy.cpp
    movie_boot.cpp
    movie_sound.cpp
    graphics_output.cpp
    graphics_output_unlinked.cpp
    graphics_strip.cpp
    graphics_producers.cpp
    graphics_producer_draws.cpp
    mesh_producer.cpp
    graphics_runtime.cpp
    graphics_import.cpp
    graphics_checkpoint.cpp
    graphics_checkpoint_presentation.cpp
    graphics_frame.cpp
    graphics_census.cpp
    graphics_movie.cpp
    graphics_application.cpp
    sound_output.cpp
    service_timeline.cpp
    session_services.cpp
    service_clock.cpp
    service_restore.cpp
    service_import.cpp
    movie_storage.cpp
    service_trace_log.cpp
    startup_ownership.cpp
    native_startup.cpp
    file_services.cpp
    # The effect applications (shadow bucket 21, distortion bucket 23, screen bucket 4).
    shadow_application.cpp
    shadow_census.cpp
    distortion_application.cpp
    distortion_census.cpp
    screen_application.cpp
    effects_application.cpp)

# ps2r_add_title_composition(<target> <iop_oracle 0|1>)
function(ps2r_add_title_composition target iop_oracle)
    add_library(${target} STATIC ${PS2R_TITLE_COMPOSITION_SOURCES})
    target_include_directories(${target} PUBLIC ${CMAKE_SOURCE_DIR})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(${target} PRIVATE PS2R_TITLE_ID="${PS2R_TITLE}"
        PS2R_TITLE_DIR="${CMAKE_SOURCE_DIR}/games/${PS2R_TITLE}")
    # The title-neutral host services (app/host: launcher, first run, user data, session
    # services, shutdown ladder) plus this title's libraries and the engine tiers they compose.
    # ps2r_iop PUBLIC-links ps2r_platform, which PUBLIC-links ps2r_core; naming a library twice
    # makes ld warn about a duplicate, so only the leaves are named.
    target_link_libraries(${target} PUBLIC ps2r_app_host ps2r_title_api
        ps2r_title_hle ps2r_title_recomp ps2r_title_vu1_library ps2r_gs_sync ps2r_gif_hle ps2r_dmac
        ps2r_cdvd ps2r_mc ps2r_pad ps2r_sound ps2r_sound_hle ps2r_gs_hle_queue
        ps2r_title_renderer_profile ps2r_render_gl ps2r_render_review ps2r_platform_sdl ps2r_ipu ps2r_imgui
        ps2r_session_runtime ps2r_session_checkpoint ps2r_session_restart ps2r_session_clock ps2r_input_save)
    if(iop_oracle)
        target_link_libraries(${target} PUBLIC ps2r_iop_models)   # defines PS2R_IOP_ORACLE=1
    else()
        target_compile_definitions(${target} PRIVATE PS2R_IOP_ORACLE=0)
    endif()
    if(PS2R_DIAGNOSTICS)
        target_compile_definitions(${target} PRIVATE PS2R_DIAGNOSTICS=1)
        target_link_libraries(${target} PUBLIC ps2r_diag)
    endif()
endfunction()

ps2r_add_title_composition(ps2r_title_composition 0)
# The ORACLE arm links the recompiled STREAM.IRX (ps2r_iop_models -> ps2r_title_stream_irx):
# only when that module is being generated (a disc image or STREAM.IRX was given, TitleInputs).
if(TARGET ps2r_title_stream_irx)
    ps2r_add_title_composition(ps2r_title_composition_oracle 1)
else()
    message(STATUS "games/slus_20909_crash_twinsanity/composition: the oracle arm (nativeee_oracle) SKIPPED -- needs the recompiled STREAM.IRX (give -DNATIVEEE_DISC or -DNATIVEEE_STREAM_IRX)")
endif()
