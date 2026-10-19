#include "te_test_iso.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace te_test {

namespace {
constexpr uint32_t kSector = 2048;

void Both32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
    p[4] = uint8_t(v >> 24); p[5] = uint8_t(v >> 16); p[6] = uint8_t(v >> 8); p[7] = uint8_t(v);
}
void Both16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// One directory record appended to `dir`
void Record(std::vector<uint8_t>& dir, uint32_t lba, uint32_t size, bool directory, const std::string& id) {
    std::vector<uint8_t> r(33 + id.size() + ((33 + id.size()) & 1), 0);
    r[0] = uint8_t(r.size());
    Both32(&r[2], lba);
    Both32(&r[10], size);
    r[18] = 105; r[19] = 6; r[20] = 15; r[21] = 12;   // 2005-06-15 12:00
    r[25] = directory ? 0x02 : 0x00;
    Both16(&r[28], 1);
    r[32] = uint8_t(id.size());
    std::memcpy(&r[33], id.data(), id.size());
    dir.insert(dir.end(), r.begin(), r.end());
}

uint32_t Sectors(size_t bytes) { return uint32_t((bytes + kSector - 1) / kSector); }
}  // namespace

std::vector<uint8_t> Pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) { x = x * 1103515245u + 12345u; v[i] = uint8_t(x >> 16); }
    return v;
}

std::string WriteTestIso(const std::vector<IsoFile>& files, const std::string& volume_id) {
    // Allocate file extents from sector 20
    uint32_t next = 20;
    std::vector<uint32_t> lbas;
    for (const IsoFile& f : files) { lbas.push_back(next); next += Sectors(f.bytes.size()) ? Sectors(f.bytes.size()) : 1; }
    const uint32_t total = next;
    std::vector<uint8_t> image(size_t(total) * kSector, 0);

    // Directories: root (18) and CRASH6 (19)
    std::vector<uint8_t> root, sub;
    Record(root, 18, kSector, true, std::string(1, '\0'));
    Record(root, 18, kSector, true, std::string(1, '\x01'));
    Record(root, 19, kSector, true, "CRASH6");
    Record(sub, 19, kSector, true, std::string(1, '\0'));
    Record(sub, 18, kSector, true, std::string(1, '\x01'));
    for (size_t i = 0; i < files.size(); ++i) {
        std::vector<uint8_t>& dir = files[i].dir == "CRASH6" ? sub : root;
        Record(dir, lbas[i], uint32_t(files[i].bytes.size()), false, files[i].name + ";1");
        std::memcpy(&image[size_t(lbas[i]) * kSector], files[i].bytes.data(), files[i].bytes.size());
    }
    if (root.size() > kSector || sub.size() > kSector) return "";
    std::memcpy(&image[18 * kSector], root.data(), root.size());
    std::memcpy(&image[19 * kSector], sub.data(), sub.size());

    // PVD
    uint8_t* pvd = &image[16 * kSector];
    pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    std::memset(pvd + 8, ' ', 32); std::memcpy(pvd + 8, "PLAYSTATION", 11);
    std::memset(pvd + 40, ' ', 32); std::memcpy(pvd + 40, volume_id.data(), std::min<size_t>(32, volume_id.size()));
    Both32(pvd + 80, total);
    Both16(pvd + 120, 1); Both16(pvd + 124, 1); Both16(pvd + 128, kSector);
    std::vector<uint8_t> rootrec;
    Record(rootrec, 18, kSector, true, std::string(1, '\0'));
    std::memcpy(pvd + 156, rootrec.data(), rootrec.size());
    // Terminator
    uint8_t* term = &image[17 * kSector];
    term[0] = 255; std::memcpy(term + 1, "CD001", 5); term[6] = 1;

    // A fresh name under the host's temp directory (std::filesystem on every lane: no mkstemps / "/tmp" on Windows); the
    // pid + a counter keep two test processes and two fixtures in one process apart.
    static int counter = 0;
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif
    const std::string path = (std::filesystem::temp_directory_path() /
                              ("te_test_" + std::to_string(pid) + "_" + std::to_string(++counter) + ".iso")).string();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return "";
    const bool ok = std::fwrite(image.data(), 1, image.size(), f) == image.size();
    std::fclose(f);
    return ok ? path : "";
}

}  // namespace te_test
