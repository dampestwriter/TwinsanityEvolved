#include "games/slus_20909_crash_twinsanity/composition/service_trace_log.h"

#include <exception>
#include <iomanip>
#include <utility>

#include "engine/cdvd/delivery_meter.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/timing/emu_domain.h"
#if PS2R_IOP_ORACLE
#include "engine/iop/iop_module_table.h"
#endif

namespace ps2r::app {
ServiceTraceLog::~ServiceTraceLog() {
    if (trace_) trace_->Disable();
}
#if PS2R_IOP_ORACLE
bool ServiceTraceLog::Install(const std::string& path, EmuDomain& domain,
                             iop::IopModuleTable* table, DeliveryMeter& reads) {
#else
bool ServiceTraceLog::Install(const std::string& path, EmuDomain& domain, DeliveryMeter& reads) {
    const void* table = nullptr;   // no remaining services: service_us / frame_us / replies stay 0
#endif
    if (path.empty()) return true;
    if (trace_ || log_.is_open()) return false;
    log_.open(path, std::ios::out | std::ios::trunc);
    if (!log_) return false;
    log_.exceptions(std::ios::badbit | std::ios::failbit);
    trace_ = std::make_unique<title::ServiceTrace>(
        [&domain, table, &reads] {
            title::ServiceTrace::Position p;
            const auto& read = reads.Timing()->Capture();
            p.presented = domain.BoundaryCount();
            p.domain = domain.DomainTick();
#if PS2R_IOP_ORACLE
            if (table) {
                const auto& clock = table->Kernel().Clock();
                p.service_us = clock.NowUs();
                p.service_frame_us = clock.Frames() * iop::IopClock::kFrameUs;
                p.replies = table->Rpc().Completed();
            }
#else
            (void)table;
#endif
            p.read_busy_ms = read.busy_ms;
            p.read_end_ms = read.last_end_ms;
            p.read_head = read.head_lba;
            return p;
        },
        [this](const title::ServiceTrace::Event& event) {
            const auto boundary = event.position.presented;
            if (boundary > 300 && (boundary < 1020 || boundary > 1140)) return;
            log_ << title::ServiceTrace::Format(event);
            if (const auto* files = StreamLibrary::Scheduling())
                log_ << " owned_time=" << files->UsesServiceTime()
                     << " owned_ms=" << std::setprecision(17) << files->ServiceInstantMs()
                     << " owned_sequence=" << files->ServiceSequence();
            log_ << '\n';
            ++lines_;
        });
    return trace_->Install(
        [](uint32_t address) { return DispatchTable::Capture(address); },
        [](uint32_t address, StreamLibrary::Body body) {
            return DispatchTable::Install(address, std::move(body));
        });
}
void ServiceTraceLog::Record(verify::RunMeta& meta) {
    if (log_.is_open()) {
        try { log_.flush(); } catch (const std::exception&) { ++write_failures_; }
    }
    meta.Set("service_trace", std::string("enabled=") + (trace_ ? "1" : "0") +
        ",lines=" + std::to_string(lines_) +
        ",observer_failures=" + std::to_string(trace_ ? trace_->ObserverFailures() : 0) +
        ",write_failures=" + std::to_string(write_failures_) +
        ",boundary=domain_boundary,ranges=0-300/1020-1140");
}
} // namespace ps2r::app
