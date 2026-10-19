# Source-owned checkpoint tests require no ELF or game run.
add_executable(test_math_library_culling_checkpoint
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_culling_checkpoint.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp")
target_link_libraries(test_math_library_culling_checkpoint PRIVATE ps2r_title_hle)
target_compile_options(test_math_library_culling_checkpoint PRIVATE -Wall -Wextra -Werror)
# The legacy anchors root is the command argument (read-only; names absent there are skipped).
add_test(NAME MathLibraryCullingCheckpoint COMMAND test_math_library_culling_checkpoint "${PS2R_OG_ROOT}/save_states")
set_tests_properties(MathLibraryCullingCheckpoint PROPERTIES TIMEOUT 120)
