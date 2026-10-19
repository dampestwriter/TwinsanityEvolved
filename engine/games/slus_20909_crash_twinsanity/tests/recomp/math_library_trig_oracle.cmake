# Parent includes this fragment with MathLibrary and TrigMath in title_hle.
include("${CMAKE_CURRENT_LIST_DIR}/math_library_oracle_artifacts.cmake")
get_filename_component(PS2R_TRIG_ORACLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(PS2R_TRIG_ORACLE_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_trig_oracle.py")
set(PS2R_TRIG_ORACLE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/math_library_trig_oracle_generated.cpp")
if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
add_custom_command(
    OUTPUT "${PS2R_TRIG_ORACLE_OUTPUT}"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${PS2R_TRIG_ORACLE_GENERATOR}"
        --root "${PS2R_TRIG_ORACLE_ROOT}" --output "${PS2R_TRIG_ORACLE_OUTPUT}"
    DEPENDS ps2r_title_recomp_gen "${PS2R_TRIG_ORACLE_GENERATOR}"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/math_library_trig_oracle.h"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_001.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_008.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_009.cpp"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_030.cpp"
        "${PS2R_TRIG_ORACLE_ROOT}/engine/vu/vu0/lib_vu0_sin_cos.cpp"
        "${PS2R_TRIG_ORACLE_ROOT}/engine/vu/vu0/lib_vu0_sin_cos.h"
        "${PS2R_TRIG_ORACLE_ROOT}/engine/vu/vu_float.inl"
        "${PS2R_TRIG_ORACLE_ROOT}/engine/ee/cpu/ps2_float.h"
        "${PS2R_TRIG_ORACLE_ROOT}/engine/ee/cpu/ps2_float.inl"
        "${PS2R_TRIG_ORACLE_ROOT}/engine/vu/vu_chop_round_guard.cpp"
    VERBATIM)
add_library(math_library_trig_oracle_model OBJECT
    "${PS2R_TRIG_ORACLE_OUTPUT}"
    "${PS2R_TRIG_ORACLE_ROOT}/engine/vu/vu0/lib_vu0_sin_cos.cpp"
    "${PS2R_TRIG_ORACLE_ROOT}/engine/vu/vu_chop_round_guard.cpp")
target_include_directories(math_library_trig_oracle_model PRIVATE "${PS2R_TRIG_ORACLE_ROOT}")
target_compile_options(math_library_trig_oracle_model PRIVATE
    -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
set_property(TARGET math_library_trig_oracle_model PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(test_math_library_trig_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_trig_oracle.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp"
    $<TARGET_OBJECTS:math_library_trig_oracle_model>)
target_include_directories(test_math_library_trig_oracle PRIVATE "${PS2R_TRIG_ORACLE_ROOT}")
target_link_libraries(test_math_library_trig_oracle PRIVATE ps2r_title_hle)
target_compile_options(test_math_library_trig_oracle PRIVATE -Wall -Wextra -Werror)
math_library_register_oracle(MathLibraryTrigOracle test_math_library_trig_oracle)
# 992,000 comparisons construct both installed selections with the real
# continuation/counting wrappers. Debug took 231s; the ASan/UBSan binary took
# 665s run directly yet exceeded 900s once under ctest on a loaded builder.
# Keep the full corpus, comparisons and optional byte artifacts in every preset.
set_tests_properties(MathLibraryTrigOracle PROPERTIES TIMEOUT 1800)
