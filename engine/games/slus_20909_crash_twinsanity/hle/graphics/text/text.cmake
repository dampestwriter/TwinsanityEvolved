include_guard(GLOBAL)
target_sources(ps2r_title_hle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/text_layout.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/text_draw.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/text_intent.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/text_service.cpp")
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/text_layout.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/text_draw.cpp"
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
