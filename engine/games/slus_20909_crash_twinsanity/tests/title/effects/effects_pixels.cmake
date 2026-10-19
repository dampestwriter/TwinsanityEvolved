# Pixel self-oracles for the effects passes. Included through cmake_language(DEFER) from
# tests.cmake so it runs after the including directory has processed engine/render and
# engine/platform/sdl; the TARGET guards below are therefore decided by whether those
# libraries exist in the configuration, never by include order.
include_guard(GLOBAL)
if(NOT TARGET ps2r_render_gl OR NOT TARGET ps2r_platform_sdl)
    message(STATUS "effects: ps2r_render_gl/ps2r_platform_sdl absent; pixel self-oracles not registered")
    return()
endif()
# The three pixel oracles below compile the distortion/screen GENERATED-body TUs (title tests).
if(TARGET ps2r_vu AND NOT TARGET ps2r_title_recomp)
    message(STATUS "effects: DistortionPixelsSelfOracle / WavePixelsSelfOracle / ScreenPixelsSelfOracle SKIPPED (no title ELF)")
endif()
if(TARGET ps2r_vu AND TARGET ps2r_title_recomp)
    add_executable(test_distortion_pixels
        "${CMAKE_CURRENT_LIST_DIR}/test_distortion_pixels.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/distortion_fixture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/distortion_oracle.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/distortion_material_oracle.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/effects_pixel_fixture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/effects_pixel_device.cpp"
        "${distortion_generated}/distortion_material_generated.cpp")
    target_compile_options(test_distortion_pixels PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(test_distortion_pixels PRIVATE ps2r_title_hle ps2r_gs_state ps2r_vu
        ps2r_render_gl ps2r_platform_sdl)
    add_test(NAME DistortionPixelsSelfOracle COMMAND test_distortion_pixels ${PS2R_DISTORTION_PROGRAM})
    set_tests_properties(DistortionPixelsSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(test_wave_pixels
        "${CMAKE_CURRENT_LIST_DIR}/test_wave_pixels.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/effects_pixel_fixture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/effects_pixel_device.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/wave_oracle.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/screen_oracle.cpp"
        "${screen_generated_dir}/screen_generated.cpp")
    target_compile_options(test_wave_pixels PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(test_wave_pixels PRIVATE ps2r_title_hle ps2r_gs_state ps2r_vu
        ps2r_render_gl ps2r_platform_sdl)
    add_test(NAME WavePixelsSelfOracle COMMAND test_wave_pixels ${PS2R_WAVE_PROGRAM})
    set_tests_properties(WavePixelsSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(test_screen_pixels
        "${CMAKE_CURRENT_LIST_DIR}/test_screen_pixels.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/effects_pixel_fixture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/effects_pixel_device.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/screen_oracle.cpp"
        "${screen_generated_dir}/screen_generated.cpp")
    target_compile_options(test_screen_pixels PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(test_screen_pixels PRIVATE ps2r_title_hle ps2r_gs_state ps2r_gs_format ps2r_vu
        ps2r_render_gl ps2r_platform_sdl)
    add_test(NAME ScreenPixelsSelfOracle COMMAND test_screen_pixels)
    set_tests_properties(ScreenPixelsSelfOracle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
add_executable(test_shadow_pixels
    "${CMAKE_CURRENT_LIST_DIR}/test_shadow_pixels.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_fixture.cpp")
target_compile_options(test_shadow_pixels PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_shadow_pixels PRIVATE ps2r_title_hle ps2r_render_gl ps2r_platform_sdl)
add_test(NAME ShadowPixels COMMAND test_shadow_pixels)
set_tests_properties(ShadowPixels PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
