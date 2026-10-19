// tests/iop/test_stream_irx_stub_surface.cpp -- StreamIrxStubSurface: the 74-name
// contract between the generated STREAM.IRX TU and StubSurface. (1) census: every
// `iop_stubs::` name the installed iop_recompiled.cpp references has a census row
// and every row is referenced. The LINK GUARD: main() references the generated
// `iop_recompiled_functions` map, which pulls iop_recompiled.o out of the static
// archive, so every `iop_stubs::` extern the TU names must have a forwarder or
// this binary fails to link (an archive member is only linked when referenced;
// without that reference the guard would be vacuous); (2) the artifact pin ported from OLD
// tests/test_iop_sysclib_map.cpp: the seven sysclib call-site counts, the five
// pre-S44 names absent, no UNKNOWN STUB, and the 0x1a8c slice copy = iop_memcpy;
// (3) the forwarders reach the bound modules ($v0 travels back).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "engine/iop/hle/intrman/intrman_module.h"
#include "engine/iop/hle/sifman/sifman_module.h"
#include "engine/iop/hle/sysclib/sysclib_module.h"
#include "engine/iop/hle/sysmem/sysmem_module.h"
#include "engine/iop/iop_memory.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/abi/host_app/iop_stubs.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/abi/stub_surface.h"
#include "iop_recompiled.h"   // generated (build dir; ps2r_title_stream_irx exports PS2R_IOP_GEN_DIR)
#include "tests/iop/vector_ee_ram_sink.h"

using namespace ps2r::iop;
using namespace ps2r::iop::hle;
using ps2r::title::StubSurface;

static int g_fail = 0;
#define CHECK(c, msg)                                                          \
    do {                                                                       \
        if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); g_fail++; } \
    } while (0)

static size_t CountOccurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size())) ++n;
    return n;
}

static std::set<std::string> ReferencedNames(const std::string& src) {
    std::set<std::string> names;
    const std::string key = "iop_stubs::";
    for (size_t pos = src.find(key); pos != std::string::npos; pos = src.find(key, pos + key.size())) {
        size_t end = pos + key.size();
        while (end < src.size() && (std::isalnum(static_cast<unsigned char>(src[end])) || src[end] == '_')) ++end;
        names.insert(src.substr(pos + key.size(), end - pos - key.size()));
    }
    return names;
}

static void TestCensus(const std::string& src) {
    const std::set<std::string> referenced = ReferencedNames(src);
    std::printf("referenced iop_stubs:: names: %zu; census rows: %zu (kernel-facing %zu, device-facing %zu)\n",
                referenced.size(), StubSurface::kRows, StubSurface::KernelFacingCount(), StubSurface::DeviceFacingCount());
    CHECK(referenced.size() == 74, "the generated TU references exactly 74 distinct iop_stubs:: names");
    size_t unlisted = 0;
    for (const std::string& n : referenced) {
        if (StubSurface::FindRow(n.c_str()) < 0) {
            std::printf("UNLISTED EXTERN: iop_stubs::%s has no StubSurface census row\n", n.c_str());
            ++unlisted;
        }
    }
    CHECK(unlisted == 0, "every referenced extern has a provider row (0 unlisted)");
    size_t unreferenced = 0;
    std::set<std::string> seen;
    for (size_t i = 0; i < StubSurface::kRows; ++i) {
        const char* name = StubSurface::Census()[i].name;
        CHECK(seen.insert(name).second, "census rows are distinct");
        if (!referenced.count(name)) {
            std::printf("STALE ROW: census lists iop_stubs::%s but the TU never references it\n", name);
            ++unreferenced;
        }
    }
    CHECK(unreferenced == 0, "every census row is referenced by the TU (the table is honest)");
    CHECK(StubSurface::KernelFacingCount() == 49 && StubSurface::DeviceFacingCount() == 25,
          "49 kernel-facing rows (10 libraries + the thunk) and 25 device-facing rows (libsd 16 + cdvdman 9)");
    std::printf("census: %zu referenced / %zu providers / %zu unlisted\n", referenced.size(), StubSurface::kRows, unlisted);
}

