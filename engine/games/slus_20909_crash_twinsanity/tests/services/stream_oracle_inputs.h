#pragma once

#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/iop/iop_module_table.h"
#include "engine/sif/hle/ee_rpc_server.h"
#include "engine/sif/hle/main_memory_ee_ram_sink.h"
#include "engine/sif/hle/rpc_router.h"
#include "games/slus_20909_crash_twinsanity/tests/services/music_oracle_case.h"

// References to one live same-input fixture. Splitting the cases must not
// reboot the comparison arm or discard its client, file or sample ownership.
struct StreamOracleInputs {
    CpuContext& ctx;
    ps2r::iop::IopModuleTable& table;
    ps2r::iop::IopMemory& iop_mem;
    ps2r::sound::SpuCore& spu;
    ps2r::title::stream_irx::StreamIrxPlugin& plugin;
    MainMemoryEeRamSink& ee_sink;
    ps2r::title::stream_irx::StreamRpcServer& face;
    EeRpcServer& ee_server;
    RpcRouter& router;
    LoaderModel& model;
    DeliveryMeter& meter;
    const LoaderFileIo::Deps& deps;
    const LoaderFileIo::Layout& layout;
    uint64_t& tick;
    uint32_t& pump_calls;
    uint32_t mgr;
};
