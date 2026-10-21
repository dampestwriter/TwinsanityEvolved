// te_profile.h -- TE_PROFILE=FILE: a sampling profile of the process's threads, written at exit. See te_profile.cpp.
#pragma once

namespace te::profile {
void StartFromEnvironment();   // no-op unless TE_PROFILE names a file
}  // namespace te::profile
