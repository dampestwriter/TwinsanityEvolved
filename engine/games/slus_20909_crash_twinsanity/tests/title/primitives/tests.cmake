include_guard(GLOBAL)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(sprite_repo "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
if(TARGET ps2r_title_recomp)   # title test: SpriteSelfOracle reads the GENERATED shards; skipped without a title ELF
set(sprite_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/sprite_oracle")
set(sprite_oracle_shards "")
foreach(shard 001 009 010 011 013 014)
    list(APPEND sprite_oracle_shards
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_${shard}.cpp")
endforeach()
add_custom_command(
    OUTPUT "${sprite_generated_dir}/sprite_generated.cpp" "${sprite_generated_dir}/sprite_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_sprite_oracle.py"
        --root "${sprite_repo}" --out "${sprite_generated_dir}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_sprite_oracle.py" ${sprite_oracle_shards}
    VERBATIM)
add_executable(test_sprite_self_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_sprite_self_oracle.cpp"
    "${sprite_repo}/games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.cpp"
    "${sprite_generated_dir}/sprite_generated.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../textures/texture_binding_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../textures/texture_test_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../textures/texture_test_data.cpp")
# GCC 13 at -O3 reports -W(maybe-)uninitialized through the VR_reg union operators
# (engine/ee/recomp_abi/host_app/cpu_state.h) inside these generated bodies: a body's partial
# lane writes before a vector op, the same class the shards silence with -w (games/<t>/recomp);
# scoped to this generated TU, GNU only (B1 review F1; the GCC lane).
set_source_files_properties("${sprite_generated_dir}/sprite_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-ffp-contract=off;-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces;$<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized;-Wno-uninitialized>")
target_compile_options(test_sprite_self_oracle PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_sprite_self_oracle PRIVATE ps2r_private_fixtures)  # the data map: a private fixture (SKIP 77)
target_link_libraries(test_sprite_self_oracle PRIVATE ps2r_title_hle ps2r_gs_state ps2r_fake_backend)
set(PS2R_SPRITE_ASSET_LIST "" CACHE FILEPATH
    "Original UI texture records from audit_texture_assets.py ui_assets.txt")
# Artifact export arguments (both or neither), appended after the optional asset
# list exactly as the Movie/Strip/Frame/Sky oracles register them.
movie_oracle_export_args(sprite_export SpriteSelfOracle)
add_test(NAME SpriteSelfOracle COMMAND test_sprite_self_oracle ${PS2R_SPRITE_ASSET_LIST} ${sprite_export})
set_tests_properties(SpriteSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
if(PS2R_SPRITE_ASSET_LIST)
    set_tests_properties(SpriteSelfOracle PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
    add_test(NAME SpriteSelfOracleComponents COMMAND test_sprite_self_oracle)
    set_tests_properties(SpriteSelfOracleComponents PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
endif()
add_executable(test_sprite_service
    "${CMAKE_CURRENT_LIST_DIR}/test_sprite_service.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../textures/texture_binding_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../textures/texture_test_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../textures/texture_test_data.cpp")
target_compile_options(test_sprite_service PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_sprite_service PRIVATE ps2r_private_fixtures)  # the data map: a private fixture (SKIP 77)
target_link_libraries(test_sprite_service PRIVATE ps2r_title_hle ps2r_fake_backend)
add_test(NAME SpriteService COMMAND test_sprite_service)
set_tests_properties(SpriteService PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
if(TARGET ps2r_title_recomp)   # title test(s): SpriteBoundaries audit -- read the GENERATED shards; skipped without a title ELF
add_test(NAME SpriteBoundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/audit_sprite_boundaries.py")
set_tests_properties(SpriteBoundaries PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "${PS2R_RECOMP_TEST_ENV}")  # reads the private data map
endif()
