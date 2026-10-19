# Include after ps2r_title_hle is declared. Both services are selected/wired by
# the composition root; merely compiling them does not change the default.
include_guard(GLOBAL)
target_sources(ps2r_title_hle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/input.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/input_bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/snapshot_bytes.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/input_state_codec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/input_legacy_import.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/input_save_checkpoint.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../saves/save_storage.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../saves/save_bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../saves/save_state_codec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../saves/save_legacy_import.cpp")
