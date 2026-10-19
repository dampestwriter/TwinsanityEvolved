#include <cstdio>
#include <unistd.h>

#include "games/slus_20909_crash_twinsanity/composition/file_services.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "tests/cdvd/stream_library_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/services/files_fixture.h"

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)
int32_t Call(uint32_t entry, std::initializer_list<uint32_t> arguments) {
    CpuContext context{};
    unsigned reg = 4;
    for (uint32_t arg : arguments) context.cpuRegs.GPR.r[reg++].UL[0] = arg;
    DispatchTable::Invoke(entry, context);
    return context.cpuRegs.GPR.r[2].SL[0];
}
}

int main() {
    using Files = title::Files;
    using App = ps2r::app::FileServices;
    using Rig = FilesFixture;
    const auto path = stream_fixture::Disc();
    Rig rig(path, true);
    CHECK(rig.ready);
    // Real production dispatch, app adapter and SectionRegistry. Only the
    // verified synchronous SDK boundaries are invoked; no game runs here.
    DispatchTable::Initialize();
    App off(rig.memory, rig.iso, false);
    App selected(rig.memory, rig.iso, true);
    CHECK(selected.Install());
    rig.Text("cdrom0:\\DATA.BD;1");
    const auto handle = uint32_t(Call(Files::kOpen, {Rig::kPath, 1}));
    CHECK(int32_t(handle) >= 0);
    CHECK(Call(Files::kRead, {handle, Rig::kOutput, 1031}) == 1031);
    ps2r::savestate::SectionRegistry registry;
    CHECK(registry.Register(selected.Section()));
    const auto snapshot = registry.Capture();
    CHECK(snapshot.size() == 1 && snapshot[0].tag == App::kTag);
    std::string error;
    CHECK(off.CanCheckpoint(&error) && selected.CanCheckpoint(&error));
    CHECK(selected.ValidateImage(snapshot, &error) && registry.Validate(snapshot, &error));
    CHECK(!selected.ValidateImage({}, &error) && !registry.Validate({}, &error));
    CHECK(off.ValidateImage({}, &error) && !off.ValidateImage(snapshot, &error));
    auto malformed = snapshot;
    malformed.push_back(snapshot.front());
    CHECK(!selected.ValidateImage(malformed, &error));
    malformed = snapshot; malformed[0].flags = 0;
    CHECK(!selected.ValidateImage(malformed, &error) && !registry.Validate(malformed, &error));
    malformed = snapshot; ++malformed[0].version;
    CHECK(!selected.ValidateImage(malformed, &error) && !registry.Validate(malformed, &error));
    malformed = snapshot; malformed[0].payload.push_back(0);
    CHECK(!selected.ValidateImage(malformed, &error) && !registry.Restore(malformed, &error));
    CHECK(registry.Capture()[0].payload == snapshot[0].payload);
    CHECK(Call(Files::kClose, {handle}) == 0);
    CHECK(Call(Files::kOpen, {Rig::kPath, 1}) == int32_t(handle));
    CHECK(Call(Files::kRead, {handle, Rig::kOutput, 7}) == 7);
    CHECK(registry.Restore(snapshot, &error));
    CHECK(Call(Files::kSeek, {handle, 0, 1}) == 1031);
    CHECK(Call(Files::kRead, {handle, Rig::kOutput, 4097}) == 4097);
    for (uint32_t i = 0; i < 4097; ++i)
        CHECK(rig.ram[Rig::kOutput + i] == stream_fixture::Byte(i + 1031));
    CHECK(Call(Files::kReset, {}) == 0);
    CHECK(Call(Files::kClose, {handle}) == -9);
    unlink(path.c_str());
    std::printf("FileServices: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
