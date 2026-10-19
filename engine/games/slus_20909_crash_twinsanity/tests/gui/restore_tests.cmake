# The generated shadow ABI fixture (generated_stack.h) is cut from the GENERATED common TU
# (continuation_tests.cmake): a title test, present only when the title is being generated.
if(PS2R_PRODUCT_TESTS AND PS2R_PRODUCT_GUI AND NOT TARGET ps2r_title_recomp)
    message(STATUS "app/gui: SessionRestoreComposition SKIPPED (no title ELF)")
endif()
if(PS2R_PRODUCT_TESTS AND PS2R_PRODUCT_GUI AND TARGET ps2r_title_recomp)
    if(NOT TARGET ps2r_core)
        add_subdirectory("${PS2R_PRODUCT_ROOT}/engine/core" product_restore_core EXCLUDE_FROM_ALL)
        target_include_directories(ps2r_core PUBLIC "${PS2R_PRODUCT_ROOT}")
        target_compile_features(ps2r_core PUBLIC cxx_std_17)
    endif()
    # Actual application transaction, host-state/SDL providers and generated
    # shadow ABI fixture. No generated title bodies or ps2r_ee dependency.
    add_executable(test_session_restore
        "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/app/test_session_restore.cpp"
        "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/app/restore_owner_fixture.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated_stack.h"
        ${PS2R_PRODUCT_ROOT}/app/host/session_restore.cpp ${PS2R_PRODUCT_ROOT}/app/host/session_restart.cpp ${PS2R_PRODUCT_ROOT}/app/host/session_workers.cpp
        ${PS2R_PRODUCT_ROOT}/app/host/session_clock.cpp ${PS2R_PRODUCT_ROOT}/app/host/session_files.cpp ${PS2R_PRODUCT_ROOT}/app/host/platform_input.cpp ${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/composition/input_save.cpp
        "${PS2R_PRODUCT_ROOT}/engine/ee/cpu/dispatch_table.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/boundary_completion.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/game_boundary.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/emu_domain.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/regime_detector.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/tick_source.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/tick_sync.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/mem/main_memory.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/mem/scratchpad.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/savestate/memory_sections.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/savestate/state_restorer.cpp")
    target_include_directories(test_session_restore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_options(test_session_restore PRIVATE -Wall -Wextra -Werror)
    # In the root build ps2r_platform_sdl -> ps2r_platform PUBLIC-links ps2r_core; naming the
    # core a second time makes ld warn about a duplicate library, so it is named only when the
    # isolated support library (no platform target) is what links it.
    set(restore_core_link ps2r_core)
    if(TARGET ps2r_platform_sdl)
        set(restore_core_link)
    endif()
    target_link_libraries(test_session_restore PRIVATE ps2r_continuation_checkpoint
        ps2r_session_runtime ps2r_product_save_services ${product_input_support} ${restore_core_link})
    target_include_directories(test_session_restore PRIVATE $<TARGET_PROPERTY:SDL2::SDL2,INTERFACE_INCLUDE_DIRECTORIES>)
    add_test(NAME SessionRestoreComposition COMMAND test_session_restore)
    set_tests_properties(SessionRestoreComposition PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()
