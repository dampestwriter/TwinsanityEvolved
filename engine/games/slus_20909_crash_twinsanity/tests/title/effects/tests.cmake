include_guard(GLOBAL)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(effects_repo "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
if(TARGET ps2r_title_recomp)   # title test(s): ShadowDrawSelfOracle / ShadowSourceSelfOracle -- read the GENERATED shards; skipped without a title ELF
set(shadow_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/shadow_oracle")
add_custom_command(
    OUTPUT "${shadow_generated_dir}/shadow_generated.cpp" "${shadow_generated_dir}/shadow_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_shadow_oracle.py"
        --root "${effects_repo}" --out "${shadow_generated_dir}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_shadow_oracle.py"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_009.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_010.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_013.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_025.cpp"
    VERBATIM)
add_executable(test_shadow_draw
    "${CMAKE_CURRENT_LIST_DIR}/test_shadow_draw.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_oracle.cpp"
    "${shadow_generated_dir}/shadow_generated.cpp")
# GNU: -W(maybe-)uninitialized through the VR_reg operators in the generated body (tests/recomp/CMakeLists.txt).
set_source_files_properties("${shadow_generated_dir}/shadow_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces;$<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized;-Wno-uninitialized>")
target_compile_options(test_shadow_draw PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_shadow_draw PRIVATE ps2r_title_hle ps2r_gs_state)
add_test(NAME ShadowDrawSelfOracle COMMAND test_shadow_draw)
set_tests_properties(ShadowDrawSelfOracle PROPERTIES TIMEOUT 120)
add_executable(test_shadow_source
    "${CMAKE_CURRENT_LIST_DIR}/test_shadow_source.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_oracle.cpp"
    "${shadow_generated_dir}/shadow_generated.cpp")
target_compile_options(test_shadow_source PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_shadow_source PRIVATE ps2r_title_hle ps2r_gs_state)
add_test(NAME ShadowSourceSelfOracle COMMAND test_shadow_source)
set_tests_properties(ShadowSourceSelfOracle PROPERTIES TIMEOUT 120)
endif()
add_executable(test_shadow_ownership "${CMAKE_CURRENT_LIST_DIR}/test_shadow_ownership.cpp")
target_compile_options(test_shadow_ownership PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_shadow_ownership PRIVATE ps2r_title_hle)
add_test(NAME ShadowOwnership COMMAND test_shadow_ownership)
set_tests_properties(ShadowOwnership PROPERTIES TIMEOUT 120)
add_executable(test_shadow_pass "${CMAKE_CURRENT_LIST_DIR}/test_shadow_pass.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_pass_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_fixture.cpp")
target_compile_options(test_shadow_pass PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_shadow_pass PRIVATE ps2r_title_hle ps2r_fake_backend)
add_test(NAME ShadowPass COMMAND test_shadow_pass)
set_tests_properties(ShadowPass PROPERTIES TIMEOUT 120)
add_executable(test_shadow_state "${CMAKE_CURRENT_LIST_DIR}/test_shadow_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_pass_fixture.cpp")
target_compile_options(test_shadow_state PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_shadow_state PRIVATE ps2r_title_hle ps2r_core)
add_test(NAME ShadowState COMMAND test_shadow_state)
set_tests_properties(ShadowState PROPERTIES TIMEOUT 120)
add_executable(test_shadow_application "${CMAKE_CURRENT_LIST_DIR}/test_shadow_application.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_pass_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_fixture.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/shadow_application.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/shadow_census.cpp")
target_compile_options(test_shadow_application PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_shadow_application PRIVATE ps2r_title_hle ps2r_fake_backend ps2r_ee ps2r_core)
add_test(NAME ShadowApplication COMMAND test_shadow_application)
set_tests_properties(ShadowApplication PROPERTIES TIMEOUT 120)
if(TARGET ps2r_title_recomp)   # title test(s): ShadowBoundaries / ClothBoundaries / DistortionBoundaries audits -- read the GENERATED shards; skipped without a title ELF
add_test(NAME ShadowBoundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/audit_shadow_boundaries.py")
set_tests_properties(ShadowBoundaries PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "${PS2R_RECOMP_TEST_ENV}")  # reads the private data map
add_test(NAME ClothBoundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/audit_cloth_boundaries.py")
set_tests_properties(ClothBoundaries PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "${PS2R_RECOMP_TEST_ENV}")  # reads the private data map
add_test(NAME DistortionBoundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/audit_distortion_boundaries.py")
set_tests_properties(DistortionBoundaries PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "${PS2R_RECOMP_TEST_ENV}")  # reads the private data map
endif()
add_executable(test_cloth_certificate "${CMAKE_CURRENT_LIST_DIR}/test_cloth_certificate.cpp")
target_compile_options(test_cloth_certificate PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_cloth_certificate PRIVATE ps2r_title_hle)
add_test(NAME ClothCertificate COMMAND test_cloth_certificate)
set_tests_properties(ClothCertificate PROPERTIES TIMEOUT 120)
if(TARGET ps2r_title_recomp)   # title test(s): ClothMaterialSelfOracle / ClothMaterialAssetsSelfOracle -- read the GENERATED shards; skipped without a title ELF
set(cloth_material_generated "${CMAKE_CURRENT_BINARY_DIR}/cloth_material_oracle")
add_custom_command(
    OUTPUT "${cloth_material_generated}/cloth_material_generated.cpp"
           "${cloth_material_generated}/cloth_material_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_cloth_material_oracle.py"
        --root "${effects_repo}" --out "${cloth_material_generated}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_cloth_material_oracle.py"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_009.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_010.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_013.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_014.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_030.cpp"
    VERBATIM)
# GNU: -W(maybe-)uninitialized through the VR_reg operators in the generated body (tests/recomp/CMakeLists.txt).
set_source_files_properties("${cloth_material_generated}/cloth_material_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces;$<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized;-Wno-uninitialized>")
add_executable(test_cloth_material
    "${CMAKE_CURRENT_LIST_DIR}/test_cloth_material.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/cloth_material_oracle.cpp"
    "${cloth_material_generated}/cloth_material_generated.cpp")
target_compile_options(test_cloth_material PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_cloth_material PRIVATE ps2r_title_hle ps2r_gs_state)
set(PS2R_CLOTH_ELF "" CACHE FILEPATH "Original NTSC ELF for cloth source geometry oracle")
add_test(NAME ClothMaterialSelfOracle COMMAND test_cloth_material ${PS2R_CLOTH_ELF})
set_tests_properties(ClothMaterialSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
set(PS2R_CLOTH_MATERIAL_LIST "" CACHE FILEPATH
    "Complete original cloth material manifest from audit_cloth_assets.py")
# Registered unconditionally like its siblings: test_cloth_material skips (77) when the
# ELF or the manifest is unset instead of opening "--assets" as the ELF.
add_test(NAME ClothMaterialAssetsSelfOracle COMMAND test_cloth_material
    ${PS2R_CLOTH_ELF} --assets ${PS2R_CLOTH_MATERIAL_LIST})
set_tests_properties(ClothMaterialAssetsSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cloth_mesh.cmake")
if(TARGET ps2r_title_recomp)   # generated-body oracles (title tests)
    include("${CMAKE_CURRENT_LIST_DIR}/distortion_material.cmake")
endif()
if(TARGET ps2r_title_vu1_library)
    add_executable(test_shadow_geometry
        "${CMAKE_CURRENT_LIST_DIR}/test_shadow_geometry.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/shadow_geometry_oracle.cpp")
    target_compile_options(test_shadow_geometry PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(test_shadow_geometry PRIVATE ps2r_title_hle ps2r_gs_state ps2r_title_vu1_library)
    add_test(NAME ShadowGeometrySelfOracle COMMAND test_shadow_geometry)
    set_tests_properties(ShadowGeometrySelfOracle PROPERTIES TIMEOUT 120)
endif()
if(TARGET ps2r_vif AND TARGET ps2r_gif_hle)
    add_executable(test_cloth_geometry
        "${CMAKE_CURRENT_LIST_DIR}/test_cloth_geometry.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/cloth_oracle.cpp")
    target_compile_options(test_cloth_geometry PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(test_cloth_geometry PRIVATE ps2r_title_hle ps2r_gif_hle ps2r_vif)
    set(PS2R_CLOTH_ELF "" CACHE FILEPATH "Original NTSC ELF for cloth source geometry oracle")
    set(PS2R_CLOTH_ASSET_LIST "" CACHE FILEPATH
        "Complete original cloth parts manifest from audit_cloth_assets.py")
    add_test(NAME ClothGeometrySelfOracle COMMAND test_cloth_geometry ${PS2R_CLOTH_ELF})
    set_tests_properties(ClothGeometrySelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_test(NAME ClothAssetsSelfOracle COMMAND test_cloth_geometry
        ${PS2R_CLOTH_ELF} --assets ${PS2R_CLOTH_ASSET_LIST})
    set_tests_properties(ClothAssetsSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(test_shadow_assets
        "${CMAKE_CURRENT_LIST_DIR}/test_shadow_assets.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/shadow_asset_oracle.cpp")
    target_compile_options(test_shadow_assets PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(test_shadow_assets PRIVATE ps2r_title_hle ps2r_gif_hle ps2r_vif)
    set(PS2R_SHADOW_ASSET_LIST "" CACHE FILEPATH
        "Complete original shadow parts manifest from audit_shadow_assets.py")
    set(PS2R_SHADOW_ELF "" CACHE FILEPATH "Original NTSC ELF for shadow source oracle")
    add_test(NAME ShadowAssetsSelfOracle COMMAND test_shadow_assets
        ${PS2R_SHADOW_ASSET_LIST} ${PS2R_SHADOW_ELF})
    set_tests_properties(ShadowAssetsSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
add_executable(test_shadow_renderer
    "${CMAKE_CURRENT_LIST_DIR}/test_shadow_renderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_fixture.cpp")
target_compile_options(test_shadow_renderer PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_shadow_renderer PRIVATE ps2r_title_hle ps2r_fake_backend)
add_test(NAME ShadowRenderer COMMAND test_shadow_renderer)
set_tests_properties(ShadowRenderer PROPERTIES TIMEOUT 120)
add_executable(test_effects_scene
    "${CMAKE_CURRENT_LIST_DIR}/test_effects_scene.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_fixture.cpp")
target_compile_options(test_effects_scene PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_effects_scene PRIVATE ps2r_title_hle ps2r_fake_backend)
add_test(NAME EffectsScene COMMAND test_effects_scene)
set_tests_properties(EffectsScene PROPERTIES TIMEOUT 120)
if(TARGET ps2r_title_recomp)   # title test(s): ScreenDrawSelfOracle -- read the GENERATED shards; skipped without a title ELF
set(screen_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/screen_oracle")
add_custom_command(
    OUTPUT "${screen_generated_dir}/screen_generated.cpp" "${screen_generated_dir}/screen_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_screen_oracle.py"
        --root "${effects_repo}" --out "${screen_generated_dir}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_screen_oracle.py"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_011.cpp"
    VERBATIM)
add_executable(test_screen_draw
    "${CMAKE_CURRENT_LIST_DIR}/test_screen_draw.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/screen_oracle.cpp"
    "${screen_generated_dir}/screen_generated.cpp")
set_source_files_properties("${screen_generated_dir}/screen_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces")
target_compile_options(test_screen_draw PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_screen_draw PRIVATE ps2r_title_hle ps2r_gs_state)
add_test(NAME ScreenDrawSelfOracle COMMAND test_screen_draw)
set_tests_properties(ScreenDrawSelfOracle PROPERTIES TIMEOUT 120)
endif()
if(TARGET ps2r_title_recomp)   # title test(s): ScreenBoundarySelfOracle -- read the GENERATED shards; skipped without a title ELF
set(screen_boundary_dir "${CMAKE_CURRENT_BINARY_DIR}/screen_boundary_oracle")
add_custom_command(
    OUTPUT "${screen_boundary_dir}/screen_boundary_generated.cpp" "${screen_boundary_dir}/screen_boundary_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_screen_boundary_oracle.py"
        --root "${effects_repo}" --out "${screen_boundary_dir}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_screen_boundary_oracle.py"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_012.cpp"
    VERBATIM)
add_executable(test_screen_boundary
    "${CMAKE_CURRENT_LIST_DIR}/test_screen_boundary.cpp"
    "${screen_boundary_dir}/screen_boundary_generated.cpp")
set_source_files_properties("${screen_boundary_dir}/screen_boundary_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces")
target_compile_options(test_screen_boundary PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_screen_boundary PRIVATE ps2r_title_hle ps2r_gs_state)
add_test(NAME ScreenBoundarySelfOracle COMMAND test_screen_boundary)
set_tests_properties(ScreenBoundarySelfOracle PROPERTIES TIMEOUT 120)
endif()
if(TARGET ps2r_title_recomp)   # title test(s): ScreenPalettesSelfOracle -- read the GENERATED shards; skipped without a title ELF
set(screen_palette_dir "${CMAKE_CURRENT_BINARY_DIR}/screen_palette_oracle")
add_custom_command(
    OUTPUT "${screen_palette_dir}/screen_palette_generated.cpp" "${screen_palette_dir}/screen_palette_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_screen_palette_oracle.py"
        --root "${effects_repo}" --out "${screen_palette_dir}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_screen_palette_oracle.py"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_011.cpp"
    VERBATIM)
add_executable(test_screen_palettes
    "${CMAKE_CURRENT_LIST_DIR}/test_screen_palettes.cpp"
    "${screen_palette_dir}/screen_palette_generated.cpp")
set_source_files_properties("${screen_palette_dir}/screen_palette_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces")
target_compile_options(test_screen_palettes PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_screen_palettes PRIVATE ps2r_title_hle ps2r_gs_state)
# The retail ELF (optional) supplies the authored palettes; without it synthetic tables are used.
add_test(NAME ScreenPalettesSelfOracle COMMAND test_screen_palettes ${PS2R_CLOTH_ELF})
set_tests_properties(ScreenPalettesSelfOracle PROPERTIES TIMEOUT 120)
endif()
if(TARGET ps2r_vu)
    add_executable(test_distortion_geometry
        "${CMAKE_CURRENT_LIST_DIR}/test_distortion_geometry.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/distortion_oracle.cpp")
    target_compile_options(test_distortion_geometry PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(test_distortion_geometry PRIVATE ps2r_title_hle ps2r_gs_state ps2r_vu)
    set(PS2R_DISTORTION_PROGRAM "" CACHE FILEPATH
        "Static NTSC type18 image from audit_effect_programs.py")
    add_test(NAME DistortionGeometrySelfOracle COMMAND test_distortion_geometry ${PS2R_DISTORTION_PROGRAM})
    set_tests_properties(DistortionGeometrySelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(test_wave_geometry
        "${CMAKE_CURRENT_LIST_DIR}/test_wave_geometry.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/wave_oracle.cpp")
    target_compile_options(test_wave_geometry PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(test_wave_geometry PRIVATE ps2r_title_hle ps2r_gs_state ps2r_vu)
    set(PS2R_WAVE_PROGRAM "" CACHE FILEPATH
        "Static NTSC type-1C image from audit_effect_programs.py")
    add_test(NAME WaveGeometrySelfOracle COMMAND test_wave_geometry ${PS2R_WAVE_PROGRAM})
    set_tests_properties(WaveGeometrySelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
add_executable(effect_program_inventory
    "${CMAKE_CURRENT_LIST_DIR}/effect_program_inventory.cpp"
    "${effects_repo}/engine/vu/vu_decoder.cpp")
target_include_directories(effect_program_inventory PRIVATE "${effects_repo}")
target_compile_options(effect_program_inventory PRIVATE -Wall -Wextra -Werror)
include("${CMAKE_CURRENT_LIST_DIR}/distortion_rendering.cmake")
if(TARGET ps2r_vu AND TARGET ps2r_title_recomp)   # generated-body oracle (title test)
    include("${CMAKE_CURRENT_LIST_DIR}/distortion_blocks.cmake")
endif()
# The pixel oracles link ps2r_render_gl/ps2r_platform_sdl, which the root project adds
# after it includes these tests. Register them when the including directory finishes so
# the include order can neither drop nor silently skip them. The deferred call's arguments
# are expanded when it runs, so the fragment path is fixed here through EVAL CODE.
cmake_language(EVAL CODE
    "cmake_language(DEFER CALL include \"${CMAKE_CURRENT_LIST_DIR}/effects_pixels.cmake\")")
