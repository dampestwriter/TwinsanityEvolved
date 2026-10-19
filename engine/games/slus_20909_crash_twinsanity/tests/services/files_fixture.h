#pragma once

#include <map>

#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/ipu/hle/guest_memory_adapter.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"

class FilesFixture {
public:
    static constexpr uint32_t kPath = 0x100, kOutput = 0x1000;
    FilesFixture(const std::string& disc, bool selected);
    void Text(const std::string& text);
    int32_t Call(uint32_t entry, std::initializer_list<uint32_t> args = {});
    std::vector<uint8_t> ram;
    ps2r::ipu::GuestMemoryAdapter memory;
    IsoFs iso;
    ReadEngine model;
    title::Files files;
    std::map<uint32_t, title::Files::Body> dispatch;
    uint64_t model_calls = 0;
    bool ready = false;
};
