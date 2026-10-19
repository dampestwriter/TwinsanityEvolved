# Parent includes this fragment and compiles math/animation_math.cpp plus
# math/trig_math.cpp in ps2r_title_hle. No runtime toggle is set by this file.
include("${CMAKE_CURRENT_LIST_DIR}/math_library_oracle_artifacts.cmake")
get_filename_component(PS2R_ANIMATION_ORACLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(PS2R_ANIMATION_ORACLE_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_animation_oracle.py")
set(PS2R_ANIMATION_ORACLE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/math_library_animation_oracle_generated.cpp")
if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
add_custom_command(
    OUTPUT "${PS2R_ANIMATION_ORACLE_OUTPUT}"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${PS2R_ANIMATION_ORACLE_GENERATOR}"
        --root "${PS2R_ANIMATION_ORACLE_ROOT}" --output "${PS2R_ANIMATION_ORACLE_OUTPUT}"
    DEPENDS ps2r_title_recomp_gen "${PS2R_ANIMATION_ORACLE_GENERATOR}"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/math_library_animation_oracle.h"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_027.cpp"
    VERBATIM)
add_library(math_library_animation_oracle_model OBJECT "${PS2R_ANIMATION_ORACLE_OUTPUT}")
target_include_directories(math_library_animation_oracle_model PRIVATE "${PS2R_ANIMATION_ORACLE_ROOT}")
target_compile_options(math_library_animation_oracle_model PRIVATE
    -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
set_property(TARGET math_library_animation_oracle_model PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(test_math_library_animation_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_animation_oracle.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp"
    $<TARGET_OBJECTS:math_library_animation_oracle_model>)
target_include_directories(test_math_library_animation_oracle PRIVATE "${PS2R_ANIMATION_ORACLE_ROOT}")
target_link_libraries(test_math_library_animation_oracle PRIVATE ps2r_title_hle)
target_compile_options(test_math_library_animation_oracle PRIVATE -Wall -Wextra -Werror)
math_library_register_oracle(MathLibraryAnimationOracle test_math_library_animation_oracle)
