# Parent includes this fragment after the title and NEW model targets exist.
# The source asset is explicit; normal unit runs do not require an external ELF.
set(PS2R_MATH_ORACLE_ELF "" CACHE FILEPATH "Reviewed NTSC ELF for source math model oracles")
if(NOT PS2R_MATH_ORACLE_ELF)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_math_culling_image "${CMAKE_CURRENT_BINARY_DIR}/math_library_culling_image.h")
add_custom_command(
    OUTPUT "${_math_culling_image}"
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_culling_oracle.py"
        --elf "${PS2R_MATH_ORACLE_ELF}" --output "${_math_culling_image}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/generate_math_library_culling_oracle.py"
        "${PS2R_MATH_ORACLE_ELF}"
    VERBATIM)
add_executable(test_math_library_culling_oracle
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_culling_oracle.cpp"
    "${_math_culling_image}")
target_include_directories(test_math_library_culling_oracle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(test_math_library_culling_oracle PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_math_library_culling_oracle PRIVATE ps2r_title_hle ps2r_vu)
add_test(NAME MathLibraryCullingOracle COMMAND test_math_library_culling_oracle)
set_tests_properties(MathLibraryCullingOracle PROPERTIES TIMEOUT 120)
add_executable(test_math_library_culling_state
    "${CMAKE_CURRENT_LIST_DIR}/test_math_library_culling_state.cpp"
    "${_math_culling_image}")
target_include_directories(test_math_library_culling_state PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(test_math_library_culling_state PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_math_library_culling_state PRIVATE ps2r_title_hle ps2r_vu)
add_test(NAME MathLibraryCullingState COMMAND test_math_library_culling_state)
set_tests_properties(MathLibraryCullingState PROPERTIES TIMEOUT 120)
