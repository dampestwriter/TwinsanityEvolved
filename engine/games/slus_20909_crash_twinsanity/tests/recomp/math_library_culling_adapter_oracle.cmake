if(NOT PS2R_MATH_ORACLE_ELF)
    return()
endif()
get_filename_component(_math_culling_adapter_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_math_culling_adapter_output "${CMAKE_CURRENT_BINARY_DIR}/math_library_culling_adapter_oracle_generated.cpp")
add_custom_command(
    OUTPUT "${_math_culling_adapter_output}"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_culling_adapter_oracle.py"
        --root "${_math_culling_adapter_root}" --output "${_math_culling_adapter_output}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_culling_adapter_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_planes_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_oracle.py"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_001.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_009.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_015.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_016.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_017.cpp"
    VERBATIM)
add_library(math_library_culling_adapter_oracle_model OBJECT "${_math_culling_adapter_output}")
target_include_directories(math_library_culling_adapter_oracle_model PRIVATE "${_math_culling_adapter_root}")
target_compile_options(math_library_culling_adapter_oracle_model PRIVATE
    -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
set_property(TARGET math_library_culling_adapter_oracle_model PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(test_math_library_culling_adapter_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_culling_adapter_oracle.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp"
    "${_math_culling_image}"
    $<TARGET_OBJECTS:math_library_culling_adapter_oracle_model>)
target_include_directories(test_math_library_culling_adapter_oracle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(test_math_library_culling_adapter_oracle PRIVATE ps2r_title_hle ps2r_vu)
target_compile_options(test_math_library_culling_adapter_oracle PRIVATE -Wall -Wextra -Werror)
add_test(NAME MathLibraryCullingAdapterOracle COMMAND test_math_library_culling_adapter_oracle)
set_tests_properties(MathLibraryCullingAdapterOracle PROPERTIES TIMEOUT 120)
