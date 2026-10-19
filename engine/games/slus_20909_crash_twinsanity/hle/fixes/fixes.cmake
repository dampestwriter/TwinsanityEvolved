# RETAIL_BUGS fixes: optional owned bodies over the title's own
# constructors, default off, one Config toggle each (fixes.*).
include_guard(GLOBAL)
target_sources(ps2r_title_hle PRIVATE "${CMAKE_CURRENT_LIST_DIR}/retail_fixes.cpp")
