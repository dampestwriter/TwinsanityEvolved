# QoL: the cutscene fast-forward decision (qol.fast_forward).
include_guard(GLOBAL)
target_sources(ps2r_title_hle PRIVATE "${CMAKE_CURRENT_LIST_DIR}/cutscene_fast_forward.cpp")
