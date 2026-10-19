get_filename_component(_math_planes_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_math_planes_output "${CMAKE_CURRENT_BINARY_DIR}/math_library_planes_oracle_generated.cpp")
add_custom_command(
    OUTPUT "${_math_planes_output}"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_planes_oracle.py"
        --root "${_math_planes_root}" --output "${_math_planes_output}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_planes_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_oracle.py"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_009.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_015.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_017.cpp"
        "${_math_planes_root}/engine/ee/cpu/ps2_float.h"
        "${_math_planes_root}/engine/ee/cpu/ps2_float.inl"
        "${_math_planes_root}/engine/ee/recomp_abi/host_app/cpu_state.h"
    VERBATIM)
add_library(math_library_planes_oracle_model OBJECT "${_math_planes_output}")
target_include_directories(math_library_planes_oracle_model PRIVATE "${_math_planes_root}")
target_compile_options(math_library_planes_oracle_model PRIVATE
    -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
set_property(TARGET math_library_planes_oracle_model PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(test_math_library_planes_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_planes_oracle.cpp"
    $<TARGET_OBJECTS:math_library_planes_oracle_model>)
target_link_libraries(test_math_library_planes_oracle PRIVATE ps2r_title_hle)
target_compile_options(test_math_library_planes_oracle PRIVATE -Wall -Wextra -Werror)
add_test(NAME MathLibraryPlanesOracle COMMAND test_math_library_planes_oracle)
set_tests_properties(MathLibraryPlanesOracle PROPERTIES TIMEOUT 120)
