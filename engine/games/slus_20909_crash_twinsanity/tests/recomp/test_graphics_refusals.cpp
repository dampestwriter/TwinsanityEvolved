#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include "engine/core/verify/run_meta.h"

namespace {
using Ledger = title::GraphicsRefusals;
using Family = Ledger::Family;
using Reason = Ledger::Reason;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (false)

std::map<std::string, std::string> Keys(const std::string& path) {
    std::map<std::string, std::string> keys;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq != std::string::npos) keys[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return keys;
}

void CountsAndSites() {
    Ledger ledger;
    uint64_t host = 60, source = 81;
    ledger.SetClocks([&] { return host; }, [&] { return source; });
    CHECK(ledger.Of(Family::Sprites).total == 0 && !ledger.Of(Family::Sprites).first.recorded);
    ledger.Refuse(Family::Sprites, Reason::Ordering, 0x1a9f20, 2);
    host = 61; source = 83;
    ledger.Refuse(Family::Sprites, Reason::MissingResource, 0x1a9f20, 0x103);
    ledger.Refuse(Family::Textures, Reason::UnsupportedState, 0x1bc920, 3);
    const auto& sprites = ledger.Of(Family::Sprites);
    CHECK(sprites.total == 2 && sprites.reasons[size_t(Reason::Ordering)] == 1 &&
          sprites.reasons[size_t(Reason::MissingResource)] == 1 && sprites.reasons[size_t(Reason::UnsupportedState)] == 0);
    CHECK(sprites.first.recorded && sprites.first.frame == 60 && sprites.first.source_frame == 81 &&
          sprites.first.ordinal == 1 && sprites.first.producer == 0x1a9f20 && sprites.first.detail == 2 &&
          sprites.first.reason == Reason::Ordering);
    CHECK(sprites.last.frame == 61 && sprites.last.source_frame == 83 && sprites.last.ordinal == 2 &&
          sprites.last.detail == 0x103 && sprites.last.reason == Reason::MissingResource);
    CHECK(sprites.faults == 0 && !sprites.first_fault.recorded);
    CHECK(ledger.Of(Family::Textures).total == 1 && ledger.Of(Family::Text).total == 0 &&
          ledger.Of(Family::Sky).total == 0 && ledger.Of(Family::Strips).total == 0);
    // Faults are attributed separately and never counted as refusals.
    ledger.Fault(Family::Sprites, Reason::UnsupportedState, 0x1a9f20, 4);
    const auto faulted = ledger.Of(Family::Sprites); // Of() is a copy under the ledger's lock
    CHECK(faulted.total == 2 && faulted.faults == 1 && faulted.first_fault.recorded &&
          faulted.first_fault.frame == 61 && faulted.first_fault.detail == 4 && faulted.first_fault.ordinal == 1);
    // Sinks bind the family.
    auto refuse = ledger.Refuser(Family::Sky), fault = ledger.Faulter(Family::Sky);
    refuse(Reason::MissingResource, 0x1b9d60, 5);
    fault(Reason::Ordering, 0x1bec10, 0);
    CHECK(ledger.Of(Family::Sky).total == 1 && ledger.Of(Family::Sky).faults == 1 &&
          ledger.Of(Family::Sky).first.detail == 5);
    // Out-of-range enumerators are ignored, never an abort.
    ledger.Refuse(Family::Count, Reason::Ordering, 0, 0);
    ledger.Refuse(Family::Sky, Reason::Count, 0, 0);
    CHECK(ledger.Of(Family::Sky).total == 1);
    // A throwing clock degrades the stamp to 0; the refusal is still counted.
    ledger.SetClocks([]() -> uint64_t { throw std::runtime_error("clock"); }, {});
    ledger.Refuse(Family::Text, Reason::Ordering, 0x19d150, 1);
    CHECK(ledger.Of(Family::Text).total == 1 && ledger.Of(Family::Text).first.frame == 0 &&
          ledger.Of(Family::Text).first.source_frame == 0);
}

void RetireKeepsProcessTotals() {
    Ledger retired, live;
    uint64_t frame = 10;
    retired.SetClocks([&] { return frame; }, {});
    retired.Refuse(Family::Strips, Reason::Ordering, 0x1a2000, 2);
    frame = 20;
    retired.Refuse(Family::Strips, Reason::UnsupportedState, 0x1a2000, 0x104);
    retired.Fault(Family::Strips, Reason::Ordering, 0x17ed50, 0);
    frame = 30;
    live.SetClocks([&] { return frame; }, {});
    live.Refuse(Family::Strips, Reason::Ordering, 0x1a2100, 3);
    live.Retire(retired);
    const auto& strips = live.Of(Family::Strips);
    CHECK(strips.total == 3 && strips.reasons[size_t(Reason::Ordering)] == 2 &&
          strips.reasons[size_t(Reason::UnsupportedState)] == 1 && strips.faults == 1);
    // First keeps the live owner's first (it was recorded first in this
    // process); last follows the retired owner's last; first_fault adopts the
    // retired owner's since the live one had none.
    CHECK(strips.first.frame == 30 && strips.last.frame == 20 && strips.first_fault.frame == 20);
    Ledger empty;
    empty.Retire(retired);
    CHECK(empty.Of(Family::Strips).total == 2 && empty.Of(Family::Strips).first.frame == 10);
}

void RecordFormat() {
    Ledger ledger;
    uint64_t host = 118, source = 157;
    ledger.SetClocks([&] { return host; }, [&] { return source; });
    ledger.Refuse(Family::Strips, Reason::Ordering, 0x1a2000, 2);
    host = 119; source = 158;
    ledger.Refuse(Family::Strips, Reason::UnsupportedState, 0x1a2080, 0x104);
    ledger.Fault(Family::Strips, Reason::Ordering, 0x17ed50, 0);
    const std::string path = "test_graphics_refusals.run.meta";
    {
        ps2r::verify::RunMeta meta(path, {"test", "new", "deterministic", "", 0});
        ledger.Record(meta, {Family::Strips, Family::Sky});
        meta.Finalize();
    }
    const auto keys = Keys(path);
    std::remove(path.c_str());
    CHECK(keys.count("graphics_refusals_strips") && keys.count("graphics_refusals_sky"));
    CHECK(!keys.count("graphics_refusals_sprites")); // only the requested families are published
    CHECK(keys.at("graphics_refusals_strips") ==
          "total=2,missing_resource=0,unsupported_state=1,ordering=1,faults=1,"
          "first=frame:118/source_frame:157/n:1/producer:0x1a2000/detail:2/reason:ordering,"
          "last=frame:119/source_frame:158/n:2/producer:0x1a2080/detail:260/reason:unsupported_state,"
          "first_fault=frame:119/source_frame:158/n:1/producer:0x17ed50/detail:0/reason:ordering");
    CHECK(keys.at("graphics_refusals_sky") ==
          "total=0,missing_resource=0,unsupported_state=0,ordering=0,faults=0,first=none,last=none,first_fault=none");
    CHECK(std::string(Ledger::Name(Family::Textures)) == "textures" && std::string(Ledger::Name(Family::Text)) == "text");
    CHECK(std::string(Ledger::Name(Reason::MissingResource)) == "missing_resource");
    CHECK(Ledger::Describe(Ledger::Site{}) == "none");
}
}  // namespace

int main() {
    CountsAndSites();
    RetireKeepsProcessTotals();
    RecordFormat();
    std::printf("GraphicsRefusals: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
