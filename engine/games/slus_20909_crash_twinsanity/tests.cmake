# games/slus_20909_crash_twinsanity/tests.cmake -- this title's test suites, included by the root
# CMakeLists after tests/ (so every engine, tool and test-support target exists). Everything here
# is title-bound: it exercises THIS title's HLE, its recompiled output, its composition and its
# frozen baselines (private fixtures: cmake/Ps2rPrivateFixtures.cmake, SKIP 77 without them).
#
# The suites live HERE, under games/<title>/tests/ (tests/CMakeLists.txt l.196: a title module
# brings its own suites): tests/{recomp,services,title} whole, and the title-bound rows of
# tests/{app,cdvd,ee,gs,iop,ipu,render,vu}. The engine's own tests/<dir>/ hold only title-neutral
# rows; the shared helpers they keep (tests/lib, tests/ipu/vector_guest_memory.cpp, ...) are
# referenced from here by their engine path.
include_guard(GLOBAL)

set(PS2R_TITLE_SRC "${CMAKE_SOURCE_DIR}/games/${PS2R_TITLE}")
set(PS2R_TITLE_TESTS "${CMAKE_CURRENT_LIST_DIR}/tests")

# The disc image / game files the ISO-backed rows read (the original tree's layout; absent =
# the rows run their no-disc arm or SKIP).
set(PS2R_DISC_IMAGE "${PS2R_OG_ROOT}/game_files/Crash_TwinSanity.rep/Crash Twinsanity.iso")
set(PS2R_OG_GAME_FILES "${PS2R_OG_ROOT}/game_files")
set(PS2R_TITLE_HLE_DIR "${PS2R_TITLE_SRC}/hle")

# --- whole-directory suites (their own CMakeLists) ------------------------------------------
# tests/recomp: the recompiled output's oracles (math library, movie, graphics runtime, provenance).
add_subdirectory("${PS2R_TITLE_TESTS}/recomp" "${CMAKE_BINARY_DIR}/tests/recomp")
# tests/services: the audio / stream / file service oracles over the composition's services.
add_subdirectory("${PS2R_TITLE_TESTS}/services" "${CMAKE_BINARY_DIR}/tests/services")

# --- tests/title: the title HLE suites ------------------------------------------------------
include(${PS2R_TITLE_TESTS}/title/input_saves.cmake)
include(${PS2R_TITLE_TESTS}/title/menu/menu.cmake)
include(${PS2R_TITLE_TESTS}/title/fixes/fixes.cmake)
include(${PS2R_TITLE_TESTS}/title/qol/qol.cmake)
include(${PS2R_TITLE_TESTS}/title/textures/tests.cmake)
include(${PS2R_TITLE_TESTS}/title/primitives/tests.cmake)
include(${PS2R_TITLE_TESTS}/title/text/tests.cmake)
include(${PS2R_TITLE_TESTS}/title/effects/tests.cmake)

# --- the title-bound rows of the engine test directories ------------------------------------
include(${PS2R_TITLE_TESTS}/cdvd.cmake)
include(${PS2R_TITLE_TESTS}/ee.cmake)
include(${PS2R_TITLE_TESTS}/gs.cmake)
include(${PS2R_TITLE_TESTS}/iop.cmake)
include(${PS2R_TITLE_TESTS}/ipu.cmake)
include(${PS2R_TITLE_TESTS}/render.cmake)
include(${PS2R_TITLE_TESTS}/vu.cmake)
include(${PS2R_TITLE_TESTS}/app.cmake)
