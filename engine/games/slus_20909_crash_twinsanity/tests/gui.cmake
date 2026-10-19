# games/slus_20909_crash_twinsanity/tests/gui.cmake -- this title's suites over the session
# product, included from app/gui/CMakeLists.txt (so they see PS2R_PRODUCT_ROOT / _TESTS / _GUI and
# the product support targets): the save/input HLE composition, the continuation proofs over the
# generated shards, the restore owners.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/gui/continuation_tests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/gui/input_save.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/gui/restore_tests.cmake)
