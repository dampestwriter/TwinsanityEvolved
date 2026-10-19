set(distortion_blocks_generated "${CMAKE_CURRENT_BINARY_DIR}/distortion_blocks_oracle")
add_custom_command(
    OUTPUT "${distortion_blocks_generated}/distortion_blocks_generated.cpp"
           "${distortion_blocks_generated}/distortion_blocks_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_distortion_blocks_oracle.py"
        --root "${effects_repo}" --out "${distortion_blocks_generated}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_distortion_blocks_oracle.py"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_012.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_030.cpp"
    VERBATIM)
set_source_files_properties("${distortion_blocks_generated}/distortion_blocks_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces")
add_executable(test_distortion_blocks
    "${CMAKE_CURRENT_LIST_DIR}/test_distortion_blocks.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_blocks_oracle.cpp"
    "${distortion_blocks_generated}/distortion_blocks_generated.cpp")
target_compile_options(test_distortion_blocks PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_distortion_blocks PRIVATE ps2r_title_hle ps2r_vu)
add_test(NAME DistortionBlocksSelfOracle COMMAND test_distortion_blocks)
set_tests_properties(DistortionBlocksSelfOracle PROPERTIES TIMEOUT 120)
