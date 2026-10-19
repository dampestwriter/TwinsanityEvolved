// te_vita.h -- the PlayStation Vita runner's start-up: the device has no command line, no terminal and no
// home directory, so the arguments are baked (or read from ux0:data/twinsanity/args.txt), stderr goes to a log file under
// the same folder and the kernel's memory budget is reported once. Compiled on the Vita lane only (runner/te_vita.cpp).
#pragma once

namespace te::vita {

// Replaces argc/argv with the device's arguments (the storage they point at lives for the process) and redirects stderr
// / stdout to ux0:data/twinsanity/twinsanityevolved.log. Called first in main().
void Startup(int* argc, char*** argv);

}  // namespace te::vita
