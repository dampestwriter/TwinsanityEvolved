#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ps2r::gs { struct DrawIr; }

namespace retirement {

// Writer for the same-input self-oracle receipt pair that
// verify/retirement_artifacts.py check_oracle recomputes: per arm (model/native)
// an input channel, named output channels, the registered command, binary and
// source digests, settings and counters (gate grammar `k=v,...`); plus the
// proposed artifacts.json index with optional attributions for channels that
// differ by design. Receipts are written only by Finish; an existing directory
// is refused so prior evidence is never overwritten.
class OracleArtifacts {
public:
    using Bytes = std::vector<uint8_t>;
    struct Attribution {
        std::string channel, reason, specification_file, specification_needle, pin;
    };
    // Parses `--export-dir DIR --source-manifest FILE` (both or neither) from
    // argv[first..]; any other shape returns false.
    bool Parse(int argc, char** argv, int first);
    bool Enabled() const { return !directory_.empty(); }
    Bytes& Input(bool native) { return arms_[native].input; }
    Bytes& Output(bool native, const std::string& channel) { return arms_[native].outputs[channel]; }
    void Setting(const std::string& key, const std::string& model, const std::string& native);
    void Counter(bool native, const std::string& field, const std::string& row);
    // Free-text index fields (scope, canonicalization, ...), quoted on write.
    void Note(const std::string& key, const std::string& value);
    // A channel expected to differ between the arms: the index records the
    // exact differing byte ranges, the reason, a decomp reference and the
    // native bytes as the expected output, for the parent's manifest.
    void Attribute(const Attribution& attribution);
    void Finish(const std::string& test, const std::string& schema, int argc, char** argv);

    // Appends one draw to the arm's "draw" channel (canonical DrawRec plus the
    // out-of-record raster dither/dimx, texture mip/LOD and TEXA fields) and its
    // vertices to "vertices" (count, then VertexRec array). Canonicalization
    // flags apply identically to both arms: inactive ALPHA (PRIM.ABE=0) zeroed,
    // inactive texture view (PRIM.TME=0) zeroed; mip addresses are always zeroed
    // when MXL=0 or MMIN<2 (no level is addressed). Everything else is raw.
    void RecordDraw(bool native, const ps2r::gs::DrawIr& draw, bool zero_inactive_alpha,
                    bool zero_inactive_texture);
    static std::string Sha256Hex(const Bytes& bytes);
    static std::vector<std::pair<size_t, size_t>> DifferenceRanges(const Bytes& left, const Bytes& right);

private:
    struct Arm {
        Bytes input;
        std::map<std::string, Bytes> outputs;
        std::map<std::string, std::string> counters, settings;
    };
    std::filesystem::path directory_, source_manifest_;
    Arm arms_[2];
    std::vector<std::pair<std::string, std::string>> notes_;
    std::vector<Attribution> attributions_;
};

}  // namespace retirement
