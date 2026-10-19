# The parent already includes stream_library.cpp in ps2r_title_hle.
# Keep new sources here; do not register that source a second time.
target_sources(ps2r_title_hle PRIVATE stream_state.cpp stream_migration.cpp file_schedule.cpp)
target_sources(ps2r_title_hle PRIVATE stream_buffers.cpp)
target_sources(ps2r_title_hle PRIVATE stream_buffers_state.cpp buffer_state.cpp)
target_sources(ps2r_title_hle PRIVATE files.cpp files_state.cpp)
target_sources(ps2r_title_hle PRIVATE stream_transport.cpp)
