include_guard(GLOBAL)
target_sources(ps2r_title_hle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/sprite_draw.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/primitive_material.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sprite_intent.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sprite_service.cpp")
# Scalar source operation order is part of the interface.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/sprite_draw.cpp" PROPERTIES
    COMPILE_OPTIONS "-ffp-contract=off")
