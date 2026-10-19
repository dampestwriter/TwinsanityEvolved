# Include after math_library_joint_oracle.cmake has established the reviewed image.
if(NOT PS2R_MATH_ORACLE_ELF)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/math_library_oracle_artifacts.cmake")
get_filename_component(_joint_animate_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_joint_animate_output "${CMAKE_CURRENT_BINARY_DIR}/math_library_joint_animate_oracle_generated.cpp")
set(_joint_animate_manifest "${CMAKE_CURRENT_BINARY_DIR}/math_library_joint_animate_closure.json")
set(_joint_animate_call_sources ${PS2R_RECOMP_SHARD_FILES})   # the GENERATED shards (build dir)
add_custom_command(
    OUTPUT "${_joint_animate_output}" "${_joint_animate_manifest}"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_joint_animate_oracle.py"
        --root "${_joint_animate_root}" --elf "${PS2R_MATH_ORACLE_ELF}"
        --output "${_joint_animate_output}" --manifest "${_joint_animate_manifest}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_joint_animate_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_joint_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/math_library_joint_animate_oracle.h"
        ${_joint_animate_call_sources} "${PS2R_MATH_ORACLE_ELF}"
        "${_joint_animate_root}/engine/ee/cpu/cpu_state.h"
        "${_joint_animate_root}/engine/ee/recomp_abi/host_app/cpu_state.h"
        "${_joint_animate_root}/engine/ee/cpu/ps2_float.h"
        "${_joint_animate_root}/engine/ee/cpu/ps2_float.inl"
    VERBATIM)
add_library(math_library_joint_animate_oracle_model OBJECT "${_joint_animate_output}")
target_include_directories(math_library_joint_animate_oracle_model PRIVATE "${_joint_animate_root}")
target_compile_options(math_library_joint_animate_oracle_model PRIVATE
    -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
set_property(TARGET math_library_joint_animate_oracle_model PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(test_math_library_joint_animate_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_joint_animate_oracle.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_joint_observations.cpp"
    "${_joint_image}" $<TARGET_OBJECTS:math_library_joint_animate_oracle_model>)
target_include_directories(test_math_library_joint_animate_oracle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(test_math_library_joint_animate_oracle PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_math_library_joint_animate_oracle PRIVATE ps2r_title_hle ps2r_vu)
math_library_register_oracle(MathLibraryJointAnimateOracle test_math_library_joint_animate_oracle)
# Three source programs, three decoder calls and two pose blends per control
# case under the NEW interpreter; the sanitizer preset needs the longer bound.
set_tests_properties(MathLibraryJointAnimateOracle PROPERTIES TIMEOUT 900)
