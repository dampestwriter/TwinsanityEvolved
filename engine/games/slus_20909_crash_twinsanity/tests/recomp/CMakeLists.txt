# tests/recomp -- the EE recompiler (tools/recompiler_v2).
#
# The byte-for-byte reproduction of games/<title>/recomp/ by this tool is checked out of band
# (tools/regen_from_frozen.sh --tool ...): at ~10 s per run and 91 MB of output it is not a ctest.

# RecompDiscoveryDelaySlotRule: a synthetic fixture (the retail 0x178470
# shape: prologue in the guard branch's delay slot, pointer-seeded function) is discovered as ONE
# function with a leader at the fall-through; the delay-slot word is never a function start.
add_executable(test_discovery_delay_slot test_discovery_delay_slot.cpp)
target_include_directories(test_discovery_delay_slot PRIVATE ${CMAKE_SOURCE_DIR}/tools/recompiler_v2)
target_link_libraries(test_discovery_delay_slot PRIVATE ps2r_rv2_engine)
target_compile_options(test_discovery_delay_slot PRIVATE -Wall -Wextra -Werror)
add_test(NAME RecompDiscoveryDelaySlotRule COMMAND test_discovery_delay_slot)
set_tests_properties(RecompDiscoveryDelaySlotRule PROPERTIES TIMEOUT 60)

add_executable(test_math_library test_math_library.cpp math_library_dispatch_fixture.cpp)
target_link_libraries(test_math_library PRIVATE ps2r_title_hle)
target_compile_options(test_math_library PRIVATE -Wall -Wextra -Werror)
add_test(NAME MathLibrary COMMAND test_math_library)
set_tests_properties(MathLibrary PROPERTIES TIMEOUT 60)
# The generated-body oracles extract function bodies from the GENERATED shards (the build directory,
# ${PS2R_RECOMP_GEN_DIR}; TitleInputs): title tests, included only when the title is being generated.
# The fragments that read only the reviewed ELF (PS2R_MATH_ORACLE_ELF) or no game file at all stay.
set(_ps2r_title_gen FALSE)
if(TARGET ps2r_title_recomp)
    set(_ps2r_title_gen TRUE)
else()
    message(STATUS "tests/recomp: the generated-body self-oracles (math library, movie, strip, sky, mesh, frame, display) are SKIPPED (no title ELF)")
endif()
if(_ps2r_title_gen)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_oracle.cmake)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/math_library_collision_oracle.cmake)   # gates its generated half itself
if(_ps2r_title_gen)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_trig_oracle.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_animation_oracle.cmake)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/math_library_culling_oracle.cmake)
if(_ps2r_title_gen)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_planes_oracle.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_culling_adapter_oracle.cmake)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/math_library_culling_checkpoint.cmake)
if(_ps2r_title_gen)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_bounds_oracle.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_clipping_oracle.cmake)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/math_library_joint_oracle.cmake)
if(_ps2r_title_gen)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_joint_adapter_oracle.cmake)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/math_library_joint_matrix_oracle.cmake)
if(_ps2r_title_gen)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_joint_transform_oracle.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_joint_lerp_oracle.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/math_library_joint_animate_oracle.cmake)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/math_library_bindings.cmake)

add_executable(test_movie_decoder test_movie_decoder.cpp)
target_link_libraries(test_movie_decoder PRIVATE ps2r_title_hle)
target_compile_options(test_movie_decoder PRIVATE -Wall -Wextra -Werror)
add_test(NAME MovieDecoder COMMAND test_movie_decoder)
set_tests_properties(MovieDecoder PROPERTIES TIMEOUT 60)

