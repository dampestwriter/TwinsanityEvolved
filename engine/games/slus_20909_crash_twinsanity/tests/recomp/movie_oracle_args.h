#pragma once

#include <filesystem>
#include <string>
#include <utility>

namespace movie_oracle {

// Optional fixture inputs belong to the test command, so retirement receipts bind
// them to the registered CTest invocation. Both paths are supplied together.
struct ExportArgs {
    std::filesystem::path directory, source_manifest;

    static bool Parse(int argc, char** argv, int first, ExportArgs& out) {
        ExportArgs parsed;
        for (int i = first; i < argc; i += 2) {
            if (i + 1 == argc || !*argv[i + 1]) return false;
            const std::string option = argv[i];
            const std::string value = argv[i + 1];
            if (value.compare(0, 2, "--") == 0) return false;
            if (option == "--export-dir" && parsed.directory.empty())
                parsed.directory = value;
            else if (option == "--source-manifest" && parsed.source_manifest.empty())
                parsed.source_manifest = value;
            else return false;
        }
        if (parsed.directory.empty() != parsed.source_manifest.empty()) return false;
        out = std::move(parsed);
        return true;
    }
};

}  // namespace movie_oracle
