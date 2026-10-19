include("${CMAKE_CURRENT_LIST_DIR}/math_library_oracle_artifacts.cmake")
get_filename_component(_clipping_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_clipping_output "${CMAKE_CURRENT_BINARY_DIR}/math_library_clipping_oracle_generated.cpp")
add_custom_command(
    OUTPUT "${_clipping_output}"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_clipping_oracle.py"
        --root "${_clipping_root}" --output "${_clipping_output}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_clipping_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/math_library_clipping_oracle.h"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_017.cpp"
        "${_clipping_root}/engine/ee/cpu/cpu_state.h"
        "${_clipping_root}/engine/ee/recomp_abi/host_app/cpu_state.h"
        "${_clipping_root}/engine/ee/cpu/ps2_float.h"
        "${_clipping_root}/engine/ee/cpu/ps2_float.inl"
    VERBATIM)
add_library(math_library_clipping_oracle_model OBJECT "${_clipping_output}")
target_include_directories(math_library_clipping_oracle_model PRIVATE "${_clipping_root}")
target_compile_options(math_library_clipping_oracle_model PRIVATE
    -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
set_property(TARGET math_library_clipping_oracle_model PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(test_math_library_clipping_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_clipping_oracle.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp"
    $<TARGET_OBJECTS:math_library_clipping_oracle_model>)
target_compile_options(test_math_library_clipping_oracle PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_math_library_clipping_oracle PRIVATE ps2r_title_hle)
math_library_register_oracle(MathLibraryClippingOracle test_math_library_clipping_oracle)
