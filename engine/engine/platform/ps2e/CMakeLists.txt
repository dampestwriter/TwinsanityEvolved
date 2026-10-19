# engine/platform/ps2e -- ps2e_draw: the C typed-draw surface for a non-recompiled producer (a decomp's native
# platform arm). One SDL window + the render::Backend an ARM supplies + one frame target behind a C header
# (ps2e_draw.h). ps2e_draw.cpp speaks engine/render/backend.h only; the arm (ps2e_draw_backend.h, one translation
# unit) creates the backend. PS2R_PS2E_DRAW_BACKEND selects it:
#   gl    (the default) ps2e_draw_backend_gl.cpp: a GL 4.1 core context on the window + GlDevice (ps2r_render_gl)
#   null  ps2e_draw_backend_null.cpp: no context, NullBackend (ps2r_render_null) -- every call validated and counted,
#         nothing rasterised: a host whose graphics API has no adapter yet (a console target's first boot) or a CI box
#         without a display.
set(PS2R_PS2E_DRAW_BACKEND "gl" CACHE STRING "ps2e_draw arm: gl (window + GL 4.1 + GlDevice) or null (NullBackend, nothing rasterised)")
set_property(CACHE PS2R_PS2E_DRAW_BACKEND PROPERTY STRINGS gl null)
include(${CMAKE_SOURCE_DIR}/cmake/Sdl2Dependency.cmake)

if(PS2R_PS2E_DRAW_BACKEND STREQUAL "gl")
    include(${CMAKE_SOURCE_DIR}/cmake/OpenGlDependency.cmake)
    add_library(ps2r_ps2e_draw STATIC ps2e_draw.cpp ps2e_draw_backend_gl.cpp present_pacer.cpp scene_interpolator.cpp)
    target_link_libraries(ps2r_ps2e_draw PUBLIC ps2r_render_gl)
elseif(PS2R_PS2E_DRAW_BACKEND STREQUAL "null")
    add_library(ps2r_ps2e_draw STATIC ps2e_draw.cpp ps2e_draw_backend_null.cpp present_pacer.cpp scene_interpolator.cpp)
    target_link_libraries(ps2r_ps2e_draw PUBLIC ps2r_render_null)
    message(STATUS "nativeee: ps2e_draw = the null arm (no GL context; draws counted, nothing rasterised)")
else()
    message(FATAL_ERROR "nativeee: PS2R_PS2E_DRAW_BACKEND=${PS2R_PS2E_DRAW_BACKEND} is neither gl nor null")
endif()
target_include_directories(ps2r_ps2e_draw PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(ps2r_ps2e_draw PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ps2r_ps2e_draw PUBLIC ps2r_platform_sdl ps2r_gs_ir ps2r_gs_format ps2r_core ps2r_host)

# ps2e_movie: the C movie-decoder surface for the same producer -- a program stream's (PSS) pictures and PCM out of
# the ipu/hle pieces (PssDemux, FfmpegVideoDecoder, FmvAudioCapture) without the sceMpeg context layout, the IPU
# frame writer or the tick pacer: the producer paces the feeding and keeps its own presentation clock.
add_library(ps2r_ps2e_movie STATIC ps2e_movie.cpp)
target_include_directories(ps2r_ps2e_movie PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(ps2r_ps2e_movie PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ps2r_ps2e_movie PUBLIC ps2r_ipu ps2r_core)
