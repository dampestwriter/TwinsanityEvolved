#include "games/slus_20909_crash_twinsanity/tests/app/restore_owner_fixture.h"

#include <cstring>
#include <stdexcept>

#include "engine/ee/mem/main_memory.h"

RestoreOwnerFixture::RestoreOwnerFixture(MainMemory& ram, EmuDomain& domain, TickSource& source,
                                         const std::string& directory)
    : ram_(ram), domain_(domain), source_(source),
      owners_(ps2r::app::InputSave::Checkpoint::kAll, ps2r::app::InputSave::RamAccess(ram, spad_),
        {directory, 0x1f00, {{true, true, true, true}}, [this](::title::SaveStorage::WritePoint point) {
            if (point == ::title::SaveStorage::WritePoint::BeforeCommit) ++commits_;
            return 0;
        }}, [&] { return domain_.DomainTick(); }, {}),
      file_(directory + "/mc0/BASLUS-20909/Bank0.bin") {
    if (!owners_.Install([&](uint32_t address, auto body) {
        return entries_.emplace(address, std::move(body)).second;
    })) throw std::runtime_error("Failed to install selected fixture owners.");
}
bool RestoreOwnerFixture::Register(ps2r::savestate::SectionRegistry& registry, ps2r::app::SessionClock& clock) {
    return owners_.Register(registry, &clock);
}
int64_t RestoreOwnerFixture::Call(uint32_t address, std::initializer_list<uint32_t> args) {
    CpuContext context{};
    unsigned reg = 4;
    for (const auto arg : args) context.cpuRegs.GPR.r[reg++].UL[0] = arg;
    entries_.at(address)(context, 0);
    return context.cpuRegs.GPR.r[2].SD[0];
}
bool RestoreOwnerFixture::Finish() {
    // Startup has logical ticks before the first completed gameplay boundary.
    for (unsigned i = 0; i < 16; ++i) {
        domain_.OnRawTick(source_.Tick());
        if (!Call(0x2a6b68, {0, 0, 0x4200})) return true;
    }
    return false;
}
bool RestoreOwnerFixture::Start() {
    std::memcpy(ram_.Data() + 0x4000, "BA", 3);
    std::memcpy(ram_.Data() + 0x4020, "SLUS-20909", 11);
    std::memcpy(ram_.Data() + 0x4060, "Bank0.bin", 10);
    ram_.Data()[0x4100] = 17;
    return Call(0x2c5080, {}) == 0 && Call(0x2a8668, {0x4000, 0x4020}) == 1 &&
        Call(0x2a8728, {0, 0, 0x4040}) == 1 && Finish() &&
        Call(0x2a8c28, {0, 0, 0x4040, 0x4060, 0x4100, 1}) == 1 && Finish() &&
        Call(0x2c27c0, {0, 0, 0x1000}) == 1;
}
void RestoreOwnerFixture::QueueSetup() { setup_.store(true); }
void RestoreOwnerFixture::QueueWrite() {
    write_done_.store(false);
    write_.store(true);
}
void RestoreOwnerFixture::Step() {
    ::title::Input::Sample sample;
    sample.connected = true; sample.buttons = 0x8001;
    if (!owners_.Sample(domain_.DomainTick(), sample))
        throw std::runtime_error("Selected input rejected the fixture clock.");
    if (setup_.exchange(false) && Call(0x2c3020, {0, 0, 0, 3}) != 1)
        throw std::runtime_error("Fixture mode request was not accepted.");
    if (write_.exchange(false)) {
        ram_.Data()[0x4100] = 71;
        if (Call(0x2a8c28, {0, 0, 0x4040, 0x4060, 0x4100, 1}) != 1)
            throw std::runtime_error("Fixture game-save request was not accepted.");
        write_pending_ = true;
    }
    if (Call(0x2a6b68, {0, 0, 0x4200}) == 0 && write_pending_) {
        write_pending_ = false;
        write_done_.store(true);
    }
}
bool RestoreOwnerFixture::CanCheckpoint(std::string& error) const {
    return owners_.CanCheckpoint(&error);
}
bool RestoreOwnerFixture::Restored() const {
    return !write_pending_ && owners_.Current().connection == ::title::Input::State::Busy &&
        owners_.Current().sample.buttons == 0x8001 && !owners_.NeedsSample(domain_.DomainTick()) &&
        ram_.Data()[0x4100] == 17; // logical RAM rewinds; durable file stays at 71
}
