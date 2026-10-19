# The owned menu extension: the title's options menu grows a
# 'Display' and a 'Controls' page built from the title's own page/item structures.
# Include after ps2r_title_hle is declared; the composition root installs it in
# play mode under Config ui.menu_pages.
include_guard(GLOBAL)
target_sources(ps2r_title_hle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/menu_layout.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/menu_strings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/menu_text_table.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/menu_extension.cpp")
