# ps2r_title_stream_irx -- the IOP recompiler's output for STREAM.IRX plus the
# ABI root, the stub surface that gives its 74 iop_stubs:: externs providers, the title layout,
# the LTI streaming protocol and the plugin that hosts the module under the kernel.
#
# iop_recompiled.{h,cpp} are GENERATED AT BUILD TIME into the build directory by the in-tree
# tools/iop_recompiler (ps2r_iop_recompiler) from the user's STREAM.IRX -- extracted from the
# disc image (NATIVEEE_DISC: CRASH6/SYS/STREAM.IRX) or given loose (NATIVEEE_STREAM_IRX); see
# cmake/TitleInputs.cmake -- and checked against MANIFEST.sha256 (the committed provenance).
# Nothing generated is committed; nothing generated is edited by hand.
# The generated TU compiles like Phase 3's common TU: -O0, no warnings, semantic flags only.
if(NOT PS2R_TITLE_IRX_AVAILABLE)
    message(STATUS "ps2r_title_stream_irx: SKIPPED -- no STREAM.IRX (see TitleInputs above); the recompiled IOP module, the oracle binary and the IOP tests are not built")
    return()
endif()
if(NOT TARGET ps2r_iop_recompiler)
    message(FATAL_ERROR "ps2r_title_stream_irx: the IOP recompiler target ps2r_iop_recompiler is required (PS2R_BUILD_RECOMPILER=ON)")
endif()

set(PS2R_IOP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/MANIFEST.sha256")
set(PS2R_IOP_GEN "${PS2R_IOP_GEN_DIR}")
file(MAKE_DIRECTORY "${PS2R_IOP_GEN}")
set(PS2R_IOP_HEADER "${PS2R_IOP_GEN}/iop_recompiled.h")
set(PS2R_IOP_COMMON "${PS2R_IOP_GEN}/iop_recompiled.cpp")
set(PS2R_IOP_STAMP "${PS2R_IOP_GEN}/MANIFEST.verified")
if(PS2R_RECOMP_CHECK_MANIFEST)
    set(_ps2r_strict ON)
else()
    set(_ps2r_strict OFF)
endif()
add_custom_command(
    OUTPUT ${PS2R_IOP_HEADER} ${PS2R_IOP_COMMON} ${PS2R_IOP_STAMP}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${PS2R_IOP_HEADER} ${PS2R_IOP_COMMON} ${PS2R_IOP_STAMP}
    COMMAND ${CMAKE_COMMAND}
        "-DLABEL=ps2r_title_stream_irx: recompiling ${PS2R_TITLE_IRX_DISC_PATH}"
        "-DLOG=${PS2R_IOP_GEN}/iop_gen.log" "-DWORKDIR=${PS2R_IOP_GEN}/cwd"
        "-DCMD=$<TARGET_FILE:ps2r_iop_recompiler>;--input;${PS2R_TITLE_IRX};--output-header;${PS2R_IOP_HEADER};--output-cpp;${PS2R_IOP_COMMON}"
        -P "${CMAKE_SOURCE_DIR}/cmake/ps2r_run_tool.cmake"
    COMMAND ${CMAKE_COMMAND}
        "-DLABEL=ps2r_title_stream_irx" "-DMANIFEST=${PS2R_IOP_MANIFEST}" "-DGEN_DIR=${PS2R_IOP_GEN}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DSTRICT=${_ps2r_strict}" "-DSTAMP=${PS2R_IOP_STAMP}"
        -P "${CMAKE_SOURCE_DIR}/cmake/ps2r_verify_manifest.cmake"
    DEPENDS ps2r_iop_recompiler ps2r_title_inputs "${PS2R_TITLE_IRX}" "${PS2R_TITLE_IRX_READY}"
            "${PS2R_IOP_MANIFEST}" "${CMAKE_SOURCE_DIR}/cmake/ps2r_run_tool.cmake"
            "${CMAKE_SOURCE_DIR}/cmake/ps2r_verify_manifest.cmake"
    COMMENT "Generating the recompiled STREAM.IRX into ${PS2R_IOP_GEN} (ps2r_iop_recompiler over ${PS2R_TITLE_IRX})"
    VERBATIM)
unset(_ps2r_strict)
add_custom_target(ps2r_title_stream_irx_gen DEPENDS ${PS2R_IOP_HEADER} ${PS2R_IOP_COMMON} ${PS2R_IOP_STAMP})
set_source_files_properties(${PS2R_IOP_HEADER} ${PS2R_IOP_COMMON} PROPERTIES GENERATED TRUE)

add_library(ps2r_title_stream_irx STATIC
    ${PS2R_IOP_COMMON}
    abi/stub_surface.cpp
    lti_protocol.cpp
    stream_irx_plugin.cpp
    stream_rpc_server.cpp)
add_dependencies(ps2r_title_stream_irx ps2r_title_stream_irx_gen)
target_include_directories(ps2r_title_stream_irx PUBLIC
    ${PS2R_IOP_GEN}                           # "iop_recompiled.h" (generated; the plugin and tests/iop include it)
    ${CMAKE_CURRENT_SOURCE_DIR}/abi/anchor   # "../host_app/x.h" -> abi/host_app/x.h (consumers of iop_recompiled.h too)
    ${CMAKE_SOURCE_DIR})
set_source_files_properties(${PS2R_IOP_COMMON} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing;-fwrapv;-O0")
set_source_files_properties(abi/stub_surface.cpp lti_protocol.cpp stream_irx_plugin.cpp stream_rpc_server.cpp
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_link_libraries(ps2r_title_stream_irx PUBLIC ps2r_iop ps2r_sif_iop)
message(STATUS "ps2r_title_stream_irx: generated at build time into ${PS2R_IOP_GEN} from ${PS2R_TITLE_IRX}")
