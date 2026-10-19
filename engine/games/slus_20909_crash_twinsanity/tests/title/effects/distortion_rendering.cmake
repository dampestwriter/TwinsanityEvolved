add_executable(test_distortion_scene
    "${CMAKE_CURRENT_LIST_DIR}/test_distortion_scene.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_fixture.cpp")
target_compile_options(test_distortion_scene PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_distortion_scene PRIVATE ps2r_title_hle ps2r_fake_backend)
add_test(NAME DistortionScene COMMAND test_distortion_scene)
set_tests_properties(DistortionScene PROPERTIES TIMEOUT 120)
# test_distortion_pixels / DistortionPixelsSelfOracle live in effects_pixels.cmake (deferred).
add_executable(test_distortion_pass
    "${CMAKE_CURRENT_LIST_DIR}/test_distortion_pass.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_pass_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_fixture.cpp")
target_compile_options(test_distortion_pass PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_distortion_pass PRIVATE ps2r_title_hle)
add_test(NAME DistortionPass COMMAND test_distortion_pass)
set_tests_properties(DistortionPass PROPERTIES TIMEOUT 120)
add_executable(test_distortion_application "${CMAKE_CURRENT_LIST_DIR}/test_distortion_application.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_pass_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/effects_fixture.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/distortion_application.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/distortion_census.cpp")
target_compile_options(test_distortion_application PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_distortion_application PRIVATE ps2r_title_hle ps2r_fake_backend ps2r_ee ps2r_core)
add_test(NAME DistortionApplication COMMAND test_distortion_application)
set_tests_properties(DistortionApplication PROPERTIES TIMEOUT 120)
add_executable(test_distortion_state "${CMAKE_CURRENT_LIST_DIR}/test_distortion_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_pass_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_fixture.cpp")
target_compile_options(test_distortion_state PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_distortion_state PRIVATE ps2r_title_hle ps2r_core)
add_test(NAME DistortionState COMMAND test_distortion_state)
set_tests_properties(DistortionState PROPERTIES TIMEOUT 120)
add_executable(test_effects_application "${CMAKE_CURRENT_LIST_DIR}/test_effects_application.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_pass_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/distortion_fixture.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/shadow_application.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/shadow_census.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/distortion_application.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/distortion_census.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/screen_application.cpp"
    "${effects_repo}/games/slus_20909_crash_twinsanity/composition/effects_application.cpp")
target_compile_options(test_effects_application PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_effects_application PRIVATE ps2r_title_hle ps2r_ee ps2r_core)
add_test(NAME EffectsApplication COMMAND test_effects_application)
set_tests_properties(EffectsApplication PROPERTIES TIMEOUT 120)
