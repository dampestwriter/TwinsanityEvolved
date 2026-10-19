# ps2r_title_recomp -- the recompiler's output for this title, GENERATED AT
# BUILD TIME from the user's ELF into the build directory (cmake/TitleInputs.cmake). Nothing here
# is edited by hand; nothing generated is committed. The committed files are the provenance and
# the inputs: MANIFEST.sha256 (what the output must hash to), inputs/ (the two stub lists the tool reads from its cwd) and continuations.h (the
# continuations.py sidecar; the build regenerates its twin and checks it against the manifest too).
#
# Generation = exactly tools/regen_from_frozen.sh's one run:
#   cwd = <gen>/cwd holding copies of inputs/*.txt (the tool finds the stub files in cwd first,
#   recompiler.cpp open_stub_file; it also writes logs/recompiler_tool_v2/runtime_log.txt there)
#   ps2r_recompiler_v2 --input <ELF> --output-header <gen>/recompiled.h --output-cpp <gen>/recompiled.cpp
#                      --shards N --init-parts P --direct-calls
#   python3 tools/recompiler_v2/continuations.py <gen>      (continuations.h + the common TU's instrumentation)
#   cmake -P cmake/ps2r_verify_manifest.cmake                (every output == MANIFEST.sha256, else the build stops)
# N (the shard split) is the number of shards MANIFEST.sha256 lists; P (the init-table parts) is
# PS2R_RECOMP_INIT_PARTS below -- both are what the installed manifest was generated with, so the
# build reproduces the recorded layout byte for byte.
#
# Shards: -O2 -fno-strict-aliasing -fwrapv (semantic flags: signed overflow + GprReg union
# punning), common TU: -O0 -- exactly OLD's per-TU split (OLD/CMakeLists.txt:243/324/407).
if(NOT PS2R_TITLE_RECOMP_AVAILABLE)
    message(STATUS "ps2r_title_recomp: SKIPPED -- no title ELF (see TitleInputs above); the title and its tests are not built")
    return()
endif()
if(NOT TARGET ps2r_recompiler_v2)
    message(FATAL_ERROR "ps2r_title_recomp: the recompiler target ps2r_recompiler_v2 is required (PS2R_BUILD_RECOMPILER=ON)")
endif()

