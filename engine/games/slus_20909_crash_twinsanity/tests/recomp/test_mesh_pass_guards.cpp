#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_pass.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using Pass = title::MeshPass;
using Intent = title::MeshIntent;
size_t checks = 0;
#define REQUIRE(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"Mesh guards line%d: %s\n",__LINE__,#x); std::exit(1); } } while(false)
constexpr uint32_t object=0x10000, view=0x11000, list=0x12000, material=0x13000,
    shader=0x14000, image=0x15000, target=0x16000, model=0x17000;
struct Fixture {
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x400000);
    title::MeshAssets::ModelLease geometry;
    title::MeshMaterial::Texture texture;
    std::map<uint32_t,Pass::Body> calls;
    std::unique_ptr<Pass> pass;
    unsigned original=0, records=0, vertices=0, reservations=0;
    bool owned=true, write_ok=true, reserve_ok=true, record_ok=true;
    template<class T> void Put(uint32_t at, T value) { std::memcpy(memory.data()+at,&value,sizeof(value)); }
    template<class T> T Get(uint32_t at) { T value;std::memcpy(&value,memory.data()+at,sizeof(value));return value; }
    Fixture(bool enabled=true) {
        Put(object+8,list); Put(object+12,model); Put<uint32_t>(object+24,1);
        Put(list,list+16);Put(list+24,material);Put<uint32_t>(model+8,1);
        Put(material,shader);Put<uint32_t>(material+16,1);Put<uint64_t>(material+24,8);
        Put<uint32_t>(material+32,2);Put<uint32_t>(material+0x60,1);
        Put<uint64_t>(shader,(uint64_t(7)<<26)|(uint64_t(2)<<21)|(uint64_t(1)<<55));
        Put(shader+20,image);Put<uint32_t>(shader+0x64,1);Put(shader+0x6c,title::MeshMaterial::kVtable);
        Put<uint32_t>(title::MaterialState::kDisplayWidth,512);Put<uint32_t>(title::MaterialState::kDisplayHeight,448);
        Put<uint32_t>(title::MaterialState::kDisplayPixelBytes,2);Put<uint32_t>(title::MaterialState::kDepthBufferPage,168);
        Put(title::MaterialState::kRenderTarget,target);
        Put(target,std::array<uint32_t,8>{0,0,0,512,448,0,512,448});
        title::MeshGeometry::Matrix screen{{{16,0,0,0},{0,16,0,0},{0,0,1,0},{32768,32768,8388607,1}}};
        Put(view+0x80,screen);Put<uint32_t>(view+0xf8,1);
        auto mesh=std::make_shared<title::MeshAssets::Model>();
        mesh->parts.resize(1);mesh->parts[0].strips.resize(1);
        auto& points=mesh->parts[0].strips[0].vertices;
        points.resize(3);points[1].position[0]=1;points[2].position[1]=1;
        for(auto& point:points){point.stq={0,0,1};point.colour={128,64,32,127};}
        geometry=mesh;
        auto lease=std::make_shared<title::Graphics::Resource>();
        lease->description={32,32,ps2r::render::SurfaceFormat::Rgba8,false,1};lease->lifetime=std::make_shared<int>(1);
        texture.lease=lease;texture.width=texture.height=32;texture.buffer_width=64;
        texture.levels=1;texture.source_format=19;texture.alpha=true;
        Pass::Hooks hooks;
        hooks.enabled=enabled;
        hooks.read=[&](uint32_t at,void* out,size_t size){
            if(at>memory.size()||size>memory.size()-at)return false;
            std::memcpy(out,memory.data()+at,size);return true;
        };
        hooks.write=[&](const std::vector<Pass::Write>& writes){
            if(!write_ok)return false;
            for(const auto& write:writes)if(write.address>memory.size()||write.bytes.size()>memory.size()-write.address)return false;
            for(const auto& write:writes)std::memcpy(memory.data()+write.address,write.bytes.data(),write.bytes.size());
            return true;
        };
        hooks.model=[&](uint32_t at){return at==model?geometry:title::MeshAssets::ModelLease{};};
        hooks.texture=[&](uint32_t at,title::MeshMaterial::Texture& out){out=texture;return at==image;};
        hooks.environment=[](title::MaterialState::Environment& out){out.colour_clamp=true;return true;};
        hooks.owns_material=[&](uint32_t){return owned;};
        hooks.reserve=[&](const std::vector<Pass::Request>& requests,std::vector<Pass::Record>& out){
            if(!reserve_ok)return false;
            for(const auto& request:requests){
                REQUIRE(request.material==material&&request.shader.bucket==2);
                ++reservations;
                out.push_back([&](const std::vector<ps2r::gs::DrawIr>& draws){
                    if(!record_ok)return false;
                    ++records;for(const auto& draw:draws)vertices+=draw.verts.size();return true;
                });
            }return true;
        };
        pass=std::make_unique<Pass>(std::move(hooks));
    }
    bool Install(int fail=-1) {
        int installed=0;
        return pass->Install([&](uint32_t)->Pass::Body{return [&](CpuContext&,uint32_t){++original;};},
            [&](uint32_t at,Pass::Body body){if(installed++==fail)return false;calls[at]=std::move(body);return true;});
    }
    void Call(uint32_t boundary, uint32_t at=0) {
        CpuContext context{};context.cpuRegs.GPR.r[4].UL[0]=boundary==Intent::kFlushMaterial?material:object;
        context.cpuRegs.GPR.r[5].UL[0]=view;
        calls.at(boundary)(context,at?at:boundary);
    }
    void Throws(uint32_t boundary,uint32_t at=0){bool threw=false;try{Call(boundary,at);}catch(const std::runtime_error&){threw=true;}REQUIRE(threw);}
};
}
int main(){
    {Fixture f(false);REQUIRE(f.Install());f.Call(Intent::kPlaced);REQUIRE(f.original==1&&f.Get<uint32_t>(Pass::kMaterialCount)==0);}
    {Fixture f;f.owned=false;REQUIRE(f.Install());f.Call(Intent::kPlaced);REQUIRE(f.original==1&&f.Get<uint32_t>(Pass::kMaterialCount)==0);}
    {Fixture f;REQUIRE(f.Install());for(unsigned i=0;i<24;++i)f.Call(Intent::kPlaced);
        REQUIRE(!f.original&&f.pass->Stats().placed.replaced==24&&f.pass->Stats().blocks==2);
        REQUIRE(f.Get<uint32_t>(Pass::kMaterialCount)==1&&f.Get<uint32_t>(material+0x28)==0);
        f.Call(Intent::kClose);f.Call(Intent::kFlushMaterial);REQUIRE(f.records==1&&f.vertices==72&&f.original==1);
        f.Call(Pass::kForget);REQUIRE(f.original==2&&!f.pass->Stats().faults);}
    {Fixture f;REQUIRE(f.Install());f.Call(Intent::kPlaced,Intent::kPlaced+4);REQUIRE(f.original==1);
        f.Call(Intent::kPlaced);f.Throws(Intent::kPlaced,Intent::kPlaced+4);REQUIRE(f.original==1&&f.pass->Stats().faults==1);}
    {Fixture f;REQUIRE(f.Install());f.Call(Intent::kPlaced);f.Throws(Intent::kFlushMaterial);REQUIRE(!f.original&&!f.records);}
    {Fixture f;REQUIRE(f.Install());f.Call(Intent::kPlaced);f.Put<uint32_t>(material+0x28,0x12340);
        f.Call(Intent::kClose);f.Throws(Intent::kFlushMaterial);REQUIRE(f.original==1&&!f.records);}
    {Fixture f;REQUIRE(f.Install());f.Call(Intent::kPlaced);f.Put<uint8_t>(object+20,1);
        f.Throws(Intent::kPlaced);REQUIRE(!f.original&&!f.records);}
    {Fixture f;f.write_ok=false;REQUIRE(f.Install());f.Throws(Intent::kPlaced);
        REQUIRE(!f.original&&f.Get<uint32_t>(Pass::kMaterialCount)==0);}
    for(bool fail_reservation:{false,true}){Fixture f;REQUIRE(f.Install());f.Call(Intent::kPlaced);f.Call(Intent::kClose);
        f.reserve_ok=!fail_reservation;f.record_ok=fail_reservation;f.Throws(Intent::kFlushMaterial);
        REQUIRE(f.original==1&&!f.records&&f.pass->Stats().faults==1);}
    {Fixture f;REQUIRE(f.Install());f.Call(Intent::kPlaced);f.Throws(Intent::kDefault);REQUIRE(!f.original);}
    for(int fail=0;fail<6;++fail){Fixture f;REQUIRE(!f.Install(fail));for(auto& [entry,body]:f.calls){CpuContext c{};body(c,entry);}
        REQUIRE(f.original==unsigned(fail));}
    std::printf("MeshPass guards: %zu checks passed\n",checks);
}
