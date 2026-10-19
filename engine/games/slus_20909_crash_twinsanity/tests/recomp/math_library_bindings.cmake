add_executable(test_math_library_bindings
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp")
target_link_libraries(test_math_library_bindings PRIVATE ps2r_title_hle)
target_compile_options(test_math_library_bindings PRIVATE -Wall -Wextra -Werror)
add_test(NAME MathLibraryBindings COMMAND test_math_library_bindings)
set_tests_properties(MathLibraryBindings PROPERTIES TIMEOUT 60)