add_executable(test_movie_decoder_services test_movie_decoder_services.cpp
    ${CMAKE_SOURCE_DIR}/tests/ipu/vector_guest_memory.cpp ${CMAKE_SOURCE_DIR}/tests/ipu/file_fmv_sector_source.cpp
    ${CMAKE_SOURCE_DIR}/tests/ipu/recording_dmac_channel_face.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_checkpoint.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_sound.cpp")
target_link_libraries(test_movie_decoder_services PRIVATE ps2r_title_hle ps2r_ipu_model ps2r_sound_hle)
target_compile_options(test_movie_decoder_services PRIVATE -Wall -Wextra -Werror)
add_test(NAME MovieDecoderServices COMMAND test_movie_decoder_services "${PS2R_OG_ROOT}/game_files/Crash_TwinSanity.rep")
set_tests_properties(MovieDecoderServices PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
add_executable(test_movie_checkpoint test_movie_checkpoint.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_checkpoint.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_checkpoint_legacy.cpp")
target_link_libraries(test_movie_checkpoint PRIVATE ps2r_title_hle ps2r_ipu ps2r_sound)
target_compile_options(test_movie_checkpoint PRIVATE -Wall -Wextra -Werror)
add_test(NAME MovieCheckpoint COMMAND test_movie_checkpoint)
set_tests_properties(MovieCheckpoint PROPERTIES TIMEOUT 120)
add_executable(test_movie_boot test_movie_boot.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_checkpoint.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_checkpoint_prepare.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_checkpoint_legacy.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_boot.cpp")
target_link_libraries(test_movie_boot PRIVATE ps2r_title_hle ps2r_ipu ps2r_sound
    ps2r_input_save ps2r_session_clock)
target_compile_options(test_movie_boot PRIVATE -Wall -Wextra -Werror)
add_test(NAME MovieBoot COMMAND test_movie_boot)
set_tests_properties(MovieBoot PROPERTIES TIMEOUT 120)
add_executable(test_movie_service_checkpoint test_movie_service_checkpoint.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_checkpoint.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_checkpoint_presentation.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_checkpoint.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_checkpoint_prepare.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/service_clock.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/session_services.cpp")
target_link_libraries(test_movie_service_checkpoint PRIVATE ps2r_title_hle ps2r_ipu ps2r_sound
    ps2r_session_clock ps2r_fake_backend)
target_compile_options(test_movie_service_checkpoint PRIVATE -Wall -Wextra -Werror)
add_test(NAME MovieServiceCheckpoint COMMAND test_movie_service_checkpoint)
set_tests_properties(MovieServiceCheckpoint PROPERTIES TIMEOUT 120)
add_executable(test_movie_audio test_movie_audio.cpp ${CMAKE_SOURCE_DIR}/tests/sound/mixer_voice_source.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_sound.cpp")
target_link_libraries(test_movie_audio PRIVATE ps2r_title_hle ps2r_sound)
target_compile_options(test_movie_audio PRIVATE -Wall -Wextra -Werror)
add_test(NAME MovieAudio COMMAND test_movie_audio --both-paths)
set_tests_properties(MovieAudio PROPERTIES TIMEOUT 60)
add_executable(test_graphics_movie test_graphics_movie.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_movie.cpp")
target_link_libraries(test_graphics_movie PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_graphics_movie PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsMovie COMMAND test_graphics_movie "${CMAKE_CURRENT_BINARY_DIR}/graphics_movie_scratch")
set_tests_properties(GraphicsMovie PROPERTIES TIMEOUT 60 LABELS "graphics")
# ScreenAspect: the title's screen-shape term at the four NTSC
# entries, the HUD fits' native arithmetic (== the generated order at 16:9), the
# movie picture at the ratio, the AspectSelection snap.
add_executable(test_screen_aspect test_screen_aspect.cpp)
target_link_libraries(test_screen_aspect PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_screen_aspect PRIVATE -Wall -Wextra -Werror)
add_test(NAME ScreenAspect COMMAND test_screen_aspect)
set_tests_properties(ScreenAspect PROPERTIES TIMEOUT 60 LABELS "graphics")
add_executable(test_movie_stream test_movie_stream.cpp)
target_link_libraries(test_movie_stream PRIVATE ps2r_title_hle)
target_compile_options(test_movie_stream PRIVATE -Wall -Wextra -Werror)
add_test(NAME MovieStream COMMAND test_movie_stream --both-paths)
set_tests_properties(MovieStream PROPERTIES TIMEOUT 120)
if(TARGET ps2r_title_recomp)   # generated-body oracle (title test)
set(movie_buffers_oracle_dir "${CMAKE_CURRENT_BINARY_DIR}/movie_buffers_oracle")
add_custom_command(
    OUTPUT "${movie_buffers_oracle_dir}/movie_buffers_generated.cpp"
           "${movie_buffers_oracle_dir}/movie_buffers_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_movie_buffers_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${movie_buffers_oracle_dir}"
    DEPENDS ps2r_title_recomp_gen generate_movie_buffers_oracle.py
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_029.cpp"
    VERBATIM)
add_executable(test_movie_buffers test_movie_buffers.cpp "${movie_buffers_oracle_dir}/movie_buffers_generated.cpp")
set_source_files_properties("${movie_buffers_oracle_dir}/movie_buffers_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces")
target_link_libraries(test_movie_buffers PRIVATE ps2r_title_hle ps2r_iop)
target_compile_options(test_movie_buffers PRIVATE -Wall -Wextra -Werror)
add_test(NAME MovieBuffers COMMAND test_movie_buffers --both-paths)
set_tests_properties(MovieBuffers PROPERTIES TIMEOUT 120)
endif()

# The oracle hashes actual output through avutil; imported FFmpeg targets in
# engine/ipu are directory-scoped, so declare this direct test dependency here.
include(${CMAKE_SOURCE_DIR}/cmake/FindFFmpeg.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/movie_oracle_args.cmake)
if(TARGET ps2r_title_recomp)   # generated-body oracles (title tests)
movie_oracle_export_args(movie_lifecycle_export MovieLifecycleOracle)
movie_oracle_export_args(movie_graphics_export MovieGraphicsSelfOracle)
set(movie_oracle_dir "${CMAKE_CURRENT_BINARY_DIR}/movie_oracle")
add_custom_command(
    OUTPUT "${movie_oracle_dir}/movie_generated.cpp" "${movie_oracle_dir}/movie_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_movie_decoder_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${movie_oracle_dir}"
    DEPENDS ps2r_title_recomp_gen generate_movie_decoder_oracle.py
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_029.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_030.cpp"
    VERBATIM)
add_executable(test_movie_decoder_oracle test_movie_decoder_oracle.cpp
    ${CMAKE_SOURCE_DIR}/tests/ipu/recording_dmac_channel_face.cpp "${movie_oracle_dir}/movie_generated.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/movie_sound.cpp")
set_source_files_properties("${movie_oracle_dir}/movie_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces")
# The oracles use libavutil's headers; the archive itself reaches the link line once, through
# ps2r_ipu's PRIVATE dependency (a second, direct mention is a duplicate archive the Apple linker
# warns about under the static macos-dist preset).
target_link_libraries(test_movie_decoder_oracle PRIVATE
    ps2r_title_hle ps2r_ipu_model ps2r_sound_hle)
target_include_directories(test_movie_decoder_oracle PRIVATE $<TARGET_PROPERTY:FFmpeg::avutil,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(test_movie_decoder_oracle PRIVATE -Wall -Wextra -Werror)
add_test(NAME MovieLifecycleOracle COMMAND test_movie_decoder_oracle
    "${PS2R_OG_ROOT}/game_files/Crash_TwinSanity.rep" --both-paths ${movie_lifecycle_export})
set_tests_properties(MovieLifecycleOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

set(movie_graphics_oracle_dir "${CMAKE_CURRENT_BINARY_DIR}/movie_graphics_oracle")
add_custom_command(
    OUTPUT "${movie_graphics_oracle_dir}/movie_graphics_generated.cpp"
           "${movie_graphics_oracle_dir}/movie_graphics_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_movie_graphics_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${movie_graphics_oracle_dir}"
    DEPENDS ps2r_title_recomp_gen generate_movie_graphics_oracle.py
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_029.cpp"
    VERBATIM)
add_executable(test_movie_graphics_oracle test_movie_graphics_oracle.cpp
    "${movie_graphics_oracle_dir}/movie_graphics_generated.cpp")
set_source_files_properties("${movie_graphics_oracle_dir}/movie_graphics_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces;-ffp-contract=off")
target_link_libraries(test_movie_graphics_oracle PRIVATE
    ps2r_title_hle ps2r_dmac ps2r_gif_hle ps2r_fake_backend)
target_include_directories(test_movie_graphics_oracle PRIVATE $<TARGET_PROPERTY:FFmpeg::avutil,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(test_movie_graphics_oracle PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
add_test(NAME MovieGraphicsSelfOracle COMMAND test_movie_graphics_oracle --both-paths ${movie_graphics_export})
set_tests_properties(MovieGraphicsSelfOracle PROPERTIES TIMEOUT 180 LABELS "graphics;self-oracle")

set(movie_step_dir "${CMAKE_CURRENT_BINARY_DIR}/movie_step_oracle")
add_custom_command(
    OUTPUT "${movie_step_dir}/movie_step_generated.cpp" "${movie_step_dir}/movie_step_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_movie_step_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${movie_step_dir}"
    DEPENDS ps2r_title_recomp_gen generate_movie_step_oracle.py "${CMAKE_SOURCE_DIR}/docs/design/movie_caller_inventory.json"
        "${PS2R_RECOMP_GEN_DIR}/recompiled.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_029.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_030.cpp"
    VERBATIM)
add_executable(test_movie_step_oracle test_movie_step_oracle.cpp "${movie_step_dir}/movie_step_generated.cpp")
set_source_files_properties("${movie_step_dir}/movie_step_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces")
target_link_libraries(test_movie_step_oracle PRIVATE ps2r_title_hle ps2r_gs_sync)
target_include_directories(test_movie_step_oracle PRIVATE $<TARGET_PROPERTY:FFmpeg::avutil,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(test_movie_step_oracle PRIVATE -Wall -Wextra -Werror)
movie_oracle_export_args(movie_step_export MovieStepOracle)
add_test(NAME MovieStepOracle COMMAND test_movie_step_oracle --both-paths ${movie_step_export})
set_tests_properties(MovieStepOracle PROPERTIES TIMEOUT 120 LABELS "graphics;self-oracle")
movie_oracle_export_args(movie_start_export MovieStartOracle)
add_test(NAME MovieStartOracle COMMAND test_movie_step_oracle --start-paths ${movie_start_export})
set_tests_properties(MovieStartOracle PROPERTIES TIMEOUT 120 LABELS "graphics;self-oracle")
movie_oracle_export_args(movie_wait_export MovieWaitOracle)
add_test(NAME MovieWaitOracle COMMAND test_movie_step_oracle --wait-paths ${movie_wait_export})
set_tests_properties(MovieWaitOracle PROPERTIES TIMEOUT 120 LABELS "graphics;self-oracle")
movie_oracle_export_args(movie_stop_export MovieStopOracle)
add_test(NAME MovieStopOracle COMMAND test_movie_step_oracle --stop-paths ${movie_stop_export})
set_tests_properties(MovieStopOracle PROPERTIES TIMEOUT 120 LABELS "graphics;self-oracle")
endif()

add_executable(test_graphics_submission test_graphics_submission.cpp)
target_link_libraries(test_graphics_submission PRIVATE ps2r_title_hle ps2r_gs_hle_queue)
target_compile_options(test_graphics_submission PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsSubmission COMMAND test_graphics_submission)
set_tests_properties(GraphicsSubmission PROPERTIES TIMEOUT 60)

add_executable(test_graphics_resources test_graphics_resources.cpp)
target_link_libraries(test_graphics_resources PRIVATE ps2r_title_hle ps2r_fake_backend)
target_compile_options(test_graphics_resources PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsResources COMMAND test_graphics_resources)
set_tests_properties(GraphicsResources PROPERTIES TIMEOUT 60)

add_executable(test_graphics_renderer test_graphics_renderer.cpp)
target_link_libraries(test_graphics_renderer PRIVATE ps2r_title_hle ps2r_fake_backend)
target_compile_options(test_graphics_renderer PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsRenderer COMMAND test_graphics_renderer)
set_tests_properties(GraphicsRenderer PROPERTIES TIMEOUT 60)

add_executable(test_graphics_scene test_graphics_scene.cpp)
target_link_libraries(test_graphics_scene PRIVATE ps2r_title_hle ps2r_fake_backend ps2r_gs_hle_queue)
target_compile_options(test_graphics_scene PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsScene COMMAND test_graphics_scene)
include(graphics_runtime.cmake)
set_tests_properties(GraphicsScene PROPERTIES TIMEOUT 60)

add_executable(test_graphics_buckets test_graphics_buckets.cpp)
target_link_libraries(test_graphics_buckets PRIVATE ps2r_title_hle)
target_compile_options(test_graphics_buckets PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsBuckets COMMAND test_graphics_buckets)
set_tests_properties(GraphicsBuckets PROPERTIES TIMEOUT 60)

add_executable(test_graphics_bridge test_graphics_bridge.cpp)
target_link_libraries(test_graphics_bridge PRIVATE ps2r_title_hle ps2r_gs_hle_queue)
target_compile_options(test_graphics_bridge PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsBridge COMMAND test_graphics_bridge)
set_tests_properties(GraphicsBridge PROPERTIES TIMEOUT 60)

add_executable(test_graphics_service test_graphics_service.cpp)
target_link_libraries(test_graphics_service PRIVATE ps2r_title_hle)
target_compile_options(test_graphics_service PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsService COMMAND test_graphics_service)
set_tests_properties(GraphicsService PROPERTIES TIMEOUT 60)

add_executable(test_material_state test_material_state.cpp)
target_link_libraries(test_material_state PRIVATE ps2r_title_hle)
target_compile_options(test_material_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME MaterialState COMMAND test_material_state)
set_tests_properties(MaterialState PROPERTIES TIMEOUT 60)

add_executable(test_strip_draw test_strip_draw.cpp)
target_link_libraries(test_strip_draw PRIVATE ps2r_title_hle)
target_compile_options(test_strip_draw PRIVATE -Wall -Wextra -Werror)
add_test(NAME StripDraw COMMAND test_strip_draw)
set_tests_properties(StripDraw PROPERTIES TIMEOUT 60)

add_executable(test_strip_intent test_strip_intent.cpp)
target_link_libraries(test_strip_intent PRIVATE ps2r_title_hle)
target_compile_options(test_strip_intent PRIVATE -Wall -Wextra -Werror)
add_test(NAME StripIntent COMMAND test_strip_intent)
set_tests_properties(StripIntent PROPERTIES TIMEOUT 60)

# Isolated NEW-generated-body self-oracle. The only model dependencies here are
# test-only; the enabled producer never parses its own output through a model.
if(TARGET ps2r_title_recomp)   # generated-body oracle (title test)
set(strip_oracle_dir "${CMAKE_CURRENT_BINARY_DIR}/strip_oracle")
set(strip_oracle_shards)
foreach(shard 000 009 010 011 012 014)
    list(APPEND strip_oracle_shards
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_${shard}.cpp")
endforeach()
add_custom_command(
    OUTPUT "${strip_oracle_dir}/strip_generated.cpp" "${strip_oracle_dir}/strip_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_strip_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${strip_oracle_dir}"
    DEPENDS ps2r_title_recomp_gen generate_strip_oracle.py ${strip_oracle_shards}
    VERBATIM)
add_executable(test_strip_self_oracle test_strip_self_oracle.cpp strip_oracle_export.cpp oracle_artifacts.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_strip.cpp"
    "${strip_oracle_dir}/strip_generated.cpp")
# GCC 13 at -O3 reports -W(maybe-)uninitialized through the VR_reg union operators
# (engine/ee/recomp_abi/host_app/cpu_state.h) inside these generated bodies: a body's partial
# lane writes before a vector op, the same class the shards silence with -w (games/<t>/recomp);
# scoped to this generated TU, GNU only (B1 review F1; the GCC lane).
set_source_files_properties("${strip_oracle_dir}/strip_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-ffp-contract=off;-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces;$<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized;-Wno-uninitialized>")
target_link_libraries(test_strip_self_oracle PRIVATE ps2r_title_hle ps2r_gif_hle ps2r_fake_backend)
target_compile_options(test_strip_self_oracle PRIVATE -Wall -Wextra -Werror)
# The same export root/manifest pair as the Movie oracles (one artifact
# run per build); without them the registered command is unchanged.
movie_oracle_export_args(strip_export StripSelfOracle)
add_test(NAME StripSelfOracle COMMAND test_strip_self_oracle ${strip_export})
set_tests_properties(StripSelfOracle PROPERTIES TIMEOUT 180)
endif()

if(TARGET ps2r_title_recomp)   # generated-body oracle (title test)
set(sky_oracle_dir "${CMAKE_CURRENT_BINARY_DIR}/sky_oracle")
add_custom_command(
    OUTPUT "${sky_oracle_dir}/sky_generated.cpp" "${sky_oracle_dir}/sky_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_sky_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${sky_oracle_dir}"
    DEPENDS ps2r_title_recomp_gen generate_sky_oracle.py
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_012.cpp"
    VERBATIM)
add_executable(test_sky_self_oracle test_sky_self_oracle.cpp "${sky_oracle_dir}/sky_generated.cpp")
set_source_files_properties("${sky_oracle_dir}/sky_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label")
target_link_libraries(test_sky_self_oracle PRIVATE ps2r_title_hle ps2r_gif_hle ps2r_fake_backend)
target_compile_options(test_sky_self_oracle PRIVATE -Wall -Wextra -Werror)
add_test(NAME SkySelfOracle COMMAND test_sky_self_oracle)
# Debug measured ~56 seconds during concurrent work. Keep the complete corpus
# under suite contention rather than reducing comparisons to fit a 60s cap.
set_tests_properties(SkySelfOracle PROPERTIES TIMEOUT 180)
endif()

add_executable(test_sky_geometry_oracle test_sky_geometry_oracle.cpp)
target_link_libraries(test_sky_geometry_oracle PRIVATE ps2r_title_hle ps2r_vif ps2r_gif_hle)
target_compile_options(test_sky_geometry_oracle PRIVATE -Wall -Wextra -Werror -ffp-contract=off)

add_executable(test_mesh_geometry_oracle test_mesh_geometry_oracle.cpp)
target_link_libraries(test_mesh_geometry_oracle PRIVATE ps2r_title_hle ps2r_vif ps2r_gif_hle)
target_compile_options(test_mesh_geometry_oracle PRIVATE -Wall -Wextra -Werror -ffp-contract=off)

# Prepare + BuildPrepared (the native platform arm's per-frame path) against Build: bit-identical vertices, the
# same draws, the same rejections, over randomized meshes and views (no assets)
add_executable(test_mesh_geometry_prepared test_mesh_geometry_prepared.cpp)
target_link_libraries(test_mesh_geometry_prepared PRIVATE ps2r_title_hle)
target_compile_options(test_mesh_geometry_prepared PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
add_test(NAME MeshGeometryPrepared COMMAND test_mesh_geometry_prepared)
set_tests_properties(MeshGeometryPrepared PROPERTIES TIMEOUT 120 LABELS "graphics")

if(TARGET ps2r_title_recomp)   # generated-body oracle (title test)
set(rigid_content_dir "${CMAKE_CURRENT_BINARY_DIR}/rigid_content_oracle")
set(rigid_content_shards ${PS2R_RECOMP_SHARD_FILES})   # the GENERATED shards (build dir)
add_custom_command(
    OUTPUT "${rigid_content_dir}/rigid_content_generated.cpp" "${rigid_content_dir}/rigid_content_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_rigid_content_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${rigid_content_dir}"
    DEPENDS ps2r_title_recomp_gen generate_rigid_content_oracle.py ${rigid_content_shards}
    VERBATIM)
set_source_files_properties("${rigid_content_dir}/rigid_content_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-w;-ffp-contract=off")
add_executable(test_mesh_pass_oracle test_mesh_pass_oracle.cpp "${rigid_content_dir}/rigid_content_generated.cpp")
target_link_libraries(test_mesh_pass_oracle PRIVATE ps2r_title_hle ps2r_dmac ps2r_gif_hle ps2r_fake_backend)
target_compile_options(test_mesh_pass_oracle PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
endif()
add_executable(test_mesh_pass_guards test_mesh_pass_guards.cpp)
target_link_libraries(test_mesh_pass_guards PRIVATE ps2r_title_hle)
target_compile_options(test_mesh_pass_guards PRIVATE -Wall -Wextra -Werror)
add_test(NAME MeshPassGuards COMMAND test_mesh_pass_guards)
set_tests_properties(MeshPassGuards PROPERTIES TIMEOUT 60 LABELS "graphics")

add_executable(test_sky_texture_oracle test_sky_texture_oracle.cpp)
target_link_libraries(test_sky_texture_oracle PRIVATE ps2r_title_hle ps2r_gif_hle ps2r_gs_hle_texsrc)
target_compile_options(test_sky_texture_oracle PRIVATE -Wall -Wextra -Werror)

if(TARGET ps2r_title_recomp)   # generated-body oracles (title tests)
set(sky_content_dir "${CMAKE_CURRENT_BINARY_DIR}/sky_content_oracle")
add_custom_command(
    OUTPUT "${sky_content_dir}/sky_content_generated.cpp" "${sky_content_dir}/sky_content_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_sky_content_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${sky_content_dir}"
    DEPENDS ps2r_title_recomp_gen generate_sky_content_oracle.py
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_008.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_009.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_010.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_012.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_013.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_014.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_017.cpp"
    VERBATIM)
set_source_files_properties("${sky_content_dir}/sky_content_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-w;-ffp-contract=off")
add_executable(test_sky_setup_oracle test_sky_setup_oracle.cpp "${sky_content_dir}/sky_content_generated.cpp")
target_link_libraries(test_sky_setup_oracle PRIVATE ps2r_title_hle ps2r_gif_hle)
target_compile_options(test_sky_setup_oracle PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
add_test(NAME SkySetupSelfOracle COMMAND test_sky_setup_oracle)
set_tests_properties(SkySetupSelfOracle PROPERTIES TIMEOUT 120 LABELS "graphics;self-oracle")
add_executable(test_sky_pass_oracle test_sky_pass_oracle.cpp oracle_artifacts.cpp
    "${sky_content_dir}/sky_content_generated.cpp")
target_link_libraries(test_sky_pass_oracle PRIVATE ps2r_title_hle ps2r_dmac ps2r_gif_hle ps2r_fake_backend)
target_compile_options(test_sky_pass_oracle PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
endif()
add_executable(test_sky_pass test_sky_pass.cpp)
target_link_libraries(test_sky_pass PRIVATE ps2r_title_hle)
target_compile_options(test_sky_pass PRIVATE -Wall -Wextra -Werror)
add_test(NAME SkyPass COMMAND test_sky_pass)
add_executable(test_sky_programs test_sky_programs.cpp)
target_link_libraries(test_sky_programs PRIVATE ps2r_title_hle)
target_compile_options(test_sky_programs PRIVATE -Wall -Wextra -Werror)
add_test(NAME SkyPrograms COMMAND test_sky_programs)
set_tests_properties(SkyPrograms PROPERTIES TIMEOUT 60 LABELS "graphics")
add_executable(test_sky_state test_sky_state.cpp)
target_link_libraries(test_sky_state PRIVATE ps2r_title_hle)
target_compile_options(test_sky_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME SkyState COMMAND test_sky_state)
set_tests_properties(SkyPass PROPERTIES TIMEOUT 60 LABELS "graphics")

# Optional user-owned, STATIC retail inputs. No savestate/capture is required.
# audit_sky_assets.py produces this directory from CRASH.BH/BD outside the repo.
set(PS2R_SKY_ASSETS "" CACHE PATH "Static authored sky assets extracted with audit_sky_assets.py")
set(PS2R_SKY_ELF "" CACHE FILEPATH "User-owned SLUS_209.09 for the static sky shader self-oracle")
add_test(NAME SkyGeometrySelfOracle COMMAND test_sky_geometry_oracle "${PS2R_SKY_ELF}" "${PS2R_SKY_ASSETS}")
add_test(NAME SkyTextureSelfOracle COMMAND test_sky_texture_oracle "${PS2R_SKY_ASSETS}")
set_tests_properties(SkyGeometrySelfOracle SkyTextureSelfOracle PROPERTIES
    TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "graphics;self-oracle;static-assets")
if(TARGET test_sky_pass_oracle)   # the generated-body half (title test)
    movie_oracle_export_args(sky_pass_export SkyPassSelfOracle)
    add_test(NAME SkyPassSelfOracle COMMAND test_sky_pass_oracle "${PS2R_SKY_ELF}" "${PS2R_SKY_ASSETS}" ${sky_pass_export})
    set_tests_properties(SkyPassSelfOracle PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "graphics;self-oracle;static-assets")
endif()
set(PS2R_MESH_ASSETS "" CACHE PATH "Static type01 assets/parts.tsv from audit_rigid_assets.py")
add_test(NAME MeshGeometrySelfOracle COMMAND test_mesh_geometry_oracle "${PS2R_SKY_ELF}" "${PS2R_MESH_ASSETS}")
set_tests_properties(MeshGeometrySelfOracle PROPERTIES
    TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "graphics;self-oracle;static-assets")
if(TARGET test_mesh_pass_oracle)   # the generated-body half (title test)
    add_test(NAME MeshPassSelfOracle COMMAND test_mesh_pass_oracle "${PS2R_SKY_ELF}" "${PS2R_MESH_ASSETS}")
    set_tests_properties(MeshPassSelfOracle PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "graphics;self-oracle;static-assets")
endif()

add_executable(test_frame_environment test_frame_environment.cpp)
target_link_libraries(test_frame_environment PRIVATE ps2r_title_hle)
target_compile_options(test_frame_environment PRIVATE -Wall -Wextra -Werror)
add_test(NAME FrameEnvironment COMMAND test_frame_environment)
set_tests_properties(FrameEnvironment PROPERTIES TIMEOUT 60)

if(TARGET ps2r_title_recomp)   # generated-body oracles (title tests)
set(frame_oracle_dir "${CMAKE_CURRENT_BINARY_DIR}/frame_oracle")
add_custom_command(
    OUTPUT "${frame_oracle_dir}/frame_generated.cpp" "${frame_oracle_dir}/frame_generated.json"
        "${frame_oracle_dir}/frame_callers.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_frame_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${frame_oracle_dir}"
    DEPENDS ps2r_title_recomp_gen generate_frame_oracle.py
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_010.cpp"
    VERBATIM)
set_source_files_properties("${frame_oracle_dir}/frame_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label")
add_executable(test_frame_self_oracle test_frame_self_oracle.cpp oracle_artifacts.cpp
    "${frame_oracle_dir}/frame_generated.cpp")
target_link_libraries(test_frame_self_oracle PRIVATE ps2r_title_hle ps2r_gif_hle)
target_compile_options(test_frame_self_oracle PRIVATE -Wall -Wextra -Werror)
movie_oracle_export_args(frame_export FrameSelfOracle)
add_test(NAME FrameSelfOracle COMMAND test_frame_self_oracle ${frame_export})
set_tests_properties(FrameSelfOracle PROPERTIES TIMEOUT 120)
set(frame_clear_oracle_dir "${CMAKE_CURRENT_BINARY_DIR}/frame_clear_oracle")
add_custom_command(
    OUTPUT "${frame_clear_oracle_dir}/frame_clear_generated.cpp"
           "${frame_clear_oracle_dir}/frame_clear_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_frame_clear_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${frame_clear_oracle_dir}"
    DEPENDS ps2r_title_recomp_gen generate_frame_clear_oracle.py
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_010.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_028.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_029.cpp"
    VERBATIM)
set_source_files_properties("${frame_clear_oracle_dir}/frame_clear_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label")
add_executable(test_frame_clear_self_oracle test_frame_clear_self_oracle.cpp oracle_artifacts.cpp
    "${frame_clear_oracle_dir}/frame_clear_generated.cpp")
target_link_libraries(test_frame_clear_self_oracle PRIVATE ps2r_title_hle ps2r_gif_hle)
target_compile_options(test_frame_clear_self_oracle PRIVATE -Wall -Wextra -Werror)
movie_oracle_export_args(frame_clear_export FrameClearSelfOracle)
add_test(NAME FrameClearSelfOracle COMMAND test_frame_clear_self_oracle ${frame_clear_export})
set_tests_properties(FrameClearSelfOracle PROPERTIES TIMEOUT 120 LABELS "graphics;self-oracle")
set(display_oracle_dir "${CMAKE_CURRENT_BINARY_DIR}/display_oracle")
add_custom_command(
    OUTPUT "${display_oracle_dir}/display_generated.cpp" "${display_oracle_dir}/display_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/generate_display_oracle.py"
        --root "${CMAKE_SOURCE_DIR}" --out "${display_oracle_dir}"
    DEPENDS ps2r_title_recomp_gen generate_display_oracle.py
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_010.cpp"
    VERBATIM)
set_source_files_properties("${display_oracle_dir}/display_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label")
add_executable(test_display_layout_self_oracle test_display_layout_self_oracle.cpp oracle_artifacts.cpp
    "${display_oracle_dir}/display_generated.cpp")
target_link_libraries(test_display_layout_self_oracle PRIVATE ps2r_title_hle ps2r_gs_hle_queue
    ps2r_fake_backend ps2r_gs_state)
target_compile_options(test_display_layout_self_oracle PRIVATE -Wall -Wextra -Werror)
movie_oracle_export_args(display_layout_export DisplayLayoutSelfOracle)
add_test(NAME DisplayLayoutSelfOracle COMMAND test_display_layout_self_oracle ${display_layout_export})
set_tests_properties(DisplayLayoutSelfOracle PROPERTIES TIMEOUT 120 LABELS "graphics;self-oracle")
endif()   # TARGET ps2r_title_recomp (frame / frame-clear / display oracles)
