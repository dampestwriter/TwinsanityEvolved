// te_autopilot.h -- assisted play for pad-log authoring (--autopilot PLAN, with --record-pad). See te_autopilot.cpp.
#pragma once
#include <string>

namespace te::autopilot {
bool Load(const std::string& plan_path, std::string* error);   // installs the override sampler
bool Done();
}  // namespace te::autopilot
