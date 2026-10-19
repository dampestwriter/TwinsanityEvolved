# RetailFixes: the owned bodies over the title's constructors on a fake title.
include_guard(GLOBAL)
add_executable(test_retail_fixes "${CMAKE_CURRENT_LIST_DIR}/test_retail_fixes.cpp")
target_link_libraries(test_retail_fixes PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_retail_fixes PRIVATE -Wall -Wextra -Werror)
add_test(NAME RetailFixes COMMAND test_retail_fixes)
set_tests_properties(RetailFixes PROPERTIES TIMEOUT 60)
