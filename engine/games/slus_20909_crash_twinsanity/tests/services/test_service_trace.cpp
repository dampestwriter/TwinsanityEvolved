#include <cstdio>
#include <map>
#include <stdexcept>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/scheduler/service_trace.h"

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)
}
int main() {
    using title::ServiceTrace;
    std::map<uint32_t, StreamLibrary::Body> dispatch;
    uint32_t original_calls = 0, original_entry = 0;
    bool original_throws = false;
    auto capture = [&](uint32_t address) {
        if (!dispatch.count(address))
            dispatch[address] = [&, address](CpuContext& ctx, uint32_t entry) {
                ++original_calls; original_entry = entry;
                if (original_throws) throw std::runtime_error("body");
                if (address == 0x1e2958) dispatch.at(0x1e6dd0)(ctx, 0);
                ctx.cpuRegs.GPR.r[2].SD[0] = -37;
                ctx.cpuRegs.GPR.r[4].UD[0] += 2;
            };
        return dispatch.at(address);
    };
    auto install = [&](uint32_t address, StreamLibrary::Body body) {
        dispatch[address] = std::move(body); return true;
    };
    std::vector<ServiceTrace::Event> events;
    ServiceTrace::Position position;
    position.domain = 113; position.presented = 100; position.service_us = 900000;
    position.service_frame_us = 700000; position.read_busy_ms = 2107.125;
    bool observer_throws = false;
    CpuContext ctx{};
    ctx.cpuRegs.GPR.r[4].UD[0] = 7;
    ctx.cpuRegs.GPR.r[31].UD[0] = 0xabcdef;
    {
        ServiceTrace trace([&] { return position; }, [&](const auto& event) {
            if (observer_throws) throw std::runtime_error("observer");
            events.push_back(event);
        });
        CHECK(!trace.Install([](uint32_t) { return StreamLibrary::Body{}; }, install));
        CHECK(!trace.Install(capture, [&](uint32_t address, StreamLibrary::Body body) {
            if (address == 0x2ada30) return false;
            return install(address, std::move(body));
        }));
        dispatch.at(0x2aac80)(ctx, 0x2aac84);
        CHECK(events.empty() && original_calls == 1 && original_entry == 0x2aac84);
        CHECK(trace.Install(capture, install) && trace.Install(capture, install));
        ctx.cpuRegs.GPR.r[4].UD[0] = 7;
        dispatch.at(0x1e2958)(ctx, 0);
        CHECK(original_calls == 3 && ctx.cpuRegs.GPR.r[2].SD[0] == -37 && ctx.cpuRegs.GPR.r[4].UL[0] == 11);
        CHECK(events.size() == 4 && !events[0].leaving && !events[1].leaving && events[2].leaving && events[3].leaving);
        CHECK(events[1].parent == events[0].call && events[1].depth == 1 && events[3].depth == 0);
        CHECK(events[0].arguments[0] == 7 && events[3].arguments[0] == 7 &&
              events[3].result == -37 && events[3].caller == 0xabcdef);
        CHECK(events[3].position.service_us == 900000 && events[3].position.domain == 113);
        CHECK(ServiceTrace::Format(events[3]).find("busy_ms=2107.125") != std::string::npos);
        observer_throws = true;
        dispatch.at(0x1e6dd0)(ctx, 0x1e6dd4);
        CHECK(trace.ObserverFailures() == 2 && original_calls == 4 && original_entry == 0x1e6dd4);
        observer_throws = false;
        original_throws = true;
        bool propagated = false;
        try { dispatch.at(0x1e6dd0)(ctx, 0); } catch (const std::runtime_error&) { propagated = true; }
        CHECK(propagated && original_calls == 5);
        original_throws = false;
        dispatch.at(0x1e6dd0)(ctx, 0);
        CHECK(events.back().depth == 0); // body exception unwinds trace nesting
    }
    const auto count = events.size();
    dispatch.at(0x1e6dd0)(ctx, 0x1e6dd0);
    CHECK(events.size() == count && original_calls == 7); // destroyed observer cannot dangle
    std::printf("ServiceTrace: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
