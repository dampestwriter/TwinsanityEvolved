# The title-bound rows of tests/gs: the renderer profile, the class renderers it registers and
# the DrawIR queue consumer mapped through it.
include_guard(GLOBAL)
set(_gs "${PS2R_TITLE_TESTS}/gs")
set(_gs_engine "${CMAKE_SOURCE_DIR}/tests/gs")   # the engine's shared helpers (origin/main)

add_executable(test_renderer_profile ${_gs}/targets/test_renderer_profile.cpp)
target_link_libraries(test_renderer_profile PRIVATE ps2r_title_renderer_profile)
target_compile_options(test_renderer_profile PRIVATE -Wall -Wextra -Werror -O2)
add_test(NAME Profile_VramLayoutEmptyIsFalse COMMAND test_renderer_profile)
set_tests_properties(Profile_VramLayoutEmptyIsFalse PROPERTIES TIMEOUT 120)

add_executable(test_classrdr ${_gs}/classrdr/test_classrdr.cpp)
target_link_libraries(test_classrdr PRIVATE ps2r_title_renderer_profile ps2r_fake_backend)
target_compile_options(test_classrdr PRIVATE -Wall -Wextra -Werror -O2)
add_test(NAME Classrdr_Classify COMMAND test_classrdr classify)
add_test(NAME Gs_Inv29_DispatchOrderFallback COMMAND test_classrdr dispatch)
add_test(NAME Classrdr_AcceptTuples COMMAND test_classrdr accept)
add_test(NAME Classrdr_LiveSnapshot COMMAND test_classrdr livesnap)
add_test(NAME Classrdr_S9Recipe COMMAND test_classrdr sequence)
add_test(NAME Classrdr_ShuffleSnap COMMAND test_classrdr snap)
add_test(NAME Classrdr_DrawPipelineOrder COMMAND test_classrdr pipeline)
add_test(NAME Classrdr_EmitCountersGrammar COMMAND test_classrdr grammar)
add_test(NAME Profile_RegisterClassRenderers COMMAND test_classrdr profile)
add_test(NAME Classrdr_LeavingCapture COMMAND test_classrdr hooks)
add_test(NAME Classrdr_S9StencilPasses COMMAND test_classrdr s9passes)
add_test(NAME Classrdr_S9Steer COMMAND test_classrdr s9steer)
add_test(NAME Classrdr_S9Emit COMMAND test_classrdr s9emit)
add_test(NAME Classrdr_S9Lifetime COMMAND test_classrdr s9lifetime)
set_tests_properties(Classrdr_Classify Gs_Inv29_DispatchOrderFallback Classrdr_AcceptTuples Classrdr_S9Recipe
    Classrdr_ShuffleSnap Classrdr_DrawPipelineOrder Classrdr_EmitCountersGrammar Profile_RegisterClassRenderers
    Classrdr_LeavingCapture Classrdr_S9StencilPasses Classrdr_S9Steer Classrdr_S9Emit Classrdr_S9Lifetime
    PROPERTIES TIMEOUT 120)

add_executable(test_draw_ir_queue_consumer ${_gs}/queue/test_draw_ir_queue_consumer.cpp)
target_link_libraries(test_draw_ir_queue_consumer PRIVATE ps2r_gs_hle_queue ps2r_title_renderer_profile ps2r_fake_backend)
target_compile_options(test_draw_ir_queue_consumer PRIVATE -Wall -Wextra -Werror -O2)
add_test(NAME DrawIrQueueConsumerMapping COMMAND test_draw_ir_queue_consumer mapping)
add_test(NAME DrawIrQueueConsumerDrawOrder COMMAND test_draw_ir_queue_consumer draws)
add_test(NAME DrawIrQueueConsumerFrame COMMAND test_draw_ir_queue_consumer frame)
add_test(NAME DrawIrQueueConsumerFrameHash COMMAND test_draw_ir_queue_consumer framehash)
set_tests_properties(DrawIrQueueConsumerMapping DrawIrQueueConsumerDrawOrder DrawIrQueueConsumerFrame PROPERTIES TIMEOUT 60)
