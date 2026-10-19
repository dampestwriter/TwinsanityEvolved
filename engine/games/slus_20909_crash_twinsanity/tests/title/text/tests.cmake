include_guard(GLOBAL)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(text_repo "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(text_fixture_sources
    "${CMAKE_CURRENT_LIST_DIR}/text_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../textures/texture_binding_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../textures/texture_test_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../textures/texture_test_data.cpp")
add_executable(test_text_service "${CMAKE_CURRENT_LIST_DIR}/test_text_service.cpp" ${text_fixture_sources})
target_compile_options(test_text_service PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_text_service PRIVATE ps2r_private_fixtures)  # the data map: a private fixture (SKIP 77)
target_link_libraries(test_text_service PRIVATE ps2r_title_hle ps2r_fake_backend)
add_test(NAME TextService COMMAND test_text_service)
set_tests_properties(TextService PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
if(TARGET ps2r_title_recomp)   # title test(s): TextSelfOracle -- read the GENERATED shards; skipped without a title ELF
set(text_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/text_oracle")
set(text_oracle_shards "")
foreach(shard 006 009 010 011 013 014)
    list(APPEND text_oracle_shards
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_${shard}.cpp")
endforeach()
add_custom_command(
    OUTPUT "${text_generated_dir}/text_generated.cpp" "${text_generated_dir}/text_generated.json"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_text_oracle.py"
        --root "${text_repo}" --out "${text_generated_dir}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_text_oracle.py" ${text_oracle_shards}
    VERBATIM)
# Existing NEW model sources, test-only. The title service does not link this.
add_library(ps2r_text_oracle_model STATIC
    "${text_repo}/games/slus_20909_crash_twinsanity/vu1_library/sprite_text.cpp"
    "${text_repo}/games/slus_20909_crash_twinsanity/vu1_library/library_common.cpp"
    "${text_repo}/games/slus_20909_crash_twinsanity/vu1_library/qw_ptr.cpp"
    "${text_repo}/engine/vu/vu_float.cpp"
    "${text_repo}/engine/vu/vu_chop_round_guard.cpp")
target_include_directories(ps2r_text_oracle_model PRIVATE "${text_repo}")
target_compile_options(ps2r_text_oracle_model PRIVATE -Wall -Wextra -Werror -fno-strict-aliasing -fwrapv)
add_executable(test_text_self_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_text_self_oracle.cpp"
    "${text_repo}/games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/text_oracle.cpp"
    "${text_generated_dir}/text_generated.cpp"
    ${text_fixture_sources})
# GCC 13 at -O3 reports -W(maybe-)uninitialized through the VR_reg union operators
# (engine/ee/recomp_abi/host_app/cpu_state.h) inside these generated bodies: a body's partial
# lane writes before a vector op, the same class the shards silence with -w (games/<t>/recomp);
# scoped to this generated TU, GNU only (B1 review F1; the GCC lane).
set_source_files_properties("${text_generated_dir}/text_generated.cpp" PROPERTIES
    COMPILE_OPTIONS "-ffp-contract=off;-Wno-unused-variable;-Wno-unused-parameter;-Wno-unused-label;-Wno-missing-braces;$<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized;-Wno-uninitialized>")
target_compile_options(test_text_self_oracle PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_text_self_oracle PRIVATE ps2r_private_fixtures)  # the data map: a private fixture (SKIP 77)
target_link_libraries(test_text_self_oracle PRIVATE ps2r_title_hle ps2r_gs_state ps2r_text_oracle_model ps2r_fake_backend)
set(PS2R_FONT_GLYPH_LIST "" CACHE FILEPATH "Original font glyph arrays enumerated by audit_font_assets.py")
# Artifact export arguments (both or neither), appended after the optional glyph list.
movie_oracle_export_args(text_export TextSelfOracle)
add_test(NAME TextSelfOracle COMMAND test_text_self_oracle ${PS2R_FONT_GLYPH_LIST} ${text_export})
set_tests_properties(TextSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
if(TARGET ps2r_title_recomp)   # title test(s): TextBoundaries audit -- read the GENERATED shards; skipped without a title ELF
add_test(NAME TextBoundaries COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/audit_text_boundaries.py")
set_tests_properties(TextBoundaries PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 ENVIRONMENT "${PS2R_RECOMP_TEST_ENV}")  # reads the private data map
endif()
