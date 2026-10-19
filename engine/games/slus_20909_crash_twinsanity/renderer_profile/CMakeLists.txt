# games/<title>/renderer_profile -- RendererProfile: the VRAM layout (and, from
# the class-renderer stage on, the class stamps / recipes) the GS HLE consults.
add_library(ps2r_title_renderer_profile STATIC renderer_profile.cpp)
target_include_directories(ps2r_title_renderer_profile PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(ps2r_title_renderer_profile PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ps2r_title_renderer_profile PUBLIC ps2r_gs_hle_targets ps2r_gs_hle_classrdr)