static void TestArtifactPin(const std::string& src) {
    auto expect_count = [&](const char* needle, size_t want) {
        const size_t got = CountOccurrences(src, needle);
        if (got != want) {
            std::printf("FAIL: '%s' expected %zu occurrences, got %zu\n", needle, want, got);
            g_fail++;
        }
    };
    // The seven correct sysclib bindings (call-site counts pinned by the S44 map fix).
    expect_count("iop_stubs::iop_memcpy(iop_ctx);", 1);
    expect_count("iop_stubs::iop_memset(iop_ctx);", 1);
    expect_count("iop_stubs::iop_strcat(iop_ctx);", 1);
    expect_count("iop_stubs::iop_strcpy(iop_ctx);", 1);
    expect_count("iop_stubs::iop_strlen(iop_ctx);", 1);
    expect_count("iop_stubs::iop_strncmp(iop_ctx);", 3);
    expect_count("iop_stubs::iop_strstr(iop_ctx);", 1);
    // The five pre-S44 names are gone.
    expect_count("iop_stubs::iop_memcmp(iop_ctx);", 0);
    expect_count("iop_stubs::iop_prnt(iop_ctx);", 0);
    expect_count("iop_stubs::iop_strchr(iop_ctx);", 0);
    expect_count("iop_stubs::iop_index(iop_ctx);", 0);
    expect_count("iop_stubs::iop_strrchr(iop_ctx);", 0);
    expect_count("UNKNOWN STUB", 0);
    // The CD-job executor's partial-sector slice copy (FUN_00001930, pc 0x1a8c) binds memcpy.
    const size_t site = src.find("// jal + delay slot at 0x1a8c");
    CHECK(site != std::string::npos, "the jal block at 0x1a8c exists");
    if (site != std::string::npos) {
        const size_t call = src.find("iop_stubs::", site);
        CHECK(call != std::string::npos && src.compare(call, std::strlen("iop_stubs::iop_memcpy"), "iop_stubs::iop_memcpy") == 0,
              "the 0x1a8c slice-copy call site is iop_memcpy");
    }
}

static void TestForwardersReachModules() {
    IopMemory mem;
    SysmemModule sysmem(mem);
    IntrmanModule intrman(mem);
    SysclibModule sysclib(mem);
    VectorEeRamSink sink;
    SifmanModule sifman(mem, &sink);
    StubSurface::BindMemory(mem);
    StubSurface::Modules m;
    m.sysmem = &sysmem;
    m.intrman = &intrman;
    m.sysclib = &sysclib;
    m.sifman = &sifman;
    StubSurface::BindModules(m);

    IopContext ctx;
    ctx.gpr[4] = 0x1000;
    for (uint32_t i = 0; i < 5; ++i) mem.Write<uint8_t>(0x1000 + i, static_cast<uint8_t>("abcd"[i]));
    iop_stubs::iop_strlen(ctx);
    CHECK(ctx.gpr[2] == 4, "iop_stubs::iop_strlen reaches SysclibModule::strlen");

    ctx.gpr[4] = 0x2000;   // descriptor: src 0x1000, dst 8, size 4
    mem.Write<uint32_t>(0x2000, 0x1000);
    mem.Write<uint32_t>(0x2004, 8);
    mem.Write<uint32_t>(0x2008, 4);
    ctx.gpr[5] = 1;
    iop_stubs::sceSifSetDma(ctx);
    CHECK(ctx.gpr[2] == 1 && std::memcmp(sink.bytes + 8, "abcd", 4) == 0, "sceSifSetDma copies the descriptor into the EE sink, id 1");
    iop_stubs::sceSifCheckInit(ctx);
    CHECK(ctx.gpr[2] == 1, "sceSifCheckInit -> 1");

    // Device-facing rows idle to $v0 = 0 until 4B'' binds a provider.
    ctx.gpr[2] = 0xAB;
    iop_stubs::sceCdSync(ctx);
    CHECK(ctx.gpr[2] == 0, "an unbound device-facing row idles to 0");
    size_t seen_row = 999;
    StubSurface::BindDeviceProvider([&](size_t row, IopContext& c) { seen_row = row; c.gpr[2] = 7; });
    iop_stubs::libsd_stub_1(ctx);
    CHECK(ctx.gpr[2] == 7 && StubSurface::FindRow("libsd_stub_1") == static_cast<int>(seen_row),
          "a bound device provider receives the census row of the called name");
    iop_stubs::sceCdMmode(ctx);
    CHECK(StubSurface::FindRow("sceCdMmode") == static_cast<int>(seen_row), "cdvdman rows index into the same census");
    StubSurface::Unbind();
}

// The link guard made real: this reference forces iop_recompiled.o out of the
// ps2r_title_stream_irx archive, so its 74 `iop_stubs::` externs must all resolve
// against StubSurface's forwarders at link time. The map has one entry per
// generated function (248, MANIFEST.md) and the module entry 0x13C is among them.
static void TestLinkGuard() {
    initialize_iop_recompiled_functions();
    std::printf("generated map: %zu functions (the TU is linked in)\n", iop_recompiled_functions.size());
    CHECK(iop_recompiled_functions.size() == 248, "iop_recompiled_functions holds the 248 generated functions");
    CHECK(iop_recompiled_functions.count(0x13Cu) == 1, "the module entry iop_func_0000013c is in the map");
    CHECK(iop_recompiled_functions.count(0x1930u) == 1, "the CD-job executor FUN_00001930 is in the map");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <path to iop_recompiled.cpp>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::printf("FAIL: cannot open %s\n", argv[1]);
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    TestLinkGuard();
    TestCensus(src);
    TestArtifactPin(src);
    TestForwardersReachModules();
    std::printf("%s: %d failure(s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
