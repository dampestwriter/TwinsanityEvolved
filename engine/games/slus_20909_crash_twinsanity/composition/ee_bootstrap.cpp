#include "games/slus_20909_crash_twinsanity/composition/leg_state.h"

#include "engine/platform/host/platform.h"

// The composition root's entry and the helpers its phase TUs share. The phases
// themselves (one function each, in construction order) are the ee_bootstrap_*.cpp
// files; LegState (leg_state.h) is what one phase hands the next.

std::string Hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string Basename(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ExpectedHash(const std::string& tsv, const std::string& anchor_file, uint64_t* out) {
    std::ifstream in(tsv);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string name, hash;
        if (!(ss >> name >> hash)) continue;
        if (name == anchor_file || Basename(name) == anchor_file) {
            *out = std::strtoull(hash.c_str(), nullptr, 16);
            return true;
        }
    }
    return false;
}

uint64_t HostPhysFootprintBytes() { return ps2r::platform::host::PhysFootprintBytes(); }
std::string HostCpuFeaturesCensus() { return ps2r::platform::host::CpuFeaturesCensus(); }

#if PS2R_DIAGNOSTICS
WatchFlushState& WatchFlush() {
    static WatchFlushState state;
    return state;
}

TexresTraceRequest ReadTexresTraceRequest(const std::string& dump_dir) {
    TexresTraceRequest r;
    if (dump_dir.empty()) return r;
    std::ifstream in(dump_dir + "/texres_trace.cfg");
    if (!in) return r;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "tbp") r.filter.tbp = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
        else if (k == "cbp") r.filter.cbp = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
        else if (k == "bytes") r.filter.keep_bytes = v == "1";
        else if (k == "from") r.from = std::strtoull(v.c_str(), nullptr, 10);
    }
    r.enabled = true;
    r.filter.enabled = true;
    return r;
}

void WriteTexresTrace(const ps2r::gs::TexresTrace& tr, const std::string& dump_dir, uint64_t key) {
    if (tr.LastFrame().empty()) return;
    const std::string base = dump_dir + "/texres_f" + std::to_string(key);
    if (std::FILE* f = std::fopen((base + ".txt").c_str(), "w")) {
        tr.Write(f);
        std::fclose(f);
    }
    if (tr.GetFilter().keep_bytes) {
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
        if (!ec) tr.WriteBytes(base, "d");
    }
}
#endif

int EeBootstrap::Run(const ps2r::Config& cfg, ps2r::verify::VerifyHost& host, const TitleProfile& title,
                     const Paths& paths) {
    LegState s;
    s.cfg = &cfg;
    s.host = &host;
    s.title = &title;
    s.paths = &paths;
    return ArmMemoryAndClocks(s);
}
