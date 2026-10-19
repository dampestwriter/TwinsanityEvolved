# Include from tests/CMakeLists.txt after the engine/title targets exist.
include_guard(GLOBAL)
add_executable(test_menu_extension "${CMAKE_CURRENT_LIST_DIR}/test_menu_extension.cpp" "${CMAKE_CURRENT_LIST_DIR}/fake_title.cpp")
target_link_libraries(test_menu_extension PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_menu_extension PRIVATE -Wall -Wextra -Werror)
add_test(NAME MenuExtension COMMAND test_menu_extension)
set_tests_properties(MenuExtension PROPERTIES TIMEOUT 60)
