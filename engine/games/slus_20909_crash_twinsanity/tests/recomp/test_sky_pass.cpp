#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_pass.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using Pass = title::SkyPass;
using Setup = title::SkySetup;
using Finish = title::SkyIntent;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("SkyPass line%d: %s\n",__LINE__,#x); } } while(false)
struct Fixture {
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x400000);
    std::map<uint32_t,Pass::Body> dispatch;
    size_t original = 0, begins = 0, writes = 0, records = 0;
    bool begin_ok = true, write_ok = true, record_ok = true, record_throws = false;
    struct Refusal { title::GraphicsRefusals::Reason reason; uint32_t boundary, detail; };
    std::vector<Refusal> refusals;
    size_t faults = 0;
    std::unique_ptr<Pass> pass;
    template<class T> void Put(uint32_t at, const T& value) { std::memcpy(memory.data()+at,&value,sizeof(T)); }
    explicit Fixture(bool enabled = true) {
        Put<uint32_t>(title::MaterialState::kDisplayWidth,512);
        Put<uint32_t>(title::MaterialState::kDisplayHeight,448);
        Put<uint32_t>(title::MaterialState::kRenderTarget,0x11000);
        Put<uint32_t>(Setup::kRenderView,0x12000);
        Put(0x11000,std::array<uint32_t,8>{0,0,0,512,448,0,512,448});
        Put(0x12080,title::SkyGeometry::Matrix{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}});
        Pass::Hooks hooks;
        hooks.enabled=enabled;
        hooks.read=[&](uint32_t at,void* out,size_t size) {
            if(at>memory.size() || size>memory.size()-at) return false;
            std::memcpy(out,memory.data()+at,size); return true;
        };
        hooks.write=[&](const auto& batch) {
            ++writes;
            if(!write_ok) return false;
            for(const auto& item:batch) if(item.address>memory.size() || item.bytes.size()>memory.size()-item.address) return false;
            for(const auto& item:batch) std::memcpy(memory.data()+item.address,item.bytes.data(),item.bytes.size());
            return true;
        };
        hooks.environment=[](auto& env) {env.colour_clamp=true;return true;};
        hooks.begin=[&](uint32_t w,uint32_t h) {++begins;CHECK(w==512 && h==448);return begin_ok;};
        hooks.record=[&](const auto& output) {
            ++records;
            CHECK(output.draws.empty() && output.finish.draw.verts.size()==96);
            if(record_throws) throw std::runtime_error("record");
            return record_ok;
        };
        hooks.refuse=[&](auto reason,uint32_t boundary,uint32_t detail) {refusals.push_back({reason,boundary,detail});};
        hooks.fault=[&](auto,uint32_t,uint32_t) {++faults;};
        pass=std::make_unique<Pass>(std::move(hooks));
    }
    bool Install(uint32_t fail=0, bool missing=false) {
        return pass->Install([&](uint32_t) -> Pass::Body {
            if(missing) return {};
            return [&](CpuContext&,uint32_t) {++original;};
        },[&](uint32_t at,Pass::Body body) {
            if(at==fail) return false;
            dispatch[at]=std::move(body); return true;
        });
    }
    void Call(uint32_t address, uint32_t entry=0, uint32_t object=0x10000) {
        CpuContext context{};context.cpuRegs.GPR.r[4].UL[0]=object;
        dispatch.at(address)(context,entry ? entry:address);
    }
    bool Throws(uint32_t address) {
        try {Call(address);} catch(const std::exception&) {return true;} return false;
    }
};
void TestSelection() {
    Fixture model(false);
    CHECK(model.Install());
    for(auto at:{Setup::kAddress,Pass::kDrawModel,Finish::kAddress}) model.Call(at);
    CHECK(model.original==3 && model.begins==0 && model.writes==0);
    CHECK(model.pass->Stats().start.model_calls==1 && model.pass->Stats().model.model_calls==1 &&
          model.pass->Stats().finish.model_calls==1);
    Fixture selected;
    CHECK(selected.Install());
    selected.Call(Setup::kAddress);
    selected.Call(Finish::kAddress);
    CHECK(selected.original==0 && selected.begins==1 && selected.writes==1 && selected.records==1);
    CHECK(selected.pass->Stats().start.replaced==1 && selected.pass->Stats().finish.replaced==1);
    // DrawSkyModel with no StartSkyBuffer seen: an ordering REFUSAL -- the
    // original runs (counted as a model call), nothing is faulted or replayed.
    selected.Call(Pass::kDrawModel);
    CHECK(selected.original==1 && selected.pass->Stats().faults==0 && selected.pass->Stats().model.model_calls==1);
    CHECK(selected.refusals.size()==1 && selected.refusals[0].detail==uint32_t(Pass::Refusal::Unopened) &&
          selected.refusals[0].reason==title::GraphicsRefusals::Reason::Ordering &&
          selected.refusals[0].boundary==Pass::kDrawModel);
    selected.Call(Setup::kAddress); selected.Call(Finish::kAddress);
    CHECK(selected.original==1 && selected.pass->Stats().finish.replaced==2); // the owner stays live
    // A StartSkyBuffer this owner cannot take REFUSES before any commit: the
    // original Start runs and the whole pass is a model pass (DrawSkyModel and
    // FinishSky originals, counted as model calls), attributed once with why.
    const std::array<std::pair<Pass::Refusal,title::GraphicsRefusals::Reason>,4> declines{{
        {Pass::Refusal::Begin,title::GraphicsRefusals::Reason::Ordering},
        {Pass::Refusal::Source,title::GraphicsRefusals::Reason::UnsupportedState},
        {Pass::Refusal::Source,title::GraphicsRefusals::Reason::UnsupportedState},
        {Pass::Refusal::ModelMissing,title::GraphicsRefusals::Reason::MissingResource}}};
    for(size_t decline=0;decline!=declines.size();++decline) {
        Fixture fixture;
        CHECK(fixture.Install());
        if(decline==0) fixture.begin_ok=false;
        if(decline==1) fixture.Put<uint32_t>(0x10008,257);
        if(decline==2) {fixture.Put<uint32_t>(0x10008,1);fixture.Put<uint32_t>(0x1000c,0xfffffff0);}
        if(decline==3) {fixture.Put<uint32_t>(0x10008,1);fixture.Put<uint32_t>(0x1000c,0x20000);
                        fixture.Put<uint32_t>(0x20000,0x21000);fixture.Put<uint32_t>(0x2100c,0x777);}
        const auto before=fixture.memory;
        fixture.Call(Setup::kAddress);
        fixture.Call(Pass::kDrawModel);
        fixture.Call(Finish::kAddress);
        CHECK(fixture.original==3 && fixture.writes==0 && fixture.records==0 && fixture.memory==before);
        const auto stats=fixture.pass->Stats();
        CHECK(stats.start.model_calls==1 && stats.model.model_calls==1 && stats.finish.model_calls==1);
        CHECK(stats.start.replaced==0 && stats.faults==0 && stats.refused==1 && stats.last_refusal==declines[decline].first);
        CHECK(fixture.refusals.size()==1 && fixture.refusals[0].detail==uint32_t(declines[decline].first) &&
              fixture.refusals[0].reason==declines[decline].second && fixture.refusals[0].boundary==Setup::kAddress);
        CHECK(fixture.begins==(decline==0 ? 1u:0u));
        // The source FinishSky closed the model pass: the next pass is owned again.
        if(decline==0) fixture.begin_ok=true;
        if(decline) {fixture.Put<uint32_t>(0x10008,0);fixture.Put<uint32_t>(0x1000c,0);}
        fixture.Call(Setup::kAddress); fixture.Call(Finish::kAddress);
        CHECK(fixture.original==3 && fixture.pass->Stats().finish.replaced==1 && fixture.pass->Stats().refused==1);
    }
    // A second StartSkyBuffer inside an open model pass is refused (Nested) and
    // the model pass continues; it never selects mid-pass.
    Fixture nested;
    CHECK(nested.Install());
    nested.begin_ok=false;
    nested.Call(Setup::kAddress);
    nested.begin_ok=true;
    nested.Call(Setup::kAddress);
    CHECK(nested.original==2 && nested.begins==1 && nested.refusals.size()==2 &&
          nested.refusals[1].detail==uint32_t(Pass::Refusal::Nested));
    nested.Call(Finish::kAddress);
    nested.Call(Setup::kAddress); nested.Call(Finish::kAddress);
    CHECK(nested.original==3 && nested.pass->Stats().finish.replaced==1);
}
void TestFailuresAndRollback() {
    Fixture missing;
    CHECK(!missing.Install(0,true) && missing.dispatch.empty());
    for(uint32_t fail:{Pass::kDrawModel,Finish::kAddress,title::SkyAssets::kTextureDestroy}) {
        Fixture fixture;
        CHECK(!fixture.Install(fail));
        fixture.Call(Setup::kAddress);
        CHECK(fixture.original==1 && fixture.begins==0);
    }
    for(int error=0;error!=4;++error) {
        Fixture fixture;
        CHECK(fixture.Install());
        if(error==0) fixture.write_ok=false;
        if(error==0) CHECK(fixture.Throws(Setup::kAddress));
        else {
            fixture.Call(Setup::kAddress);
            if(error==1) fixture.record_ok=false;
            if(error==2) fixture.record_throws=true;
            if(error==3) CHECK(fixture.Throws(Setup::kAddress)); // nested pass, never replay
            else CHECK(fixture.Throws(Finish::kAddress));
        }
        CHECK(fixture.original==0 && fixture.pass->Stats().faults==1);
        CHECK(fixture.Throws(Finish::kAddress));
        fixture.pass->Reset();
        fixture.write_ok=fixture.record_ok=true;fixture.record_throws=false;
        fixture.Call(Setup::kAddress);fixture.Call(Finish::kAddress);
        CHECK(fixture.original==0 && fixture.pass->Stats().finish.replaced==1);
    }
}
void TestSelectedInteriorEntries() {
    const std::array<std::pair<uint32_t,uint32_t>,4> entries{{
        {Setup::kAddress,0x1b9d94}, {Setup::kAddress,0x1b9da4},
        {Pass::kDrawModel,0x1bec6c}, {Finish::kAddress,0x1ba198}
    }};
    for (const auto& entry : entries) {
        Fixture fixture;
        CHECK(fixture.Install());
        fixture.Call(Setup::kAddress);
        bool threw = false;
        try { fixture.Call(entry.first,entry.second); }
        catch (const std::exception&) { threw = true; }
        CHECK(threw && fixture.original==0 && fixture.pass->Stats().faults==1);
        CHECK(fixture.begins==1 && fixture.writes==1 && fixture.records==0);
        const auto counts = fixture.pass->Stats();
        CHECK(counts.start.interior_calls + counts.model.interior_calls + counts.finish.interior_calls==1);
        CHECK(fixture.Throws(Finish::kAddress));
        fixture.pass->Reset();
        fixture.Call(Setup::kAddress); fixture.Call(Finish::kAddress);
        CHECK(fixture.original==0 && fixture.pass->Stats().finish.replaced==1);
    }
    // An interior entry with no owned pass open: nothing committed, refused to
    // the original continuation (Interior), counted as an interior model call.
    Fixture missing_start;
    CHECK(missing_start.Install());
    missing_start.Call(Setup::kAddress,0x1b9d94);
    CHECK(missing_start.original==1 && missing_start.pass->Stats().faults==0 &&
          missing_start.pass->Stats().start.interior_calls==1 && missing_start.pass->Stats().start.model_calls==1);
    CHECK(missing_start.refusals.size()==1 && missing_start.refusals[0].detail==uint32_t(Pass::Refusal::Interior));
    missing_start.Call(Setup::kAddress); missing_start.Call(Finish::kAddress);
    CHECK(missing_start.original==1 && missing_start.pass->Stats().finish.replaced==1);
    // Faults after selection stay attributed (fault sink) and thrown.
    Fixture committed;
    CHECK(committed.Install());
    committed.Call(Setup::kAddress);
    CHECK(committed.Throws(Setup::kAddress) && committed.faults==1 && committed.pass->Stats().faults==1);
}
}
int main() {
    TestSelection();TestFailuresAndRollback();TestSelectedInteriorEntries();
    std::printf("SkyPass selection/lifetime boundary: %d checks, %d failures\n",checks,failures);
    return failures ? 1:0;
}
