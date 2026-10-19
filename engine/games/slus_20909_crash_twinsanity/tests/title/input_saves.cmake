# Include from tests/CMakeLists.txt after the engine/title targets exist.
# Current NEW pad/store implementations are linked solely as SELF oracles.
include_guard(GLOBAL)

add_executable(test_input_service "${CMAKE_CURRENT_LIST_DIR}/test_input_service.cpp")
target_link_libraries(test_input_service PRIVATE ps2r_title_hle ps2r_pad)
target_compile_options(test_input_service PRIVATE -Wall -Wextra -Werror)
add_test(NAME InputService COMMAND test_input_service)

add_executable(test_save_storage "${CMAKE_CURRENT_LIST_DIR}/test_save_storage.cpp")
target_link_libraries(test_save_storage PRIVATE ps2r_title_hle ps2r_mc)
target_compile_options(test_save_storage PRIVATE -Wall -Wextra -Werror)
add_test(NAME SaveStorage COMMAND test_save_storage)

add_executable(test_input_edges "${CMAKE_CURRENT_LIST_DIR}/test_input_edges.cpp")
target_link_libraries(test_input_edges PRIVATE ps2r_title_hle ps2r_platform_sdl)
target_compile_options(test_input_edges PRIVATE -Wall -Wextra -Werror)
add_test(NAME InputEdges COMMAND test_input_edges)

set_tests_properties(InputService SaveStorage InputEdges PROPERTIES TIMEOUT 120)

add_executable(test_input_save_state_codecs "${CMAKE_CURRENT_LIST_DIR}/test_input_save_state_codecs.cpp")
target_link_libraries(test_input_save_state_codecs PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_input_save_state_codecs PRIVATE -Wall -Wextra -Werror)
add_test(NAME InputSaveStateCodecs COMMAND test_input_save_state_codecs)
set_tests_properties(InputSaveStateCodecs PROPERTIES TIMEOUT 120)

add_executable(test_input_legacy_import "${CMAKE_CURRENT_LIST_DIR}/test_input_legacy_import.cpp")
target_link_libraries(test_input_legacy_import PRIVATE ps2r_title_hle ps2r_pad ps2r_core)
target_compile_options(test_input_legacy_import PRIVATE -Wall -Wextra -Werror)
add_test(NAME InputLegacyImport COMMAND test_input_legacy_import)
set_tests_properties(InputLegacyImport PROPERTIES TIMEOUT 120)

add_executable(test_input_save_checkpoint "${CMAKE_CURRENT_LIST_DIR}/test_input_save_checkpoint.cpp")
target_link_libraries(test_input_save_checkpoint PRIVATE ps2r_title_hle ps2r_pad ps2r_core)
target_compile_options(test_input_save_checkpoint PRIVATE -Wall -Wextra -Werror)
add_test(NAME InputSaveCheckpoint COMMAND test_input_save_checkpoint)
set_tests_properties(InputSaveCheckpoint PROPERTIES TIMEOUT 120)

add_executable(test_save_legacy_import "${CMAKE_CURRENT_LIST_DIR}/test_save_legacy_import.cpp")
target_link_libraries(test_save_legacy_import PRIVATE ps2r_title_hle ps2r_mc ps2r_core ps2r_ee)
target_compile_options(test_save_legacy_import PRIVATE -Wall -Wextra -Werror)
add_test(NAME SaveLegacyImport COMMAND test_save_legacy_import)
set_tests_properties(SaveLegacyImport PROPERTIES TIMEOUT 120)
