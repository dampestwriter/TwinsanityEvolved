# The caller oracle extracts bodies from a GENERATED shard (build dir): a title test, present only
# when the title is being generated (TitleInputs).
if(PS2R_PRODUCT_TESTS AND TARGET ps2r_title_recomp)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(input_caller_generated "${CMAKE_CURRENT_BINARY_DIR}/input_callers/input_callers.cpp")
    add_custom_command(OUTPUT "${input_caller_generated}"
        COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/recomp/generate_input_callers.py"
            --root "${PS2R_PRODUCT_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/input_callers"
        DEPENDS ps2r_title_recomp_gen "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/recomp/generate_input_callers.py"
            "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_029.cpp"
        VERBATIM)
    add_executable(test_product_input_callers "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/recomp/test_input_callers.cpp"
        "${input_caller_generated}")
    target_link_libraries(test_product_input_callers PRIVATE ps2r_product_save_services)
    target_compile_options(test_product_input_callers PRIVATE -Wall -Wextra -Werror)
    set_source_files_properties("${input_caller_generated}" PROPERTIES
        COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-unused-parameter;-Wno-unused-label")
    add_test(NAME InputCallers COMMAND test_product_input_callers)
    set_tests_properties(InputCallers PROPERTIES TIMEOUT 30)
endif()
