// te_test_iso.h -- a tiny ISO 9660 volume written to a temp file for the disc/files/stream tests (no game data in the
// tree: the fixture is GENERATED, its contents a byte pattern). Layout: PVD at sector 16, terminator at
// 17, root directory at 18, CRASH6 directory at 19, the files from sector 20 on.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace te_test {

struct IsoFile {
    std::string dir;    // "" (root) or "CRASH6"
    std::string name;   // "CRASH.BH" (the ";1" is added)
    std::vector<uint8_t> bytes;
};

// Writes the image; returns its path ("" on failure). `files` may name "" or "CRASH6" as the directory.
std::string WriteTestIso(const std::vector<IsoFile>& files, const std::string& volume_id = "TWINSANITY_TEST");
// A deterministic byte pattern of `n` bytes seeded by `seed`.
std::vector<uint8_t> Pattern(size_t n, uint32_t seed);

}  // namespace te_test
