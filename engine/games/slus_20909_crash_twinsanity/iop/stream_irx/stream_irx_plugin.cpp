// games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.cpp
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"

#include <cstring>

#include "engine/iop/hle/idle/idle_module.h"
#include "engine/iop/iop_kernel.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/abi/stub_surface.h"
#include "iop_recompiled.h"   // generated (build dir; ps2r_title_stream_irx exports PS2R_IOP_GEN_DIR)
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"

namespace ps2r::title::stream_irx {

namespace {
struct ModuleRow {
    const char* name;
    uint32_t version;   // BCD major.minor as RegisterLibraryEntries reports it
};
// The post-reboot IOPRP300 set (runtime-environment: IOP kernel v2.2) + the disc IRXs.
constexpr ModuleRow kIoprpRows[] = {
    {"excepman", 0x101}, {"intrman", 0x102}, {"ssbusc", 0x101},  {"dmacman", 0x102}, {"timrman", 0x103},
    {"sysclib", 0x104},  {"stdio", 0x103},   {"heaplib", 0x101}, {"thbase", 0x102},  {"thevent", 0x101},
    {"thsemap", 0x101},  {"thmsgbx", 0x101}, {"thfpool", 0x101}, {"thvpool", 0x101}, {"vblank", 0x101},
    {"ioman", 0x104},    {"modload", 0x107}, {"romdrv", 0x201},  {"sifman", 0x101},  {"sifcmd", 0x101},
    {"cdvdman", 0x101},  {"cdvdfsv", 0x101},
};
constexpr ModuleRow kIdleDiscRows[] = {
    {"sio2man", 0x203}, {"padman", 0x204}, {"mcman", 0x207}, {"mcserv", 0x201}, {"sdrdrv", 0x101}, {"cdvdstm", 0x101},
};
constexpr uint32_t kLibsdVersion = 0x105;
constexpr uint32_t kStreamVersion = 0x100;   // STREAM.IRX registers no library; the IRX file stem row
}  // namespace

iop::IopLoader::GuestFunction StreamIrxPlugin::Resolve(uint32_t addr) {
    if (iop_recompiled_functions.empty()) initialize_iop_recompiled_functions();
    auto it = iop_recompiled_functions.find(addr);
    if (it == iop_recompiled_functions.end()) return {};
    return it->second;
}

StreamIrxPlugin::StreamIrxPlugin(iop::IopModuleTable& table, iop::IopMemory& mem, sound::SpuCore& spu,
                                 cdvd::CdvdReader* reader, iop::hle::EeRamSink* sink)
    : table_(table),
      mem_(mem),
      loader_(mem),
      sysmem_(mem),
      intrman_(mem),
      stdio_(mem),
      sysclib_(mem),
      ioman_(mem, reader),
      sifcmd_(table, mem),
      sifman_(mem, sink),
      thbase_(table, mem, &StreamIrxPlugin::Resolve, ModuleLayout::Threads()),
      thsemap_(table, mem),
      timrman_(table, &StreamIrxPlugin::Resolve, ModuleLayout::Timer()),
      libsd_(mem, spu),
      cdvdman_(mem, reader),
      protocol_(mem) {
    StubSurface::BindMemory(mem_);
    StubSurface::Modules m;
    m.sysmem = &sysmem_;
    m.intrman = &intrman_;
    m.stdio = &stdio_;
    m.sysclib = &sysclib_;
    m.ioman = &ioman_;
    m.sifcmd = &sifcmd_;
    m.sifman = &sifman_;
    m.thbase = &thbase_;
    m.thsemap = &thsemap_;
    m.timrman = &timrman_;
    StubSurface::BindModules(m);
    StubSurface::BindDeviceProvider([this](size_t row, iop::IopContext& ctx) { DeviceCall(row, ctx); });
    sifcmd_.SetCdReadRpc([this](iop::IopContext& ctx) { protocol_.CdReadRpc(ctx); });
    // sceCdRead completions: the registered cdvdman callback runs as a deferred
    // kernel event (dispatched on the IOP's own thread at its due time).
    cdvdman_.SetCompletion(&table_.Kernel(), [](uint32_t addr, iop::IopContext& ctx) {
        if (iop::IopLoader::GuestFunction fn = Resolve(addr)) fn(ctx);
    });
    // SdVoiceTrans completions: the handler STREAM.IRX registered through
    // SdSetTransIntrHandler (module-relative or absolute address) runs as a
    // deferred kernel event with the module's $gp, as a thread entry does.
    libsd_.SetCompletion(&table_.Kernel(), [](uint32_t addr, iop::IopContext& ctx) {
        iop::IopLoader::GuestFunction fn = Resolve(addr & 0xFFFF);
        if (!fn) fn = Resolve(addr);
        if (!fn) return;
        ctx.gpr[28] = ModuleLayout::kGp;
        fn(ctx);
    });
}

StreamIrxPlugin::~StreamIrxPlugin() { StubSurface::Unbind(); }

void StreamIrxPlugin::DeviceCall(size_t row, iop::IopContext& ctx) {
    const StubSurface::CensusRow& r = StubSurface::Census()[row];
    if (std::strncmp(r.name, "libsd_stub_", 11) == 0) {
        libsd_.Call(std::atoi(r.name + 11), ctx);
        return;
    }
    using Fn = void (iop::hle::CdvdmanModule::*)(iop::IopContext&);
    struct Entry { const char* name; Fn fn; };
    static const Entry kCdvd[] = {
        {"sceCdInit", &iop::hle::CdvdmanModule::sceCdInit},
        {"sceCdRead", &iop::hle::CdvdmanModule::sceCdRead},
        {"sceCdGetError", &iop::hle::CdvdmanModule::sceCdGetError},
        {"sceCdSearchFile", &iop::hle::CdvdmanModule::sceCdSearchFile},
        {"sceCdSync", &iop::hle::CdvdmanModule::sceCdSync},
        {"sceCdGetDiskType", &iop::hle::CdvdmanModule::sceCdGetDiskType},
        {"sceCdDiskReady", &iop::hle::CdvdmanModule::sceCdDiskReady},
        {"sceCdCallback", &iop::hle::CdvdmanModule::sceCdCallback},
        {"sceCdMmode", &iop::hle::CdvdmanModule::sceCdMmode},
    };
    for (const Entry& e : kCdvd) {
        if (std::strcmp(e.name, r.name) == 0) {
            (cdvdman_.*e.fn)(ctx);
            return;
        }
    }
    ctx.gpr[2] = 0;
}

void StreamIrxPlugin::RegisterModuleRows() {
    for (const ModuleRow& r : kIoprpRows) table_.RegisterModule(r.name, r.version, 0);
    table_.RegisterModule("libsd", kLibsdVersion, 0);
    for (const ModuleRow& r : kIdleDiscRows) iop::hle::IdleModule(r.name, r.version).Register(table_);
}

StreamIrxPlugin::BootReport StreamIrxPlugin::Boot(const uint8_t* irx, size_t size) {
    BootReport rep;
    rep.sections = loader_.LoadSections(irx, size);
    if (!rep.sections.ok) {
        rep.error = rep.sections.error;
        return rep;
    }
    RegisterModuleRows();
    rep.entry_ran = loader_.RunEntry(ModuleLayout::Loader(), &StreamIrxPlugin::Resolve, table_);
    if (!rep.entry_ran) {
        rep.error = "no recompiled function at the module entry";
        return rep;
    }
    rep.rpc_tid = loader_.LastRpcTid();
    table_.RegisterModule("STREAM", kStreamVersion, rep.rpc_tid);
    booted_ = true;
    return rep;
}

void StreamIrxPlugin::BootSymmetricRepark(const iop::IopmSection::Statics& s) {
    // The parked coroutines are the boot path's (the module is never relaunched on a
    // load); the restored statics re-park them: heap arena pointer, the transient
    // voice-transfer sequences, the CD callback, then the kernel's thread/sema flags.
    sysmem_.RestoreHeapPtr(s.heap_ptr);
    libsd_.ResetPendingSequence();
    cdvdman_.SetCallbackAddr(s.cd_callback_addr);
    table_.Kernel().DropPendingEvents();
    // The boot path only reaches the module's first block; the image's thread table
    // also holds the threads the saver's title created afterwards through the RPC
    // face (the streaming worker: CreateThread + StartThread on the stream-open
    // command, torn down on close). Those do not exist in this kernel. The rule: a
    // thread the image records as started is running -- re-created from its record
    // and armed from its entry, then dispatched to its first park (the worker's
    // WaitSema on the restored sema id, whose token count the kernel reconcile has
    // already set from the image) before the EE continues, as module init runs to
    // its first block at boot. A thread recorded terminated, or one already live
    // (the boot path's), is left to the kernel reconcile.
    int armed = 0;
    for (int tid = 1; tid < iop::IopmSection::kThreads; tid++) {
        const iop::IopmSection::ThreadRec& t = s.threads[tid];
        if (t.entry == 0 || t.terminated) continue;
        if (thbase_.RestoreThread(tid, t.entry, t.priority, t.arg, t.started && t.active)) armed++;
    }
    if (armed > 0) table_.Kernel().Dispatch();
    // The same rule for the module's heartbeat hard timer: the title's init RPC
    // allocates, sets up and starts it (the saver's kernel beat at the save; the
    // IOPM layout carries no timer record, the module's own descriptor words do:
    // a non-zero id word is a started timer, its compare word the period). A timer
    // the image records as running beats after the restore, its first due one
    // period after the restored clock; one the image records as freed is stopped.
    // Armed after the re-park Dispatch so that pass cannot phase-shift it.
    RestoreHeartbeat();
}

void StreamIrxPlugin::RestoreHeartbeat() {
    const uint32_t id = mem_.Read<uint32_t>(ModuleLayout::kTimerDescIdWord);
    const uint32_t compare = mem_.Read<uint32_t>(ModuleLayout::kTimerDescCompareWord);
    const bool live = id >= 1 && id <= static_cast<uint32_t>(iop::hle::TimrmanModule::kMaxTimers) && compare != 0;
    for (int t = 1; t <= iop::hle::TimrmanModule::kMaxTimers; t++) {
        if (live && static_cast<uint32_t>(t) == id) {
            timrman_.RestoreHardTimer(t, compare, ModuleLayout::kTimerHandler, ModuleLayout::kTimerDesc);
        } else {
            timrman_.ReleaseHardTimer(t);
        }
    }
}

iop::IopmSection::Deps StreamIrxPlugin::IopmDeps(std::function<uint64_t()> restored_vf) {
    iop::IopmSection::Deps d;
    d.ram = &mem_;
    d.statics = &statics_;
    d.kernel = &table_.Kernel();
    d.restored_vf = std::move(restored_vf);
    // The spec's restore has already parsed into statics_ and reconciled the kernel.
    d.after_restore = [this] {
        BootSymmetricRepark(statics_);
        table_.NotifyHost();
    };
    return d;
}

}  // namespace ps2r::title::stream_irx
