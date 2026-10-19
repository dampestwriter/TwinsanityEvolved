// core/config/mods_config.cpp -- see mods_config.h.
#include "engine/core/config/mods_config.h"

namespace ps2r {

bool ModsConfig::Enabled(const std::string& id) const {
    auto it = enabled.find(id);
    return it != enabled.end() && it->second;
}

bool ModsConfig::Any() const {
    for (const auto& kv : enabled)
        if (kv.second) return true;
    return false;
}

bool ModsConfig::ParseKey(const std::string& key, std::string* id) {
    static const char kPrefix[] = "mods.", kSuffix[] = ".enabled";
    const size_t np = sizeof(kPrefix) - 1, ns = sizeof(kSuffix) - 1;
    if (key.size() <= np + ns || key.compare(0, np, kPrefix) != 0 || key.compare(key.size() - ns, ns, kSuffix) != 0) return false;
    const std::string mid = key.substr(np, key.size() - np - ns);
    for (char c : mid)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    if (id) *id = mid;
    return true;
}

}  // namespace ps2r
