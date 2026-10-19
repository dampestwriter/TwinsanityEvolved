set(graphics_app_sources
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_producer_draws.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_producers.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_census.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_import.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_runtime.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/mesh_producer.cpp")
add_executable(test_graphics_checkpoint test_graphics_checkpoint.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_checkpoint.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_checkpoint_presentation.cpp")
target_link_libraries(test_graphics_checkpoint PRIVATE ps2r_title_hle ps2r_fake_backend)
target_compile_options(test_graphics_checkpoint PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsCheckpoint COMMAND test_graphics_checkpoint)
set_tests_properties(GraphicsCheckpoint PROPERTIES TIMEOUT 120)
add_executable(test_graphics_checkpoint_gl test_graphics_checkpoint_gl.cpp)
target_link_libraries(test_graphics_checkpoint_gl PRIVATE ps2r_title_hle ps2r_render_gl ps2r_platform_sdl)
target_compile_options(test_graphics_checkpoint_gl PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsCheckpointGL COMMAND test_graphics_checkpoint_gl)
set_tests_properties(GraphicsCheckpointGL PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
add_executable(test_graphics_display test_graphics_display.cpp)
target_link_libraries(test_graphics_display PRIVATE ps2r_title_hle ps2r_fake_backend
    ps2r_render_gl ps2r_platform_sdl)
target_compile_options(test_graphics_display PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsDisplay COMMAND test_graphics_display)
add_test(NAME GraphicsDisplayGL COMMAND test_graphics_display --gl)
set_tests_properties(GraphicsDisplay GraphicsDisplayGL PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
add_executable(test_graphics_frame_clear test_graphics_frame_clear.cpp)
target_link_libraries(test_graphics_frame_clear PRIVATE ps2r_title_hle ps2r_fake_backend
    ps2r_render_gl ps2r_platform_sdl)
target_compile_options(test_graphics_frame_clear PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsFrameClear COMMAND test_graphics_frame_clear)
add_test(NAME GraphicsFrameClearGL COMMAND test_graphics_frame_clear --gl)
set_tests_properties(GraphicsFrameClear GraphicsFrameClearGL PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
add_executable(test_graphics_frame test_graphics_frame.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_frame.cpp")
target_link_libraries(test_graphics_frame PRIVATE ps2r_title_hle)
target_compile_options(test_graphics_frame PRIVATE -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
if(APPLE)
    target_link_options(test_graphics_frame PRIVATE -Wl,-dead_strip)
elseif(UNIX)
    target_link_options(test_graphics_frame PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME GraphicsFrame COMMAND test_graphics_frame)
set_tests_properties(GraphicsFrame PROPERTIES TIMEOUT 120)
add_executable(test_graphics_strip test_graphics_strip.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_strip.cpp")
target_link_libraries(test_graphics_strip PRIVATE ps2r_title_hle ps2r_fake_backend
    ps2r_render_gl ps2r_platform_sdl)
target_compile_options(test_graphics_strip PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsStrip COMMAND test_graphics_strip)
add_test(NAME GraphicsStripGL COMMAND test_graphics_strip --gl)
set_tests_properties(GraphicsStrip GraphicsStripGL PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
add_executable(test_graphics_presentation_state test_graphics_presentation_state.cpp
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_checkpoint.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/graphics_checkpoint_presentation.cpp")
target_link_libraries(test_graphics_presentation_state PRIVATE ps2r_title_hle ps2r_fake_backend
    ps2r_render_gl ps2r_platform_sdl)
target_compile_options(test_graphics_presentation_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsPresentationState COMMAND test_graphics_presentation_state)
add_test(NAME GraphicsPresentationStateGL COMMAND test_graphics_presentation_state --gl)
set_tests_properties(GraphicsPresentationState GraphicsPresentationStateGL PROPERTIES
    TIMEOUT 120 SKIP_RETURN_CODE 77)
set(graphics_texture_fixture
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_backend.cpp"
    "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_data.cpp")
list(APPEND graphics_texture_fixture "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/tests/title/text/text_fixture.cpp")
foreach(check graphics_producers graphics_runtime)
    add_executable(test_${check} test_${check}.cpp ${graphics_app_sources} ${graphics_texture_fixture})
    # TextureBindingFixture reads the title's data map: a private fixture (SKIP 77 when absent).
    target_link_libraries(test_${check} PRIVATE ps2r_title_hle ps2r_gs_hle_queue ps2r_fake_backend ps2r_private_fixtures)
    target_compile_options(test_${check} PRIVATE -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
    # The unit entry uses the GL-free constructor; exclude the application's
    # VideoStack constructor and its process-only dispatch dependencies.
    if(APPLE)
        target_link_options(test_${check} PRIVATE -Wl,-dead_strip)
    elseif(UNIX)
        target_link_options(test_${check} PRIVATE -Wl,--gc-sections)
    endif()
endforeach()
add_test(NAME GraphicsProducers COMMAND test_graphics_producers)
add_test(NAME GraphicsRuntime COMMAND test_graphics_runtime)
set_tests_properties(GraphicsProducers GraphicsRuntime PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# The root composition of the owned mesh bucket (MeshProducer) over fake
# services -- the MaterialResourceRead receipts, the certificate's verdicts, the texture
# header/asset agreement, the FlushMaterial reservation and its slot delivery.
add_executable(test_mesh_producer test_mesh_producer.cpp "${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/composition/mesh_producer.cpp")
target_link_libraries(test_mesh_producer PRIVATE ps2r_title_hle ps2r_core)
target_compile_options(test_mesh_producer PRIVATE -Wall -Wextra -Werror)
add_test(NAME MeshProducer COMMAND test_mesh_producer)
set_tests_properties(MeshProducer PROPERTIES TIMEOUT 120)
add_executable(test_graphics_report test_graphics_report.cpp)
target_link_libraries(test_graphics_report PRIVATE ps2r_title_hle)
target_compile_options(test_graphics_report PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsReport COMMAND test_graphics_report)
set_tests_properties(GraphicsReport PROPERTIES TIMEOUT 60)
add_executable(test_graphics_refusals test_graphics_refusals.cpp)
target_link_libraries(test_graphics_refusals PRIVATE ps2r_title_hle)
target_compile_options(test_graphics_refusals PRIVATE -Wall -Wextra -Werror)
add_test(NAME GraphicsRefusals COMMAND test_graphics_refusals)
set_tests_properties(GraphicsRefusals PROPERTIES TIMEOUT 60)
# Toggle-pair frame attribution (verify/frame_pair_attribute.py: tick offset vs edge rows
# vs content from the two legs' --dump-frames images). run_leg's --dump-frames pass-through
# is pinned by the existing VerifyRunLegSelftest.
add_test(NAME VerifyFramePairAttributeSelftest
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/verify/frame_pair_attribute.py --selftest)
set_tests_properties(VerifyFramePairAttributeSelftest PROPERTIES TIMEOUT 60)
