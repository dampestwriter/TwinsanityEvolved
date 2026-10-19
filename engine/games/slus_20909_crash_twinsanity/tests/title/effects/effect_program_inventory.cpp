// Static executable analysis only. This decoder is not linked by the title.
#include "engine/vu/vu_decoder.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 2;
    const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty() || bytes.size() > 8192 || bytes.size() % 8) return 2;
    for (size_t offset = 0; offset < bytes.size(); offset += 8) {
        uint32_t lower, upper;
        std::memcpy(&lower, bytes.data() + offset, 4);
        std::memcpy(&upper, bytes.data() + offset + 4, 4);
        const auto slot = VuDecoder::DecodeVuSlot(lower, upper);
        std::printf("%zu %08x %08x %u %u %u %u %u %u %u %u %u %u %u %u %d %u %u %u\n",
            offset / 8, upper, lower, slot.upper_kind, slot.lower_kind,
            slot.u_fd, slot.u_fs, slot.u_ft, slot.u_dest, slot.u_bc,
            slot.l_fd, slot.l_fs, slot.l_ft, slot.l_dest, slot.l_fsf,
            slot.l_imm11, slot.l_imm15u, slot.i_bit, slot.e_bit);
    }
    return 0;
}
