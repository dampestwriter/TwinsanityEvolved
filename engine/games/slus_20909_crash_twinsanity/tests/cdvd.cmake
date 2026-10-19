# The title-bound rows of tests/cdvd: the title's loader file-IO bodies, completion predicate,
# ring cache, spin lift and chunk census over the real disc (SKIP 77 without the image).
include_guard(GLOBAL)
set(_cdvd "${PS2R_TITLE_TESTS}/cdvd")
set(_cdvd_engine "${CMAKE_SOURCE_DIR}/tests/cdvd")   # the engine's shared helpers (origin/main)

# Stage 4F''': the title's five loader file-IO bodies over the real disc (byte-exact,
# delivery at the boundary, the blocking wait).
add_executable(test_loader_seams ${_cdvd}/test_loader_seams.cpp)
target_link_libraries(test_loader_seams PRIVATE ps2r_title_hle ps2r_cdvd ps2r_ee)
target_compile_options(test_loader_seams PRIVATE -Wall -Wextra -Werror)
if(EXISTS "${PS2R_DISC_IMAGE}")
    add_test(NAME LoaderSeams COMMAND test_loader_seams "${PS2R_DISC_IMAGE}")
else()
    add_test(NAME LoaderSeams COMMAND test_loader_seams)
endif()
set_tests_properties(LoaderSeams PROPERTIES TIMEOUT 60)

# The title's own completion predicate (the REAL recompiled FUN_002adaf0 ->
# FUN_001e3308 STREAM slot status) over a host-served async read; the oversized-window
# routing. Title tests: they link the GENERATED recompiled code.
if(TARGET ps2r_title_recomp)
    add_executable(test_loader_completion ${_cdvd}/test_loader_completion.cpp)
    target_link_libraries(test_loader_completion PRIVATE ps2r_title_hle ps2r_title_recomp ps2r_vu ps2r_gs_sync ps2r_cdvd ps2r_ee)
    target_compile_options(test_loader_completion PRIVATE -Wall -Wextra -Werror)
    if(EXISTS "${PS2R_DISC_IMAGE}")
        add_test(NAME LoaderCompletionOrder COMMAND test_loader_completion "${PS2R_DISC_IMAGE}")
    else()
        add_test(NAME LoaderCompletionOrder COMMAND test_loader_completion)
    endif()
    set_tests_properties(LoaderCompletionOrder PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

    add_executable(test_loader_completion_oversized ${_cdvd}/test_loader_completion_oversized.cpp)
    target_link_libraries(test_loader_completion_oversized PRIVATE ps2r_title_hle ps2r_title_recomp ps2r_vu ps2r_gs_sync ps2r_cdvd ps2r_ee)
    target_compile_options(test_loader_completion_oversized PRIVATE -Wall -Wextra -Werror)
    if(EXISTS "${PS2R_DISC_IMAGE}")
        add_test(NAME LoaderCompletionOversized COMMAND test_loader_completion_oversized "${PS2R_DISC_IMAGE}")
    else()
        add_test(NAME LoaderCompletionOversized COMMAND test_loader_completion_oversized)
    endif()
    set_tests_properties(LoaderCompletionOversized PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
else()
    message(STATUS "title tests/cdvd: LoaderCompletionOrder / LoaderCompletionOversized SKIPPED (no title ELF)")
endif()

# The loader ring cache over the coldboot request log (SKIP 77 without the image / console images).
add_executable(test_loader_ring_cache ${_cdvd}/test_loader_ring_cache.cpp)
target_link_libraries(test_loader_ring_cache PRIVATE ps2r_title_hle ps2r_cdvd ps2r_ee)
target_compile_options(test_loader_ring_cache PRIVATE -Wall -Wextra -Werror)
if(EXISTS "${PS2R_DISC_IMAGE}")
    add_test(NAME LoaderRingCache COMMAND test_loader_ring_cache "${PS2R_DISC_IMAGE}"
             "${_cdvd_engine}/fixtures/loader_requests_coldboot121.tsv"
             --console-dir "${PS2R_CONSOLE_IMAGES_DIR}")
else()
    add_test(NAME LoaderRingCache COMMAND test_loader_ring_cache)
endif()
set_tests_properties(LoaderRingCache PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

# The load-wait loop lift (LoaderSpinLift). No disc needed.
add_executable(test_loader_spin_lift ${_cdvd}/test_loader_spin_lift.cpp)
target_link_libraries(test_loader_spin_lift PRIVATE ps2r_title_hle ps2r_cdvd ps2r_ee)
target_compile_options(test_loader_spin_lift PRIVATE -Wall -Wextra -Werror)
add_test(NAME LoaderSpinLift COMMAND test_loader_spin_lift)
set_tests_properties(LoaderSpinLift PROPERTIES TIMEOUT 60)

# The title's chunk state machine as the load classifier (ChunkStateCensus). No disc needed.
add_executable(test_chunk_state_census ${_cdvd}/test_chunk_state_census.cpp)
target_link_libraries(test_chunk_state_census PRIVATE ps2r_title_hle ps2r_ee)
target_compile_options(test_chunk_state_census PRIVATE -Wall -Wextra -Werror)
add_test(NAME ChunkStateCensus COMMAND test_chunk_state_census)
set_tests_properties(ChunkStateCensus PROPERTIES TIMEOUT 60)

# A STREAM record the title stages itself served by the REAL recompiled STREAM.IRX under the
# cooperative IOP kernel (SKIP 77 without the image).
if(TARGET ps2r_title_recomp AND TARGET ps2r_title_stream_irx)
    add_executable(test_loader_completion_module ${_cdvd}/test_loader_completion_module.cpp)
    target_link_libraries(test_loader_completion_module PRIVATE ps2r_title_hle ps2r_title_recomp ps2r_title_stream_irx ps2r_vu ps2r_gs_sync ps2r_cdvd ps2r_sif ps2r_ee ps2r_iop)
    target_compile_options(test_loader_completion_module PRIVATE -Wall -Wextra -Werror)
    if(EXISTS "${PS2R_DISC_IMAGE}")
        add_test(NAME LoaderCompletionModule COMMAND test_loader_completion_module "${PS2R_DISC_IMAGE}" "${PS2R_OG_ROOT}" "${PS2R_TITLE_HLE_DIR}/sdk_bindings.tsv")
    else()
        add_test(NAME LoaderCompletionModule COMMAND test_loader_completion_module)
    endif()
    set_tests_properties(LoaderCompletionModule PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
else()
    message(STATUS "title tests/cdvd: LoaderCompletionModule SKIPPED (needs the title ELF and STREAM.IRX)")
endif()
