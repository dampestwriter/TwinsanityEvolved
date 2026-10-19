# The title-bound rows of tests/ee: the headroom query over the recompiled body, the restore
# mhash anchors, the savestate sections with the title's workaround hooks.
include_guard(GLOBAL)
set(_ee "${PS2R_TITLE_TESTS}/ee")
set(_ee_engine "${CMAKE_SOURCE_DIR}/tests/ee")   # the engine's shared helpers (origin/main)

# The headroom query FUN_00191bd0 runs its recompiled body over the dispatch install
# primitive and advances no tick (HeadroomQueryGlue). Links the GENERATED recompiled code.
if(TARGET ps2r_title_recomp)
    add_executable(test_headroom_query ${_ee}/test_headroom_query.cpp)
    target_link_libraries(test_headroom_query PRIVATE ps2r_title_recomp ps2r_title_hle ps2r_vu ps2r_gs_sync ps2r_ee)
    target_compile_options(test_headroom_query PRIVATE -Wall -Wextra -Werror)
    add_test(NAME HeadroomQueryRecompiled COMMAND test_headroom_query)
else()
    message(STATUS "title tests/ee: HeadroomQueryRecompiled SKIPPED (no title ELF)")
endif()

# The restore mhash anchors (OLD/save_states are read-only oracle inputs; SKIP 77 without them).
add_executable(test_restore_mhash ${_ee}/test_restore_mhash.cpp)
target_link_libraries(test_restore_mhash PRIVATE ps2r_title_api ps2r_title_hle ps2r_ee)
target_compile_options(test_restore_mhash PRIVATE -Wall -Wextra -Werror)
add_test(NAME RestoreMhashAnchors COMMAND test_restore_mhash
    "${PS2R_TITLE_SRC}/verify/restore_mhash_expected.tsv"
    "${PS2R_OG_ROOT}/save_states")
set_tests_properties(RestoreMhashAnchors PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_savestate_sections ${_ee}/test_savestate_sections.cpp)
target_link_libraries(test_savestate_sections PRIVATE ps2r_title_hle ps2r_ee)
target_compile_options(test_savestate_sections PRIVATE -Wall -Wextra -Werror)
add_test(NAME SavestateMetaArenaCodec COMMAND test_savestate_sections codec)
add_test(NAME SavestateValidateBeforePreRestore COMMAND test_savestate_sections order)
add_test(NAME SavestateSectionsRoundTrip COMMAND test_savestate_sections roundtrip)
add_test(NAME WorkaroundFreeListHook COMMAND test_savestate_sections freelist)
add_test(NAME WorkaroundKeyRedirect COMMAND test_savestate_sections keyredirect)
add_test(NAME SavestateHeapSection COMMAND test_savestate_sections heap)
set_tests_properties(SavestateHeapSection PROPERTIES TIMEOUT 60)
