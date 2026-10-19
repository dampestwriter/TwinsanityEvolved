# CutsceneFastForward: the decision on a fake title RAM.
include_guard(GLOBAL)
add_executable(test_cutscene_fast_forward "${CMAKE_CURRENT_LIST_DIR}/test_cutscene_fast_forward.cpp")
target_link_libraries(test_cutscene_fast_forward PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_cutscene_fast_forward PRIVATE -Wall -Wextra -Werror)
add_test(NAME CutsceneFastForward COMMAND test_cutscene_fast_forward)
set_tests_properties(CutsceneFastForward PROPERTIES TIMEOUT 60)
