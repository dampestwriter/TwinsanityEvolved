# The title-bound rows of tests/ipu: the scene-updater abort hold against the generated updater.
include_guard(GLOBAL)
set(_ipu "${PS2R_TITLE_TESTS}/ipu")
set(_ipu_engine "${CMAKE_SOURCE_DIR}/tests/ipu")   # the engine's shared helpers (origin/main)

# FmvAbortHold (the title's scene-updater abort hold, INTEGRATION_PLAN row 7.7): fires only under
# every guard, inert when MpegApi::ShouldHoldAbort is false, counts its clears, and its wrapper
# runs the generated updater after the tick.
add_executable(test_fmv_abort_hold ${_ipu}/test_fmv_abort_hold.cpp ${_ipu_engine}/vector_guest_memory.cpp
    ${_ipu_engine}/file_fmv_sector_source.cpp ${_ipu_engine}/counting_fmv_pcm_sink.cpp)
target_link_libraries(test_fmv_abort_hold PRIVATE ps2r_title_api ps2r_title_hle ps2r_ipu)
target_compile_options(test_fmv_abort_hold PRIVATE -Wall -Wextra -Werror)
add_test(NAME FmvAbortHold COMMAND test_fmv_abort_hold)
set_tests_properties(FmvAbortHold PROPERTIES TIMEOUT 120)
