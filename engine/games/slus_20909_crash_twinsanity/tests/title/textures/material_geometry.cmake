# Optional inventory tool. Include after the Mesh worker's module has added
# MeshGeometry to ps2r_title_hle. This tool has no game/PCSX2 dependency.
include_guard(GLOBAL)
add_executable(material_geometry_inventory
    "${CMAKE_CURRENT_LIST_DIR}/material_geometry_inventory.cpp")
target_link_libraries(material_geometry_inventory PRIVATE ps2r_title_hle)
target_compile_options(material_geometry_inventory PRIVATE -Wall -Wextra -Werror)
