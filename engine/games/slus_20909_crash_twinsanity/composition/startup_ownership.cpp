#include "games/slus_20909_crash_twinsanity/composition/startup_ownership.h"

#include <utility>

#include "engine/core/verify/run_meta.h"

namespace ps2r::app {
namespace {
using Startup = ::title::ServiceStartup;

std::string RetainedNames(const StartupOwnership::Report& report) {
    std::string names;
    for (size_t i = 0; i < Startup::kModules; ++i) {
        if (!report.retained[i]) continue;
        if (!names.empty()) names += '+';
        names += Startup::Name(static_cast<Startup::Module>(i));
    }
    return names.empty() ? std::string("none") : names;
}
}  // namespace

StartupOwnership::StartupOwnership(Report report) : report_(std::move(report)) {}

std::string StartupOwnership::Refusal() const {
    if (Owned()) return {};
    std::string text = "Live restore requires service-only startup; retained: " + RetainedNames(report_) + ".";
    for (const auto& consumer : report_.consumers) {
        text += ' ';
        text += Startup::Name(consumer.module);
        text += ": ";
        text += consumer.what;
        text += ';';
    }
    return text;
}

std::string StartupOwnership::Census() const {
    return std::string("stream=") + (StreamOwned() ? "native" : "retained") +
           ",sound=" + (SoundOwned() ? "native" : "retained") +
           ",retained=" + RetainedNames(report_) +
           ",consumers=" + std::to_string(report_.consumers.size());
}

void StartupOwnership::Record(verify::RunMeta& meta) const { meta.Set("service_startup", Census()); }

}  // namespace ps2r::app
