include_guard(GLOBAL)
add_executable(test_texture_assets
    "${CMAKE_CURRENT_LIST_DIR}/test_texture_assets.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_data.cpp")
target_link_libraries(test_texture_assets PRIVATE ps2r_title_hle ps2r_gs_hle_texsrc ps2r_fake_backend)
target_compile_options(test_texture_assets PRIVATE -Wall -Wextra -Werror)
set(PS2R_TEXTURE_ASSET_LIST "" CACHE FILEPATH
    "Static original texture records enumerated by audit_texture_assets.py")
add_test(NAME TextureAssets COMMAND test_texture_assets ${PS2R_TEXTURE_ASSET_LIST})
set_tests_properties(TextureAssets PROPERTIES TIMEOUT 120)
if(PS2R_TEXTURE_ASSET_LIST)
    # The complete corpus includes 84 million original/model pixels. Debug
    # exhausted the unit deadline after 2,500 of 11,968 records.
    set_tests_properties(TextureAssets PROPERTIES TIMEOUT 900 LABELS "static-assets")
    add_test(NAME TextureAssetsComponents COMMAND test_texture_assets)
    set_tests_properties(TextureAssetsComponents PROPERTIES TIMEOUT 120)
endif()
add_executable(test_texture_bindings
    "${CMAKE_CURRENT_LIST_DIR}/test_texture_bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_binding_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_data.cpp")