set(PS2R_RECOMP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/MANIFEST.sha256")
set(PS2R_RECOMP_GEN "${PS2R_RECOMP_GEN_DIR}")
file(MAKE_DIRECTORY "${PS2R_RECOMP_GEN}")

# The layout the manifest records.
file(STRINGS "${PS2R_RECOMP_MANIFEST}" _ps2r_manifest_shards REGEX "recompiled_shard_[0-9]+\\.cpp$")
list(LENGTH _ps2r_manifest_shards PS2R_RECOMP_SHARD_COUNT)
unset(_ps2r_manifest_shards)
if(PS2R_RECOMP_SHARD_COUNT EQUAL 0)
    message(FATAL_ERROR "ps2r_title_recomp: ${PS2R_RECOMP_MANIFEST} lists no shards")
endif()
# The init table is split into this many part functions (the tool's --init-parts; 1 = one body).
set(PS2R_RECOMP_INIT_PARTS 32)

set(PS2R_RECOMP_SHARDS "")
math(EXPR _last "${PS2R_RECOMP_SHARD_COUNT} - 1")
foreach(_i RANGE 0 ${_last})
    string(REGEX REPLACE "^(.)$" "00\\1" _n "${_i}")
    string(REGEX REPLACE "^(..)$" "0\\1" _n "${_n}")
    list(APPEND PS2R_RECOMP_SHARDS "${PS2R_RECOMP_GEN}/recompiled_shard_${_n}.cpp")
endforeach()
unset(_last)
set(PS2R_RECOMP_HEADER "${PS2R_RECOMP_GEN}/recompiled.h")
set(PS2R_RECOMP_COMMON "${PS2R_RECOMP_GEN}/recompiled.cpp")
set(PS2R_RECOMP_CONTINUATIONS "${PS2R_RECOMP_GEN}/continuations.h")
set(PS2R_RECOMP_STAMP "${PS2R_RECOMP_GEN}/MANIFEST.verified")
set(PS2R_RECOMP_OUTPUTS ${PS2R_RECOMP_HEADER} ${PS2R_RECOMP_COMMON} ${PS2R_RECOMP_CONTINUATIONS} ${PS2R_RECOMP_SHARDS})

set(_ps2r_rv2 "${CMAKE_SOURCE_DIR}/tools/recompiler_v2")
set(_ps2r_inputs
    "${CMAKE_CURRENT_SOURCE_DIR}/inputs/tiny_stubs.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/inputs/tiny_returners.txt")
if(PS2R_RECOMP_CHECK_MANIFEST)
    set(_ps2r_strict ON)
else()
    set(_ps2r_strict OFF)
endif()
add_custom_command(
    OUTPUT ${PS2R_RECOMP_OUTPUTS} ${PS2R_RECOMP_STAMP}
    # A clean slate: a previous run's shards must not survive a layout change.
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PS2R_RECOMP_GEN}/cwd"
    COMMAND ${CMAKE_COMMAND} -E rm -f ${PS2R_RECOMP_OUTPUTS} ${PS2R_RECOMP_STAMP}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PS2R_RECOMP_GEN}/cwd"
    COMMAND ${CMAKE_COMMAND} -E copy ${_ps2r_inputs} "${PS2R_RECOMP_GEN}/cwd"
    COMMAND ${CMAKE_COMMAND}
        "-DLABEL=ps2r_title_recomp: recompiling ${PS2R_TITLE_ELF_DISC_PATH} (${PS2R_RECOMP_SHARD_COUNT} shards)"
        "-DLOG=${PS2R_RECOMP_GEN}/recomp_gen.log" "-DWORKDIR=${PS2R_RECOMP_GEN}/cwd"
        "-DCMD=$<TARGET_FILE:ps2r_recompiler_v2>;--input;${PS2R_TITLE_ELF};--output-header;${PS2R_RECOMP_HEADER};--output-cpp;${PS2R_RECOMP_COMMON};--shards;${PS2R_RECOMP_SHARD_COUNT};--init-parts;${PS2R_RECOMP_INIT_PARTS};--direct-calls"
        -P "${CMAKE_SOURCE_DIR}/cmake/ps2r_run_tool.cmake"
    COMMAND ${CMAKE_COMMAND}
        "-DLABEL=ps2r_title_recomp: continuations.py"
        "-DLOG=${PS2R_RECOMP_GEN}/continuations.log"
        "-DCMD=${Python3_EXECUTABLE};${_ps2r_rv2}/continuations.py;${PS2R_RECOMP_GEN}"
        -P "${CMAKE_SOURCE_DIR}/cmake/ps2r_run_tool.cmake"
    COMMAND ${CMAKE_COMMAND}
        "-DLABEL=ps2r_title_recomp" "-DMANIFEST=${PS2R_RECOMP_MANIFEST}" "-DGEN_DIR=${PS2R_RECOMP_GEN}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DSTRICT=${_ps2r_strict}" "-DSTAMP=${PS2R_RECOMP_STAMP}"
        -P "${CMAKE_SOURCE_DIR}/cmake/ps2r_verify_manifest.cmake"
    DEPENDS ps2r_recompiler_v2 ps2r_title_inputs "${PS2R_TITLE_ELF}" "${PS2R_TITLE_ELF_READY}" ${_ps2r_inputs}
            "${_ps2r_rv2}/continuations.py" "${_ps2r_rv2}/continuation_audits.json"
            "${PS2R_RECOMP_MANIFEST}" "${CMAKE_SOURCE_DIR}/cmake/ps2r_run_tool.cmake"
            "${CMAKE_SOURCE_DIR}/cmake/ps2r_verify_manifest.cmake"
    COMMENT "Generating the title's recompiled EE code into ${PS2R_RECOMP_GEN} (ps2r_recompiler_v2 over ${PS2R_TITLE_ELF})"
    VERBATIM)
unset(_ps2r_inputs)
unset(_ps2r_strict)
unset(_ps2r_rv2)
# ps2r_title_recomp_gen: the generation step as a target -- the title tests that read the
# generated TUs (the oracle generators under tests/) depend on it.
add_custom_target(ps2r_title_recomp_gen DEPENDS ${PS2R_RECOMP_OUTPUTS} ${PS2R_RECOMP_STAMP})
# The shard list for the tests that read every shard (their DEPENDS; a configure-time GLOB would
# see nothing before the first build): root scope, so the tests/ tree added afterwards sees it.
set(PS2R_RECOMP_SHARD_FILES "${PS2R_RECOMP_SHARDS}" PARENT_SCOPE)
set_source_files_properties(${PS2R_RECOMP_OUTPUTS} PROPERTIES GENERATED TRUE)

# Compiler-front-end mapping of the semantic flags. GNU front ends (clang, gcc, clang-cl
# via /clang:) take them as written; the MSVC front end has no -fwrapv at all, which is
# why the Windows presets are clang-cl first.
#   -w                     generated TUs are not our code: no warnings
#   -fno-strict-aliasing   GprReg union punning
#   -fwrapv                EE signed arithmetic wraps
#   -ffp-contract=off      the EE has no fused multiply-add: every recompiled float
#                          expression must round after each operation, on every host, or
#                          the two trees (and two hosts) disagree in the last ULP of the
#                          title's own float tables (P4-FPCONTRACT)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # clang-cl: the clang semantics through the /clang: pass-through; /bigobj for the
    # >65k-section shard objects.
    set(PS2R_RECOMP_SEMANTIC_FLAGS /w /clang:-fno-strict-aliasing /clang:-fwrapv /clang:-ffp-contract=off /bigobj)
    set(PS2R_RECOMP_SHARD_OPT /O2)
    set(PS2R_RECOMP_COMMON_OPT /Od)
