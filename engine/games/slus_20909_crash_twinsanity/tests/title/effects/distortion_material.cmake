set(distortion_generated "${CMAKE_CURRENT_BINARY_DIR}/distortion_material_oracle")
add_custom_command(
    OUTPUT "${distortion_generated}/distortion_material_generated.cpp"
           "${distortion_generated}/distortion_material_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_distortion_material_oracle.py"
        --root "${effects_repo}" --out "${distortion_generated}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_distortion_material_oracle.py"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_009.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_010.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_014.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_030.cpp"
    VERBATIM)
# GNU: -W(maybe-)uninitialized through the VR_reg operators in the generated body (tests/recomp/CMakeLists.txt).
set_source_files_properties("${distortion_generated}/distortion_material_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces;$<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized;-Wno-uninitialized>")
add_executable(test_distortion_material
    "${CMAKE_CURRENT_LIST_DIR}/test_distortion_material.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_material_oracle.cpp"
    "${distortion_generated}/distortion_material_generated.cpp")
target_compile_options(test_distortion_material PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_distortion_material PRIVATE ps2r_title_hle ps2r_gs_state)
add_test(NAME DistortionMaterialSelfOracle COMMAND test_distortion_material)
set_tests_properties(DistortionMaterialSelfOracle PROPERTIES TIMEOUT 120)
add_executable(test_distortion_settings
    "${CMAKE_CURRENT_LIST_DIR}/test_distortion_settings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_material_oracle.cpp"
    "${distortion_generated}/distortion_material_generated.cpp")
target_compile_options(test_distortion_settings PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_distortion_settings PRIVATE ps2r_title_hle ps2r_gs_state)
add_test(NAME DistortionSettingsSelfOracle COMMAND test_distortion_settings)
set_tests_properties(DistortionSettingsSelfOracle PROPERTIES TIMEOUT 120)
