# Before the mesh owner merges the common extension, an explicit isolated preview compiles
# the exact prepared mesh sources. Normal builds test their actual common owner.
option(PS2R_CLOTH_MESH_PREVIEW "Test the prepared M8 complete-material extension in isolation" OFF)
set(cloth_mesh_common "${effects_repo}/games/slus_20909_crash_twinsanity/hle/graphics")
file(READ "${cloth_mesh_common}/mesh_pass.h" cloth_mesh_header)
if(PS2R_CLOTH_MESH_PREVIEW)
    set(cloth_mesh_preview "${CMAKE_CURRENT_BINARY_DIR}/cloth_mesh_preview")
    set(cloth_mesh_relative "games/slus_20909_crash_twinsanity/hle/graphics")
    add_custom_command(
        OUTPUT "${cloth_mesh_preview}/${cloth_mesh_relative}/mesh_pass.cpp"
               "${cloth_mesh_preview}/${cloth_mesh_relative}/mesh_pass.h"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare_cloth_mesh_integration.py"
            --root "${effects_repo}" --out "${cloth_mesh_preview}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/prepare_cloth_mesh_integration.py"
            "${cloth_mesh_common}/mesh_pass.cpp" "${cloth_mesh_common}/mesh_pass.h"
        VERBATIM)
    add_library(cloth_mesh_preview STATIC
        "${cloth_mesh_preview}/${cloth_mesh_relative}/mesh_pass.cpp"
        "${cloth_mesh_common}/mesh_intent.cpp" "${cloth_mesh_common}/mesh_material.cpp")
    target_include_directories(cloth_mesh_preview BEFORE PUBLIC "${cloth_mesh_preview}")
    target_compile_options(cloth_mesh_preview PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(cloth_mesh_preview PUBLIC ps2r_title_hle)
    set(cloth_mesh_owner cloth_mesh_preview)
    add_executable(test_cloth_mesh_existing_guards "${effects_repo}/games/slus_20909_crash_twinsanity/tests/recomp/test_mesh_pass_guards.cpp")
    target_include_directories(test_cloth_mesh_existing_guards BEFORE PRIVATE "${cloth_mesh_preview}")
    target_compile_options(test_cloth_mesh_existing_guards PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(test_cloth_mesh_existing_guards PRIVATE cloth_mesh_preview)
    add_test(NAME ClothMeshExistingGuards COMMAND test_cloth_mesh_existing_guards)
    set_tests_properties(ClothMeshExistingGuards PROPERTIES TIMEOUT 120)
elseif(cloth_mesh_header MATCHES "capture_material")
    set(cloth_mesh_owner ps2r_title_hle)
else()
    add_test(NAME ClothMesh COMMAND "${Python3_EXECUTABLE}" -c
        "import sys; print('SKIP: M8 complete-material extension is not integrated'); sys.exit(77)")
    set_tests_properties(ClothMesh PROPERTIES SKIP_RETURN_CODE 77)
    return()
endif()
add_executable(test_cloth_mesh
    "${CMAKE_CURRENT_LIST_DIR}/test_cloth_mesh.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_backend.cpp" "${CMAKE_CURRENT_LIST_DIR}/effects_fixture.cpp")
if(PS2R_CLOTH_MESH_PREVIEW)
    target_include_directories(test_cloth_mesh BEFORE PRIVATE "${cloth_mesh_preview}")
endif()
target_compile_options(test_cloth_mesh PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_cloth_mesh PRIVATE ${cloth_mesh_owner} ps2r_fake_backend)
add_test(NAME ClothMesh COMMAND test_cloth_mesh)
set_tests_properties(ClothMesh PROPERTIES TIMEOUT 120)
