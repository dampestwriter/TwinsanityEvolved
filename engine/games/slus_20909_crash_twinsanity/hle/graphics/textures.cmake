# Parent includes after ps2r_title_hle and ps2r_gs_format are declared.
include_guard(GLOBAL)
target_sources(ps2r_title_hle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/graphics_refusals.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_codec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_assets.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_uploads.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_certificate.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_consumers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_dump.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_import.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_owners.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_pack.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_pairs.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_selection.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_state_codec.cpp")
target_sources(ps2r_title_hle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_pairs_catalogue/identity.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_pairs_catalogue/rows_0.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_pairs_catalogue/rows_1.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_pairs_catalogue/rows_2.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_pairs_catalogue/rows_3.cpp")
target_sources(ps2r_title_hle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/identity.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/accepted_0.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/accepted_1.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/accepted_2.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/accepted_3.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/refused_0.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/refused_1.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/refused_2.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/refused_3.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/refused_4.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/material_catalogue/refused_5.cpp")
target_sources(ps2r_title_hle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_consumers_catalogue/identity.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_consumers_catalogue/rows_0.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_consumers_catalogue/rows_1.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_consumers_catalogue/rows_2.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_consumers_catalogue/rows_3.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/textures/texture_consumers_catalogue/rows_4.cpp")
target_link_libraries(ps2r_title_hle PUBLIC ps2r_gs_format ps2r_gs_hle_texsrc)
