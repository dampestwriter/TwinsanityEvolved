# Parent integration: include(${CMAKE_CURRENT_LIST_DIR}/math_library_oracle.cmake)
# Keep expected arithmetic in a separate TU with the production generated flags.
# This fragment is also usable by an isolated fixture providing ps2r_title_hle.
include("${CMAKE_CURRENT_LIST_DIR}/math_library_oracle_artifacts.cmake")
get_filename_component(PS2R_MATH_ORACLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(PS2R_MATH_ORACLE_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_oracle.py")
set(PS2R_MATH_ORACLE_SOURCE "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_009.cpp")
set(PS2R_MATH_ORACLE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/math_library_oracle_generated.cpp")
if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
add_custom_command(
    OUTPUT "${PS2R_MATH_ORACLE_OUTPUT}"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${PS2R_MATH_ORACLE_GENERATOR}"
        --root "${PS2R_MATH_ORACLE_ROOT}" --output "${PS2R_MATH_ORACLE_OUTPUT}"
    DEPENDS ps2r_title_recomp_gen "${PS2R_MATH_ORACLE_GENERATOR}" "${PS2R_MATH_ORACLE_SOURCE}"
        "${CMAKE_CURRENT_LIST_DIR}/math_library_oracle.h"
    VERBATIM)
add_library(math_library_oracle_model OBJECT "${PS2R_MATH_ORACLE_OUTPUT}")
target_include_directories(math_library_oracle_model PRIVATE "${PS2R_MATH_ORACLE_ROOT}")
target_compile_options(math_library_oracle_model PRIVATE
    -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
set_property(TARGET math_library_oracle_model PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(test_math_library_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_oracle.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp"
    $<TARGET_OBJECTS:math_library_oracle_model>)
target_include_directories(test_math_library_oracle PRIVATE "${PS2R_MATH_ORACLE_ROOT}")
target_link_libraries(test_math_library_oracle PRIVATE ps2r_title_hle)
target_compile_options(test_math_library_oracle PRIVATE -Wall -Wextra -Werror)
math_library_register_oracle(MathLibraryGeneratedOracle test_math_library_oracle)
if(UNIX)
    add_test(NAME MathLibraryOracleArtifactsIo COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/math_library_oracle_artifacts.py"
        --io-test $<TARGET_FILE:test_math_library_oracle>)
    set_tests_properties(MathLibraryOracleArtifactsIo PROPERTIES TIMEOUT 60)
endif()
add_test(NAME MathLibraryOracleGenerator
    COMMAND "${Python3_EXECUTABLE}" "${PS2R_MATH_ORACLE_GENERATOR}" --selftest)
set_tests_properties(MathLibraryOracleGenerator PROPERTIES TIMEOUT 60)