elseif(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # cl.exe: /fp:precise does not contract (no FMA) and MSVC does not exploit strict
    # aliasing, but there is NO -fwrapv: signed-overflow wrap in the recompiled EE
    # arithmetic is relied upon, not guaranteed. Builds, but is not a release compiler.
    message(WARNING "ps2r_title_recomp: MSVC has no -fwrapv equivalent; the recompiled TUs' signed-overflow semantics are not guaranteed (use the clang-cl presets for a release build)")
    set(PS2R_RECOMP_SEMANTIC_FLAGS /w /fp:precise /bigobj)
    set(PS2R_RECOMP_SHARD_OPT /O2)
    set(PS2R_RECOMP_COMMON_OPT /Od)
else()
    set(PS2R_RECOMP_SEMANTIC_FLAGS -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
    set(PS2R_RECOMP_SHARD_OPT -O2)
    set(PS2R_RECOMP_COMMON_OPT -O0)
endif()

# The body shards: one OBJECT library so the shard-only options (-O2, the precompiled
# prologue) are target-wide there and touch neither the -O0 common TU nor the glue TUs.
# The precompiled header is the shards' own include block (engine/ee/recomp_abi/anchor/
# recomp_pch.h, after the generated header); it changes nothing a shard sees -- the
# object code is byte-identical with and without it (RecompPchObjectIdentity) -- and
# removes the per-TU re-parse of a 0.5 MB generated header plus the ABI forwarders.
# PS2R_RECOMP_PCH=OFF builds the shards without it (the identity pin uses both).
option(PS2R_RECOMP_PCH "Precompile the generated shard TUs' include prologue" ON)
add_library(ps2r_title_recomp_shards OBJECT ${PS2R_RECOMP_SHARDS})
add_dependencies(ps2r_title_recomp_shards ps2r_title_recomp_gen)
target_include_directories(ps2r_title_recomp_shards PRIVATE
    ${PS2R_RECOMP_GEN}                                  # "recompiled.h" (the generated header, beside the shards)
    ${CMAKE_SOURCE_DIR}/engine/ee/recomp_abi/anchor   # "../host_app/x.h" -> engine/ee/recomp_abi/host_app/x.h
    ${CMAKE_SOURCE_DIR})
target_compile_options(ps2r_title_recomp_shards PRIVATE ${PS2R_RECOMP_SEMANTIC_FLAGS} ${PS2R_RECOMP_SHARD_OPT})
target_link_libraries(ps2r_title_recomp_shards PUBLIC ps2r_ee ps2r_title_hle)   # the forwarders' include paths
if(PS2R_RECOMP_PCH)
    target_precompile_headers(ps2r_title_recomp_shards PRIVATE
        ${PS2R_RECOMP_HEADER}
        ${CMAKE_SOURCE_DIR}/engine/ee/recomp_abi/anchor/recomp_pch.h)
endif()

# The glue TUs (ours) include the generated header as "recompiled.h": the generated
# directory is on this target's include path.
add_library(ps2r_title_recomp STATIC
    ${PS2R_RECOMP_COMMON}
    $<TARGET_OBJECTS:ps2r_title_recomp_shards>
    ../hle/glue/sce_graph_glue.cpp
    ../hle/glue/sce_dma_glue.cpp
    ../hle/glue/crt0_glue.cpp
    ../hle/glue/libc_heap_glue.cpp
    ../hle/glue/loader_glue.cpp
    ../hle/glue/headroom_query_glue.cpp
    ../hle/workarounds/setup_display_registers.cpp)
add_dependencies(ps2r_title_recomp ps2r_title_recomp_gen)
target_include_directories(ps2r_title_recomp PRIVATE
    ${PS2R_RECOMP_GEN}
    ${CMAKE_SOURCE_DIR}/engine/ee/recomp_abi/anchor   # "../host_app/x.h" -> engine/ee/recomp_abi/host_app/x.h
    ${CMAKE_SOURCE_DIR})
# Generated TUs are not our code: no warnings. The glue TUs are ours but include the
# generated header, which is not warning-clean, so they share the flag.
target_compile_options(ps2r_title_recomp PRIVATE ${PS2R_RECOMP_SEMANTIC_FLAGS})
set_source_files_properties(${PS2R_RECOMP_COMMON} PROPERTIES COMPILE_OPTIONS "${PS2R_RECOMP_COMMON_OPT}")
target_link_libraries(ps2r_title_recomp PUBLIC ps2r_ee ps2r_title_hle)   # loader_glue.cpp -> LoaderFileIo
message(STATUS "ps2r_title_recomp: generated at build time into ${PS2R_RECOMP_GEN} (${PS2R_RECOMP_SHARD_COUNT} shards, ${PS2R_RECOMP_INIT_PARTS} init parts) from ${PS2R_TITLE_ELF}")
