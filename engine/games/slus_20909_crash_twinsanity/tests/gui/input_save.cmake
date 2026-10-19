if(PS2R_PRODUCT_TESTS OR NOT TARGET ps2r_title_hle)
    set(input_save_sources)
    set(input_service_root "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/hle")
    foreach(source IN ITEMS input/input.cpp input/input_bindings.cpp input/snapshot_bytes.cpp
            input/input_state_codec.cpp input/input_legacy_import.cpp input/input_save_checkpoint.cpp
            saves/save_storage.cpp saves/save_bindings.cpp saves/save_state_codec.cpp saves/save_legacy_import.cpp)
        list(APPEND input_save_sources "${input_service_root}/${source}")
    endforeach()
    add_library(ps2r_product_save_services STATIC ${input_save_sources}
        "${PS2R_PRODUCT_ROOT}/engine/core/verify/run_meta.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/mem/main_memory.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/mem/scratchpad.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/core/hash/hash.cpp")
    target_compile_options(ps2r_product_save_services PRIVATE -Wall -Wextra -Werror)
    if(TARGET ps2r_checkpoint_support)
        target_link_libraries(ps2r_product_save_services PUBLIC ps2r_checkpoint_support)
    else()
        target_link_libraries(ps2r_product_save_services PUBLIC ${continuation_support})
    endif()
endif()
include(${CMAKE_CURRENT_LIST_DIR}/input_service_tests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/input_caller_tests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/save_caller_tests.cmake)

# ps2r_input_save itself is the composition's (games/<title>/composition/CMakeLists.txt).
if(PS2R_PRODUCT_TESTS)
    # Same production composition with narrow actual service/codec support;
    # never links generated title bodies or pad/card modules in either build.
    add_executable(test_input_save_composition "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/app/test_input_save_composition.cpp"
        ${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/composition/input_save.cpp)
    target_link_libraries(test_input_save_composition PRIVATE ps2r_product_save_services ${product_clock_support})
    target_compile_options(test_input_save_composition PRIVATE -Wall -Wextra -Werror)
    if(PS2R_PRODUCT_GUI)
        target_sources(test_input_save_composition PRIVATE ${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/composition/input_sample.cpp ${PS2R_PRODUCT_ROOT}/app/host/platform_input.cpp
            "${PS2R_PRODUCT_ROOT}/engine/pad/hle/pad_protocol.cpp"
            "${PS2R_PRODUCT_ROOT}/engine/pad/hle/pad_stamp_duty.cpp"
            "${PS2R_PRODUCT_ROOT}/engine/core/triage/input_log.cpp")
        target_compile_definitions(test_input_save_composition PRIVATE PS2R_PRODUCT_SAMPLING_TESTS=1)
        target_link_libraries(test_input_save_composition PRIVATE ${product_input_support} SDL2::SDL2)
    endif()
    add_test(NAME InputSaveComposition COMMAND test_input_save_composition)
    set_tests_properties(InputSaveComposition PROPERTIES TIMEOUT 25)
    add_executable(test_input_save_clock "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/app/test_input_save_clock.cpp"
        "${PS2R_PRODUCT_ROOT}/tests/app/checkpoint_parse_fixture.cpp" ${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/composition/input_save.cpp)
    target_link_libraries(test_input_save_clock PRIVATE ps2r_product_save_services ${product_clock_support})
    target_compile_options(test_input_save_clock PRIVATE -Wall -Wextra -Werror)
    add_test(NAME InputSaveClock COMMAND test_input_save_clock)
    set_tests_properties(InputSaveClock PROPERTIES TIMEOUT 25)
    if(NOT TARGET ps2r_title_hle)
        # The root already registers SaveStorage. The isolated module runs
        # that same real file/store oracle without linking any title bodies.
        add_executable(test_product_save_storage "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/title/test_save_storage.cpp"
            "${PS2R_PRODUCT_ROOT}/engine/mc/hle/mc_host_store.cpp")
        target_link_libraries(test_product_save_storage PRIVATE ps2r_product_save_services)
        target_compile_options(test_product_save_storage PRIVATE -Wall -Wextra -Werror)
        add_test(NAME ProductSaveStorage COMMAND test_product_save_storage)
        set_tests_properties(ProductSaveStorage PROPERTIES TIMEOUT 30)
    endif()
endif()
