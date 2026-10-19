if(PS2R_PRODUCT_TESTS)
    # No support library, dispatch fixture or title: generic timing must link
    # without requiring any generated callable/shadow-stack surface.
    add_executable(test_boundary_completion
        "${PS2R_PRODUCT_ROOT}/tests/app/test_boundary_completion.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/boundary_completion.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/tick_profile.cpp")
    target_include_directories(test_boundary_completion PRIVATE "${PS2R_PRODUCT_ROOT}")
    target_compile_features(test_boundary_completion PRIVATE cxx_std_17)
    target_compile_options(test_boundary_completion PRIVATE -Wall -Wextra -Werror)
    add_test(NAME BoundaryCompletion COMMAND test_boundary_completion)
    set_tests_properties(BoundaryCompletion PROPERTIES TIMEOUT 10)
    include(${PS2R_PRODUCT_ROOT}/app/gui/checkpoint_support.cmake)
    if(TARGET ps2r_checkpoint_support)
        set(continuation_support ps2r_checkpoint_support)
    else()
        if(NOT TARGET ps2r_continuation_support)   # app/gui/clock.cmake may have made it already
            ps2r_add_checkpoint_support(ps2r_continuation_support)
        endif()
        set(continuation_support ps2r_continuation_support)
    endif()
    # Production SessionCheckpoint links ps2r_ee in root builds. This separate
    # copy tests the same source against the narrow host-state fixture support,
    # so no generated title or duplicate shadow-stack provider can be linked.
    add_library(ps2r_continuation_checkpoint STATIC ${PS2R_PRODUCT_ROOT}/app/host/session_checkpoint.cpp)
    target_link_libraries(ps2r_continuation_checkpoint PUBLIC ps2r_session_controls
        PRIVATE ${continuation_support})
    target_compile_options(ps2r_continuation_checkpoint PRIVATE -Wall -Wextra -Werror)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # The proofs below read the GENERATED common TU (the stack ABI fixture, the continuation metadata
    # check, the emitter pins): title tests, present only when the title is being generated.
    if(TARGET ps2r_title_recomp)
    set(PS2R_PRODUCT_RECOMP "${PS2R_RECOMP_GEN_DIR}")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated_stack.h"
        COMMAND "${Python3_EXECUTABLE}" "${PS2R_PRODUCT_ROOT}/tools/recompiler_v2/continuations.py"
            "${PS2R_PRODUCT_RECOMP}" --stack-fixture "${CMAKE_CURRENT_BINARY_DIR}/generated_stack.h"
        DEPENDS ps2r_title_recomp_gen "${PS2R_PRODUCT_RECOMP}/recompiled.cpp"
            "${PS2R_PRODUCT_ROOT}/tools/recompiler_v2/continuations.py"
            "${PS2R_PRODUCT_ROOT}/tools/recompiler_v2/continuation_audits.json"
        VERBATIM)
    set(product_proof_sources
        "${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/app/test_continuation_proofs.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated_stack.h"
        "${PS2R_PRODUCT_ROOT}/engine/ee/cpu/dispatch_table.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/core/log/ring_log.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/core/log/ring_decoder.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/core/log/ring_tags.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/boundary_completion.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/game_boundary.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/emu_domain.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/ee/timing/regime_detector.cpp"
        "${PS2R_PRODUCT_ROOT}/engine/gs/hle/sce/gs_sync.cpp")
    add_executable(test_continuation_proofs ${product_proof_sources})
    target_include_directories(test_continuation_proofs PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_continuation_proofs PRIVATE ps2r_continuation_checkpoint)
    target_compile_options(test_continuation_proofs PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ContinuationProofs COMMAND test_continuation_proofs)
    set_tests_properties(ContinuationProofs PROPERTIES TIMEOUT 15)
    if(PS2R_PRODUCT_GUI)
        # Separate executable keeps the dispatch/metadata proofs SDL-free.
        # The UI route uses the same narrow support and generated ABI fixture;
        # it never links generated title bodies, including in root builds.
        add_executable(test_session_boundary_capture ${product_proof_sources}
            "${PS2R_PRODUCT_ROOT}/app/host/platform_input.cpp")
        target_include_directories(test_session_boundary_capture PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_compile_definitions(test_session_boundary_capture PRIVATE PS2R_PRODUCT_BOUNDARY_UI_TESTS=1)
        target_link_libraries(test_session_boundary_capture PRIVATE ps2r_continuation_checkpoint
            ps2r_session_runtime ${product_input_support})
        target_include_directories(test_session_boundary_capture PRIVATE $<TARGET_PROPERTY:SDL2::SDL2,INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_options(test_session_boundary_capture PRIVATE -Wall -Wextra -Werror)
        add_test(NAME SessionBoundaryCapture COMMAND test_session_boundary_capture --session-boundary)
        set_tests_properties(SessionBoundaryCapture PROPERTIES TIMEOUT 25 RUN_SERIAL TRUE)
    endif()
    add_test(NAME ContinuationMetadata COMMAND "${Python3_EXECUTABLE}"
        "${PS2R_PRODUCT_ROOT}/tools/recompiler_v2/continuations.py" "${PS2R_PRODUCT_RECOMP}" --check)
    set_tests_properties(ContinuationMetadata PROPERTIES TIMEOUT 45)
    add_test(NAME ContinuationMetadataPins COMMAND "${Python3_EXECUTABLE}"
        "${PS2R_PRODUCT_ROOT}/tests/app/test_continuation_metadata.py")
    # The engine's script imports recomp_paths (this title's tests/recomp): hand it the directory.
    set_tests_properties(ContinuationMetadataPins PROPERTIES TIMEOUT 15
        ENVIRONMENT "${PS2R_RECOMP_TEST_ENV};PYTHONPATH=${PS2R_PRODUCT_ROOT}/games/slus_20909_crash_twinsanity/tests/recomp")
    else()
        message(STATUS "app/gui: ContinuationProofs / SessionBoundaryCapture / ContinuationMetadata* SKIPPED (no title ELF)")
    endif()   # TARGET ps2r_title_recomp
endif()
