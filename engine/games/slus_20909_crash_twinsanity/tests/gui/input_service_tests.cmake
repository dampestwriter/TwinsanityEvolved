if(PS2R_PRODUCT_TESTS)
    add_executable(test_product_input_version "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/title/test_input_version.cpp")
    target_link_libraries(test_product_input_version PRIVATE ps2r_product_save_services)
    target_compile_options(test_product_input_version PRIVATE -Wall -Wextra -Werror)
    add_test(NAME InputVersion COMMAND test_product_input_version)
    set_tests_properties(InputVersion PROPERTIES TIMEOUT 20)
    if(NOT TARGET ps2r_title_hle)
        # Reuse the actual title contract tests in an isolated module. Model
        # sources serve only as same-input report/import oracles in these tests.
        add_library(ps2r_product_pad_oracle STATIC
            "${PS2R_PRODUCT_ROOT}/engine/pad/hle/pad_protocol.cpp"
            "${PS2R_PRODUCT_ROOT}/engine/pad/savestate/pads_section.cpp")
        target_link_libraries(ps2r_product_pad_oracle PUBLIC ps2r_product_save_services)
        function(product_input_test source name)
            add_executable("test_product_${source}" "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/title/test_${source}.cpp")
            target_link_libraries("test_product_${source}" PRIVATE ps2r_product_pad_oracle)
            target_compile_options("test_product_${source}" PRIVATE -Wall -Wextra -Werror)
            add_test(NAME "${name}" COMMAND "test_product_${source}")
            set_tests_properties("${name}" PROPERTIES TIMEOUT 120)
        endfunction()
        product_input_test(input_service InputService)
        product_input_test(input_save_state_codecs InputSaveStateCodecs)
        target_sources(test_product_input_save_state_codecs PRIVATE
            "${PS2R_PRODUCT_ROOT}/engine/core/savestate/quiescence.cpp")
        product_input_test(input_legacy_import InputLegacyImport)
        product_input_test(input_save_checkpoint InputSaveCheckpoint)
        target_sources(test_product_input_save_checkpoint PRIVATE
            "${PS2R_PRODUCT_ROOT}/tests/app/checkpoint_parse_fixture.cpp")
        if(PS2R_PRODUCT_GUI)
            product_input_test(input_edges InputEdges)
            target_link_libraries(test_product_input_edges PRIVATE ${product_input_support})
        endif()
    endif()
endif()
