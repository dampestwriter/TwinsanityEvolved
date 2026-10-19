# The caller oracle extracts bodies from a GENERATED shard (build dir): a title test, present only
# when the title is being generated (TitleInputs).
if(PS2R_PRODUCT_TESTS AND TARGET ps2r_title_recomp)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(save_caller_generated "${CMAKE_CURRENT_BINARY_DIR}/save_callers/save_callers.cpp")
    add_custom_command(OUTPUT "${save_caller_generated}"
        COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/recomp/generate_save_callers.py"
            --root "${PS2R_PRODUCT_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/save_callers"
        DEPENDS ps2r_title_recomp_gen "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/recomp/generate_save_callers.py"
            "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_028.cpp"
        VERBATIM)
    add_executable(test_product_save_callers "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/recomp/test_save_callers.cpp"
        "${save_caller_generated}")
    target_link_libraries(test_product_save_callers PRIVATE ps2r_product_save_services)
    target_compile_options(test_product_save_callers PRIVATE -Wall -Wextra -Werror)
    set_source_files_properties("${save_caller_generated}" PROPERTIES
        COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-unused-parameter;-Wno-unused-label")
    add_test(NAME SaveCallers COMMAND test_product_save_callers)
    set_tests_properties(SaveCallers PROPERTIES TIMEOUT 30)
endif()