target_link_libraries(test_texture_bindings PRIVATE ps2r_title_hle ps2r_gs_hle_texsrc ps2r_fake_backend)
target_link_libraries(test_texture_bindings PRIVATE ps2r_private_fixtures)  # the data map: a private fixture (SKIP 77)
target_compile_options(test_texture_bindings PRIVATE -Wall -Wextra -Werror)
add_test(NAME TextureBindings COMMAND test_texture_bindings)
set_tests_properties(TextureBindings PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# Textures at load: the owned WriteTextureUpload adapter.
add_executable(test_texture_uploads
    "${CMAKE_CURRENT_LIST_DIR}/test_texture_uploads.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_binding_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_data.cpp")
target_link_libraries(test_texture_uploads PRIVATE ps2r_title_hle ps2r_gs_hle_texsrc ps2r_fake_backend)
target_link_libraries(test_texture_uploads PRIVATE ps2r_private_fixtures)  # the data map: a private fixture (SKIP 77)
target_compile_options(test_texture_uploads PRIVATE -Wall -Wextra -Werror)
add_test(NAME TextureUploads COMMAND test_texture_uploads)
set_tests_properties(TextureUploads PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# A4 texture packs: the content hash, the pack directory, Load applying the pack,
# the slot route at another resolution, --dump-textures and its round trip.
add_executable(test_texture_pack
    "${CMAKE_CURRENT_LIST_DIR}/test_texture_pack.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_binding_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_data.cpp")
target_link_libraries(test_texture_pack PRIVATE ps2r_title_hle ps2r_gs_hle_texsrc ps2r_fake_backend)
target_link_libraries(test_texture_pack PRIVATE ps2r_private_fixtures)  # the data map: a private fixture (SKIP 77)
target_compile_options(test_texture_pack PRIVATE -Wall -Wextra -Werror)
add_test(NAME TexturePack COMMAND test_texture_pack "${CMAKE_BINARY_DIR}/test_scratch")
set_tests_properties(TexturePack PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(TARGET ps2r_title_recomp)   # title test(s): TextureBoundaries / TextureOwnerBoundaries audits -- read the GENERATED shards; skipped without a title ELF
add_test(NAME TextureBoundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/audit_texture_boundaries.py")
set_tests_properties(TextureBoundaries PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "${PS2R_RECOMP_TEST_ENV}")  # reads the private data map
add_test(NAME TextureOwnerBoundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/audit_texture_owner_boundaries.py")
set_tests_properties(TextureOwnerBoundaries PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "${PS2R_RECOMP_TEST_ENV}")  # reads the private data map
endif()
add_executable(test_texture_state
    "${CMAKE_CURRENT_LIST_DIR}/test_texture_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_data.cpp")
target_link_libraries(test_texture_state PRIVATE ps2r_title_hle ps2r_gs_hle_texsrc ps2r_fake_backend)
target_compile_options(test_texture_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME TextureState COMMAND test_texture_state)
set_tests_properties(TextureState PROPERTIES TIMEOUT 120)
add_executable(test_texture_selection
    "${CMAKE_CURRENT_LIST_DIR}/test_texture_selection.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_binding_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_test_data.cpp")
target_link_libraries(test_texture_selection PRIVATE ps2r_title_hle ps2r_fake_backend)
target_link_libraries(test_texture_selection PRIVATE ps2r_private_fixtures)  # the data map: a private fixture (SKIP 77)
target_compile_options(test_texture_selection PRIVATE -Wall -Wextra -Werror)
add_test(NAME TextureSelection COMMAND test_texture_selection)
set_tests_properties(TextureSelection PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# The legacy TXAS derivation over every inventoried frozen anchor (no game process).
add_executable(test_texture_import_anchors
    "${CMAKE_CURRENT_LIST_DIR}/test_texture_import_anchors.cpp")
target_link_libraries(test_texture_import_anchors PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_texture_import_anchors PRIVATE -Wall -Wextra -Werror)
# The frozen coldboot probe dir (old_b60.ps2st) is a private fixture: {PRIVATE_FIXTURES} = $NATIVEEE_PRIVATE_FIXTURES.
ps2r_add_private_fixture_test(NAME TextureImportAnchors
    FIXTURES games/${PS2R_TITLE}/verify/baseline/coldboot/probe/old_b60.ps2st
    COMMAND test_texture_import_anchors
    "${CMAKE_SOURCE_DIR}/games/${PS2R_TITLE}/verify/restore_mhash_expected.tsv" "${PS2R_OG_ROOT}"
    "{PRIVATE_FIXTURES}/games/${PS2R_TITLE}/verify/baseline/coldboot/probe")
set_tests_properties(TextureImportAnchors PROPERTIES TIMEOUT 300)
add_executable(test_material_certificate
    "${CMAKE_CURRENT_LIST_DIR}/test_material_certificate.cpp")
target_link_libraries(test_material_certificate PRIVATE ps2r_title_hle)
target_compile_options(test_material_certificate PRIVATE -Wall -Wextra -Werror)
add_test(NAME MaterialCertificate COMMAND test_material_certificate)
set_tests_properties(MaterialCertificate PROPERTIES TIMEOUT 30)
add_executable(test_texture_consumers
    "${CMAKE_CURRENT_LIST_DIR}/test_texture_consumers.cpp")
target_link_libraries(test_texture_consumers PRIVATE ps2r_title_hle)
target_compile_options(test_texture_consumers PRIVATE -Wall -Wextra -Werror)
add_test(NAME TextureConsumers COMMAND test_texture_consumers)
set_tests_properties(TextureConsumers PROPERTIES TIMEOUT 30)
add_test(NAME TextureConsumersGraph
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_texture_consumers_graph.py")
set_tests_properties(TextureConsumersGraph PROPERTIES TIMEOUT 30)
set(PS2R_TEXTURE_CONSUMERS_INVENTORY "" CACHE FILEPATH
    "texture_consumers.json emitted by audit_texture_consumers.py for catalogue regeneration checks")
if(PS2R_TEXTURE_CONSUMERS_INVENTORY)
    add_test(NAME TextureConsumersCatalogue
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/texture_consumers_catalogue.py"
            "${PS2R_TEXTURE_CONSUMERS_INVENTORY}"
            "${CMAKE_CURRENT_LIST_DIR}/../../../hle/graphics/textures/texture_consumers_catalogue"
            --check)
    set_tests_properties(TextureConsumersCatalogue PROPERTIES TIMEOUT 30)
endif()
# The complete PSM/PSF pair catalogue (TexturePairs) regenerated from the
# retail archive and compared with the committed partitions; SKIP 77 when the
# archive (PS2R_OG_ROOT/game_files/Crash_TwinSanity.rep/CRASH6) is absent.
add_test(NAME TexturePairsCatalogue
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/audit_texture_pairs.py"
        --archive "${PS2R_OG_ROOT}/game_files/Crash_TwinSanity.rep/CRASH6"
        --textures-dir "${CMAKE_SOURCE_DIR}/games/${PS2R_TITLE}/hle/graphics/textures" --check --skip-missing)
set_tests_properties(TexturePairsCatalogue PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
add_test(NAME MaterialGraph
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_material_graph.py")
set_tests_properties(MaterialGraph PROPERTIES TIMEOUT 30)
if(TARGET ps2r_title_recomp)   # title test(s): MaterialBoundaries audit -- read the GENERATED shards; skipped without a title ELF
add_test(NAME MaterialBoundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/audit_material_boundaries.py")
set_tests_properties(MaterialBoundaries PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "${PS2R_RECOMP_TEST_ENV}")  # reads the private data map
endif()
