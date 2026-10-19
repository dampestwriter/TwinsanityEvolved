# The title-bound rows of tests/vu: the VU1 HLE mechanism over this title's program library and
# the VU0 registry census.
include_guard(GLOBAL)
set(_vu "${PS2R_TITLE_TESTS}/vu")
set(_vu_engine "${CMAKE_SOURCE_DIR}/tests/vu")   # the engine's shared helpers (origin/main)

add_executable(test_vu1_hle_mechanism ${_vu}/test_vu1_hle_mechanism.cpp)
target_link_libraries(test_vu1_hle_mechanism PRIVATE ps2r_vu1_replay_lib ps2r_title_vu1_library)
target_compile_options(test_vu1_hle_mechanism PRIVATE -Wall -Wextra -Werror -O2)
add_test(NAME Vu1HleMechanism COMMAND test_vu1_hle_mechanism ${PS2R_OG_ROOT}/vutrace_cases2/vu1_case_crys_283276f2.bin
                                      ${PS2R_OG_ROOT}/vutrace_cases2/vu1_case_w14hub_0001.bin
                                      ${PS2R_OG_ROOT}/vutrace_cases2)
set_tests_properties(Vu1HleMechanism PROPERTIES TIMEOUT 120)
add_executable(test_vu0_registry ${_vu}/test_vu0_registry.cpp)
target_link_libraries(test_vu0_registry PRIVATE ps2r_title_vu1_library)
target_compile_options(test_vu0_registry PRIVATE -Wall -Wextra -Werror -O2)
add_test(NAME Vu0RegistryCensus COMMAND test_vu0_registry)
set_tests_properties(Vu0RegistryCensus PROPERTIES TIMEOUT 120)
