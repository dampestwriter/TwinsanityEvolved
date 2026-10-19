# Inline macro arithmetic needs no ELF/image; it is pure generated register math.
get_filename_component(_joint_lerp_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_joint_lerp_output "${CMAKE_CURRENT_BINARY_DIR}/math_library_joint_lerp_oracle_generated.cpp")
add_custom_command(
    OUTPUT "${_joint_lerp_output}"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_joint_lerp_oracle.py"
        --root "${_joint_lerp_root}" --output "${_joint_lerp_output}"
    DEPENDS ps2r_title_recomp_gen "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_joint_lerp_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/math_library_joint_lerp_oracle.h"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_027.cpp"
        "${_joint_lerp_root}/engine/ee/cpu/cpu_state.h"
        "${_joint_lerp_root}/engine/ee/recomp_abi/host_app/cpu_state.h"
        "${_joint_lerp_root}/engine/ee/cpu/ps2_float.h"
        "${_joint_lerp_root}/engine/ee/cpu/ps2_float.inl"
    VERBATIM)
add_library(math_library_joint_lerp_oracle_model OBJECT "${_joint_lerp_output}")
target_include_directories(math_library_joint_lerp_oracle_model PRIVATE "${_joint_lerp_root}")
target_compile_options(math_library_joint_lerp_oracle_model PRIVATE
    -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
set_property(TARGET math_library_joint_lerp_oracle_model PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(test_math_library_joint_lerp_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_joint_lerp_oracle.cpp"
    $<TARGET_OBJECTS:math_library_joint_lerp_oracle_model>)
target_compile_options(test_math_library_joint_lerp_oracle PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_math_library_joint_lerp_oracle PRIVATE ps2r_title_hle)
add_test(NAME MathLibraryJointLerpOracle COMMAND test_math_library_joint_lerp_oracle)
set_tests_properties(MathLibraryJointLerpOracle PROPERTIES TIMEOUT 120)
