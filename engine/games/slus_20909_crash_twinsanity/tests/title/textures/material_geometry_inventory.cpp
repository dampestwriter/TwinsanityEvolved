// Static original ModelRead records -> actual replacement load-time decoder.
// This executable neither runs a game nor executes the old hardware model.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "archive.BD models.txt manifest_sha256 qualification.txt\n";
        return 2;
    }
    std::ifstream archive(argv[1], std::ios::binary), manifest(argv[2]);
    std::ofstream output(argv[4]);
    if (!archive || !manifest || !output || std::string(argv[3]).size() != 64) return 2;
    archive.seekg(0, std::ios::end);
    const auto extent = archive.tellg();
    if (extent < 0) return 2;
    output << argv[3] << '\n';
    size_t checked = 0, accepted = 0, parts = 0;
    std::string line;
    while (std::getline(manifest, line)) {
        std::istringstream row(line);
        std::string sha;
        uint32_t count = 0;
        if (!(row >> sha >> count) || sha.size() != 64 || count > 4096) return 2;
        // Source MeshAssets::ModelLoaded's complete-model envelope.
        bool valid = count && count <= 64;
        uint64_t vertices = 0;
        for (uint32_t i = 0; i != count; ++i) {
            uint64_t offset = 0;
            uint32_t size = 0, declared = 0;
            if (!(row >> offset >> size >> declared) || offset > uint64_t(extent) ||
                size > uint64_t(extent) - offset || size > 4 * 1024 * 1024) return 2;
            std::vector<uint8_t> bytes(size);
            archive.seekg(std::streamoff(offset));
            archive.read(reinterpret_cast<char*>(bytes.data()), size);
            if (!archive) return 2;
            title::MeshGeometry::Mesh mesh;
            if (title::MeshGeometry::Decode(bytes.data(), bytes.size(), declared, mesh) !=
                title::MeshGeometry::Result::Ok) valid = false;
            vertices += declared;
            ++parts;
        }
        std::string tail;
        if (row >> tail) return 2;
        valid = valid && vertices <= title::MeshGeometry::kMaxVertices;
        output << sha << ' ' << valid << '\n';
        ++checked;
        if (valid) ++accepted;
    }
    if (!output || !manifest.eof()) return 2;
    std::cout << checked << " complete models / " << parts << " parts; "
              << accepted << " accepted by MeshGeometry + complete-model limits\n";
    return 0;
}
