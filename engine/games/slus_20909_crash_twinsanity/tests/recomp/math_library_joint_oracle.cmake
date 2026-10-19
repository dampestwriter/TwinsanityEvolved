set(PS2R_MATH_ORACLE_ELF "" CACHE FILEPATH "Reviewed NTSC ELF for source math model oracles")
if(NOT PS2R_MATH_ORACLE_ELF)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_joint_image "${CMAKE_CURRENT_BINARY_DIR}/math_library_joint_image.h")
add_custom_command(
    OUTPUT "${_joint_image}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_joint_oracle.py"
        --elf "${PS2R_MATH_ORACLE_ELF}" --output "${_joint_image}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_joint_oracle.py"
        "${PS2R_MATH_ORACLE_ELF}"
    VERBATIM)
add_executable(test_math_library_joint_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_joint_oracle.cpp" "${_joint_image}")
target_include_directories(test_math_library_joint_oracle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(test_math_library_joint_oracle PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_math_library_joint_oracle PRIVATE ps2r_title_hle ps2r_vu)
add_test(NAME MathLibraryJointOracle COMMAND test_math_library_joint_oracle)
set_tests_properties(MathLibraryJointOracle PROPERTIES TIMEOUT 120)
