#include "games/slus_20909_crash_twinsanity/tests/services/files_fixture.h"

#include <cstring>
#include <utility>

FilesFixture::FilesFixture(const std::string& disc, bool selected)
    : ram(0x10000, 0xcd), memory([this](uint32_t at, size_t size) -> uint8_t* {
          return at <= ram.size() && size <= ram.size() - at ? ram.data() + at : nullptr;
      }), model(iso), files(memory, iso) {
    if (!iso.Open(disc)) return;
    using F = title::Files;
    // The control arm is NEW's actual mounted-disc file model, not expected
    // literals or the SDK park. Negative error codes are compared by failure.
    for (uint32_t entry : {F::kReset, F::kOpen, F::kClose, F::kSeek, F::kRead, F::kWrite}) {
        dispatch[entry] = [this, entry](CpuContext& c, uint32_t) {
            ++model_calls;
            const auto a = c.cpuRegs.GPR.r[4].UL[0], b = c.cpuRegs.GPR.r[5].UL[0],
                count = c.cpuRegs.GPR.r[6].UL[0];
            int32_t result = -1;
            if (entry == F::kOpen)
                result = model.Open(reinterpret_cast<const char*>(ram.data() + a), b);
            else if (entry == F::kClose) result = model.Close(static_cast<int32_t>(a));
            else if (entry == F::kSeek)
                result = model.Seek(static_cast<int32_t>(a), static_cast<int32_t>(b), count);
            else if (entry == F::kRead)
                result = model.Read(static_cast<int32_t>(a), memory.Translate(b, count), count);
            else if (entry == F::kReset) {
                for (int fd = 1; fd <= ReadEngine::kMaxFiles; ++fd) model.Close(fd);
                result = 0;
            }
            c.cpuRegs.GPR.r[2].SD[0] = result;
        };
    }
    ready = files.Install([this](uint32_t entry) { return dispatch.at(entry); },
        [this](uint32_t entry, title::Files::Body body) {
            dispatch[entry] = std::move(body); return true;
        }, selected);
}

void FilesFixture::Text(const std::string& text) {
    std::memcpy(ram.data() + kPath, text.c_str(), text.size() + 1);
}

int32_t FilesFixture::Call(uint32_t entry, std::initializer_list<uint32_t> args) {
    CpuContext context{};
    unsigned index = 4;
    for (auto arg : args) context.cpuRegs.GPR.r[index++].UL[0] = arg;
    dispatch.at(entry)(context, 0);
    return context.cpuRegs.GPR.r[2].SL[0];
}
