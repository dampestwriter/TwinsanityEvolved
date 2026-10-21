# games/<title>/hle -- title-side HLE data: TitleProfile (identity, ELF facts, boot env)
# and the SDK binding manifest file sdk_bindings.tsv (data, read at composition time);
# the title hooks over engine protocols (PadReadinessHook: the boot UI's case-2 entry;
# loader_sync/LoaderSpinLift: the load-wait poll spin bound to LoaderModel's sync-wait rule;
# fmv/FmvContextLayoutData: the title's SceMpeg context layout for the engine's sceMpeg HLE;
# workarounds/FmvAbortHold: the scene-updater abort hold over MpegApi::ShouldHoldAbort).
add_library(ps2r_title_hle STATIC title_profile.cpp pad_readiness_hook.cpp sif_init_statics.cpp loader_sync/loader_file_io.cpp loader_sync/loader_spin_lift.cpp loader_sync/chunk_state_census.cpp workarounds/meta_arena.cpp
    workarounds/free_list_0x181560.cpp workarounds/key_redirect.cpp workarounds/fmv_abort_hold.cpp
    fmv/fmv_context_layout_data.cpp fmv/movie_decoder.cpp fmv/movie_graphics.cpp fmv/movie_stream.cpp
    fmv/movie_stream_buffers.cpp
    fmv/movie_audio.cpp
    fmv/movie_step.cpp
    fmv/movie_start.cpp
    fmv/movie_wait.cpp
    fmv/movie_stop.cpp
    math/math_library.cpp math/collision_math.cpp math/trig_math.cpp math/trig_math_rotations.cpp math/animation_math.cpp
    math/math_library_bindings.cpp math/collision_math_bindings.cpp
    math/trig_math_bindings.cpp math/animation_math_bindings.cpp
    math/culling_math.cpp
    math/culling_state.cpp
    math/culling_state_codec.cpp
    math/culling_planes.cpp
    math/culling_library.cpp
    math/culling_library_bindings.cpp
    math/culling_checkpoint.cpp
    math/bounds_math.cpp math/bounds_math_bindings.cpp
    math/clipping_math.cpp math/clipping_math_bindings.cpp
    math/joint_math.cpp math/joint_library.cpp math/joint_library_bindings.cpp
    math/ray_math.cpp
    math/particle_view_math.cpp
    math/decal_math.cpp
    game_libraries.cpp stream/stream_library.cpp
    graphics/graphics.cpp graphics/graphics_resources.cpp graphics/graphics_buckets.cpp
    graphics/graphics_targets.cpp graphics/graphics_renderer.cpp
    graphics/graphics_surfaces.cpp graphics/graphics_scene.cpp
    graphics/graphics_scene_state.cpp graphics/scene_state.cpp
    graphics/graphics_display.cpp
    graphics/graphics_presentation.cpp graphics/graphics_presentation_state.cpp
    graphics/presentation_state.cpp
    graphics/graphics_bridge.cpp graphics/graphics_service.cpp
    graphics/material_state.cpp graphics/strip_draw.cpp graphics/strip_intent.cpp graphics/rounding_guard.cpp
    graphics/sky_intent.cpp graphics/sky_service.cpp graphics/sky_geometry.cpp graphics/mesh_geometry.cpp graphics/sky_texture.cpp
    graphics/sky_setup.cpp graphics/sky_assets.cpp graphics/sky_material.cpp graphics/sky_programs.cpp graphics/sky_pass.cpp
    graphics/sky_assets_state.cpp graphics/sky_state.cpp
    graphics/mesh_assets.cpp graphics/mesh_intent.cpp graphics/mesh_material.cpp graphics/mesh_pass.cpp
    graphics/skin_geometry.cpp
    graphics/frame_environment.cpp graphics/frame_state.cpp graphics/frame_intent.cpp
    graphics/frame_clear.cpp
    graphics/display_layout.cpp
    graphics/screen_aspect.cpp
    graphics/graphics_scene_frame.cpp
    graphics/graphics_scene_strip.cpp
    graphics/frame_service.cpp
    graphics/graphics_report.cpp)
target_include_directories(ps2r_title_hle PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(ps2r_title_hle PRIVATE -Wall -Wextra -Werror)
# The accepted trig operation uses production paired libm lowering. Debug's
# separate sinf/cosf calls differ by one bit on Apple Clang; keep the same
# arithmetic compilation contract as the retained NEW oracle in every preset.
set_source_files_properties(math/trig_math.cpp math/trig_math_rotations.cpp PROPERTIES
    COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-fwrapv;-ffp-contract=off")
set_source_files_properties(math/culling_math.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set_source_files_properties(math/culling_planes.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set_source_files_properties(math/bounds_math.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set_source_files_properties(math/ray_math.cpp math/particle_view_math.cpp math/decal_math.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set_source_files_properties(math/clipping_math.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set_source_files_properties(math/joint_math.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set_source_files_properties(graphics/mesh_geometry.cpp graphics/mesh_intent.cpp graphics/sky_geometry.cpp graphics/sky_setup.cpp graphics/skin_geometry.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
# The skin path (SkinGeometry) runs the VU1 library's own skinned program bodies over a host data image.
target_link_libraries(ps2r_title_hle PUBLIC ps2r_title_api ps2r_ee ps2r_pad ps2r_sif ps2r_cdvd ps2r_ipu ps2r_sound ps2r_gs_ir ps2r_gs_format ps2r_gs_hle_classrdr ps2r_title_vu1_library)
add_subdirectory(scheduler)
include(${CMAKE_CURRENT_LIST_DIR}/input/input_saves.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/menu/menu.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/fixes/fixes.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/qol/qol.cmake)
add_subdirectory(stream)
add_subdirectory(audio)
include(${CMAKE_CURRENT_LIST_DIR}/graphics/textures.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/graphics/primitives/primitives.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/graphics/text/text.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/graphics/effects/effects.cmake)
