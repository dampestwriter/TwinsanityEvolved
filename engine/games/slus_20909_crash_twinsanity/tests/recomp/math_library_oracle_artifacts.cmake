include_guard(GLOBAL)
set(PS2R_MATH_ORACLE_ARTIFACT_DIR "" CACHE PATH
    "Explicit output root for actual math oracle artifacts; empty disables writes")
# The cross-platform identity lane. Each oracle streams the SAME bytes the
# --artifacts receipt would write into one SHA-256 per channel and writes
# <dir>/<TestName>.digest (`channel,sha256` lines; verify/component_identity.py collects them
# into the oracles lane). Nothing else is written. Mutually exclusive with the artifact dir.
set(PS2R_MATH_ORACLE_DIGEST_DIR "" CACHE PATH
    "Output dir for the math oracles' per-channel SHA-256 digests (the component identity lane); empty disables")
if(PS2R_MATH_ORACLE_ARTIFACT_DIR AND PS2R_MATH_ORACLE_DIGEST_DIR)
    message(FATAL_ERROR "PS2R_MATH_ORACLE_ARTIFACT_DIR and PS2R_MATH_ORACLE_DIGEST_DIR are mutually exclusive")
endif()
get_filename_component(PS2R_MATH_ARTIFACT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
add_library(math_library_oracle_artifacts STATIC "${CMAKE_CURRENT_LIST_DIR}/math_library_oracle_artifacts.cpp")
target_include_directories(math_library_oracle_artifacts PUBLIC "${PS2R_MATH_ARTIFACT_ROOT}")
target_compile_options(math_library_oracle_artifacts PRIVATE -Wall -Wextra -Werror)
if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
add_test(NAME MathLibraryOracleArtifactsFormat COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/math_library_oracle_artifacts.py" --selftest)
set_tests_properties(MathLibraryOracleArtifactsFormat PROPERTIES TIMEOUT 60)

# The argument belongs to the registered command, so CTest's inventory and a
# receipt.command can match exactly. No environment-only recording switch.
function(math_library_register_oracle test_name target_name)
    target_link_libraries(${target_name} PRIVATE math_library_oracle_artifacts)
    if(PS2R_MATH_ORACLE_ARTIFACT_DIR)
        get_filename_component(output_root "${PS2R_MATH_ORACLE_ARTIFACT_DIR}" ABSOLUTE
            BASE_DIR "${CMAKE_BINARY_DIR}")
        add_test(NAME "${test_name}" COMMAND ${target_name}
            --artifacts "${output_root}/${test_name}")
        set_tests_properties("${test_name}" PROPERTIES TIMEOUT 300)
    elseif(PS2R_MATH_ORACLE_DIGEST_DIR)
        get_filename_component(digest_root "${PS2R_MATH_ORACLE_DIGEST_DIR}" ABSOLUTE
            BASE_DIR "${CMAKE_BINARY_DIR}")
        add_test(NAME "${test_name}" COMMAND ${target_name}
            --digest "${digest_root}/${test_name}.digest")
        set_tests_properties("${test_name}" PROPERTIES TIMEOUT 900)
    else()
        add_test(NAME "${test_name}" COMMAND ${target_name})
        set_tests_properties("${test_name}" PROPERTIES TIMEOUT 60)
    endif()
endfunction()
