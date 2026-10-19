# engine/render/backends/null -- the backend that rasterises nothing (NullBackend): the contract
# honoured in shape and counted, no API, no window. Includes only engine/render/backend.h and
# the gs/hle/ir + texsrc types the contract names.
add_library(ps2r_render_null STATIC null_backend.cpp)
target_include_directories(ps2r_render_null PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(ps2r_render_null PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ps2r_render_null PUBLIC ps2r_render)
