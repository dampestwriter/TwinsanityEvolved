# Parent owns shared CMake. Include this fragment only alongside compiling
# hle/math/collision_math.cpp into ps2r_title_hle; runtime enablement is separate.
if(TARGET ps2r_title_recomp)   # the generated-body oracle (title test); MathLibraryCollision below stays
include("${CMAKE_CURRENT_LIST_DIR}/math_library_oracle_artifacts.cmake")
get_filename_component(PS2R_COLLISION_ORACLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(PS2R_COLLISION_ORACLE_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_collision_oracle.py")
set(PS2R_COLLISION_ORACLE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/math_library_collision_oracle_generated.cpp")
if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
add_custom_command(
    OUTPUT "${PS2R_COLLISION_ORACLE_OUTPUT}"
    COMMAND ${PS2R_RECOMP_ENV} "${Python3_EXECUTABLE}" "${PS2R_COLLISION_ORACLE_GENERATOR}"
        --root "${PS2R_COLLISION_ORACLE_ROOT}" --output "${PS2R_COLLISION_ORACLE_OUTPUT}"
    DEPENDS ps2r_title_recomp_gen "${PS2R_COLLISION_ORACLE_GENERATOR}"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_oracle.py"
        "${CMAKE_CURRENT_LIST_DIR}/math_library_collision_oracle.h"
        "${PS2R_RECOMP_GEN_DIR}/recompiled_shard_027.cpp"
    VERBATIM)
add_library(math_library_collision_oracle_model OBJECT "${PS2R_COLLISION_ORACLE_OUTPUT}")
target_include_directories(math_library_collision_oracle_model PRIVATE "${PS2R_COLLISION_ORACLE_ROOT}")
target_compile_options(math_library_collision_oracle_model PRIVATE
    -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
set_property(TARGET math_library_collision_oracle_model PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(test_math_library_collision_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_collision_oracle.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp"
    $<TARGET_OBJECTS:math_library_collision_oracle_model>)
target_include_directories(test_math_library_collision_oracle PRIVATE "${PS2R_COLLISION_ORACLE_ROOT}")
target_link_libraries(test_math_library_collision_oracle PRIVATE ps2r_title_hle)
target_compile_options(test_math_library_collision_oracle PRIVATE -Wall -Wextra -Werror)
math_library_register_oracle(MathLibraryCollisionOracle test_math_library_collision_oracle)
endif()
add_executable(test_math_library_collision "${CMAKE_CURRENT_LIST_DIR}/test_math_library_collision.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_dispatch_fixture.cpp")
target_link_libraries(test_math_library_collision PRIVATE ps2r_title_hle)
target_compile_options(test_math_library_collision PRIVATE -Wall -Wextra -Werror)
add_test(NAME MathLibraryCollision COMMAND test_math_library_collision)
set_tests_properties(MathLibraryCollision PROPERTIES TIMEOUT 60)
