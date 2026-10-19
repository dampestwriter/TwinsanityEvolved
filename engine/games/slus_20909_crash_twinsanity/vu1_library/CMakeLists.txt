# games/<title>/vu1_library -- the VU1 HLE program library (title data): the
# family signature, the 223-row registration table and the native programs.
# Links ps2r_vu only (the contract PODs, VuFloat, the round-toward-zero guard).
#
# Compiled at -O2 -fno-strict-aliasing -fwrapv like ps2r_vu: the programs are
# bit-identity code proven against the recorded corpus by tools/vu1_replay
# --hle; contraction is left at the compiler default and every fused step is
# written as std::fmaf so the setting cannot move a result.

add_library(ps2r_title_vu1_library STATIC
    qw_ptr.cpp
    library_common.cpp
    clip_gate.cpp
    near_clipper.cpp
    family_tristrip.cpp
    textured_tristrip.cpp
    poscolor_strip.cpp
    lit_tristrip.cpp
    crys_programs.cpp
    vu0_programs.cpp
    displace_prefix.cpp
    skinned_lit_strip.cpp
    indexed_displace.cpp
    zero_kick.cpp
    sprite_text.cpp
    vu1_loaded_programs.cpp
    vu1_library.cpp)
target_include_directories(ps2r_title_vu1_library PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(ps2r_title_vu1_library PRIVATE -Wall -Wextra -Werror -O2 -fno-strict-aliasing -fwrapv)
target_link_libraries(ps2r_title_vu1_library PUBLIC ps2r_vu)
