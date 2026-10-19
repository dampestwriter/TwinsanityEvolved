# The title-bound rows of tests/iop: the recompiled STREAM.IRX (stub surface, load/park, first RPC).
include_guard(GLOBAL)
set(_iop "${PS2R_TITLE_TESTS}/iop")
set(_iop_engine "${CMAKE_SOURCE_DIR}/tests/iop")   # the engine's shared helpers (origin/main)

if(TARGET ps2r_title_stream_irx)
    # The IOPM section codec over the title's anchor images, and the re-park oracle that boots the
    # real STREAM.IRX through the plugin (was tests/iop/CMakeLists.txt's title-gated row).
    add_executable(test_iopm_section ${_iop}/test_iopm_section.cpp)
    target_link_libraries(test_iopm_section PRIVATE ps2r_iop ps2r_title_stream_irx)
    target_compile_options(test_iopm_section PRIVATE -Wall -Wextra -Werror)
    add_test(NAME IopmSection COMMAND test_iopm_section "${PS2R_OG_ROOT}")
    set_tests_properties(IopmSection PROPERTIES TIMEOUT 120)
    # 4B: the STREAM.IRX regen manifest over the GENERATED output (tools/regen_from_frozen.sh --check --iop DIR).
    add_test(NAME IopRecompManifestIntact COMMAND ${CMAKE_SOURCE_DIR}/tools/regen_from_frozen.sh --check --iop ${PS2R_IOP_GEN_DIR})

    add_executable(test_stream_irx_stub_surface ${_iop}/test_stream_irx_stub_surface.cpp ${_iop_engine}/vector_ee_ram_sink.cpp)
    target_link_libraries(test_stream_irx_stub_surface PRIVATE ps2r_title_stream_irx)
    target_compile_options(test_stream_irx_stub_surface PRIVATE -Wall -Wextra -Werror)
    add_test(NAME StreamIrxStubSurface COMMAND test_stream_irx_stub_surface
        ${PS2R_IOP_GEN_DIR}/iop_recompiled.cpp)
    set_tests_properties(StreamIrxStubSurface PROPERTIES TIMEOUT 120)

    # 4B'': the plugin boots the real STREAM.IRX bytes under FRAME to its first block (heartbeat
    # armed, RPC thread in Wait::Rpc), and the IOPM boot-symmetric re-park oracle (mode `repark`).
    add_executable(test_stream_irx_loads_and_parks ${_iop}/test_stream_irx_loads_and_parks.cpp)
    target_link_libraries(test_stream_irx_loads_and_parks PRIVATE ps2r_title_stream_irx)
    target_compile_options(test_stream_irx_loads_and_parks PRIVATE -Wall -Wextra -Werror)
    add_test(NAME StreamIrxLoadsAndParks COMMAND test_stream_irx_loads_and_parks "${PS2R_OG_ROOT}")
    add_test(NAME IopmBootSymmetricRepark COMMAND test_stream_irx_loads_and_parks "${PS2R_OG_ROOT}" repark)
    set_tests_properties(StreamIrxLoadsAndParks IopmBootSymmetricRepark PROPERTIES TIMEOUT 120)

    # 4C'': the first EE -> STREAM.IRX RPC round trip through the real module; the INIT command's
    # libsd init shape (P4-CDINIT-SOUND-SHAPE) and the heartbeat hard timer.
    add_executable(test_stream_irx_serves_first_rpc ${_iop}/test_stream_irx_serves_first_rpc.cpp)
    target_link_libraries(test_stream_irx_serves_first_rpc PRIVATE ps2r_title_stream_irx)
    target_compile_options(test_stream_irx_serves_first_rpc PRIVATE -Wall -Wextra -Werror)
    add_test(NAME StreamIrxServesFirstRpc COMMAND test_stream_irx_serves_first_rpc "${PS2R_OG_ROOT}")
    set_tests_properties(StreamIrxServesFirstRpc PROPERTIES TIMEOUT 120)
else()
    message(STATUS "title tests/iop: the STREAM.IRX suites SKIPPED (no recompiled IOP module)")
endif()
