#include "games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "engine/gs/hle/ir/draw_ir.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/sha256.h"
#include "engine/gs/hle/ir/gsdl_format.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_oracle_args.h"

namespace retirement {
namespace {
using Bytes = OracleArtifacts::Bytes;

[[noreturn]] void Fail(const char* what) {
    std::fprintf(stderr, "retirement oracle artifacts: %s\n", what);
    std::exit(1);
}
#define CHECK(value) do { if (!(value)) Fail(#value); } while (false)

using ps2r_tests::Sha256;

std::string Digest(const Bytes& bytes) {
    Sha256 sha;
    sha.Update(bytes.data(), bytes.size());
    return sha.Hex();
}
std::string Quote(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char b : value) {
        if (b == '"' || b == '\\') out << '\\' << b;
        else if (b < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << unsigned(b);
        else out << b;
    }
    out << '"'; return out.str();
}
Bytes Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    CHECK(file.good() && file.tellg() > 0);
    Bytes bytes(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    CHECK(file.good()); return bytes;
}
void Save(const std::filesystem::path& path, const Bytes& bytes) {
    CHECK(!std::filesystem::exists(path));
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    file.close(); CHECK(file.good() && std::filesystem::is_regular_file(path));
}
void Save(const std::filesystem::path& path, const std::string& text) {
    Save(path, Bytes(text.begin(), text.end()));
}
}  // namespace

std::string OracleArtifacts::Sha256Hex(const Bytes& bytes) { return Digest(bytes); }

namespace {
template <class T> void Word(Bytes& out, T value) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.insert(out.end(), raw, raw + sizeof(T));
}
}  // namespace

void OracleArtifacts::RecordDraw(bool native, const ps2r::gs::DrawIr& source, bool zero_inactive_alpha,
                                 bool zero_inactive_texture) {
    ps2r::gs::DrawIr draw = source;
    if (zero_inactive_alpha && !draw.alpha_blend) draw.raster.alpha = {};
    if (zero_inactive_texture && !draw.tex_enabled) draw.tex = {};
    // No mip level is addressed without levels (MXL=0) or without a mip
    // minification filter (MMIN < 2): MIPTBP1/2 are inactive registers then.
    if (draw.tex.mxl == 0 || draw.tex.mmin < 2)
        for (size_t i = 0; i < 6; ++i) draw.tex.mip_tbp[i] = draw.tex.mip_tbw[i] = 0;
    Bytes& out = arms_[native].outputs["draw"];
    const ps2r::gs::DrawRec rec = draw.ToDrawRec();
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&rec), reinterpret_cast<const uint8_t*>(&rec) + sizeof(rec));
    Word(out, draw.raster.dthe); Word(out, draw.raster.dimx);
    Word(out, draw.tex.mxl); Word(out, uint8_t(draw.tex.lcm)); Word(out, draw.tex.l); Word(out, draw.tex.k);
    for (uint32_t tbp : draw.tex.mip_tbp) Word(out, tbp);
    for (uint32_t tbw : draw.tex.mip_tbw) Word(out, tbw);
    Word(out, draw.tex.texa_ta0); Word(out, draw.tex.texa_ta1);
    Bytes& verts = arms_[native].outputs["vertices"];
    Word(verts, uint32_t(draw.verts.size()));
    verts.insert(verts.end(), reinterpret_cast<const uint8_t*>(draw.verts.data()),
                 reinterpret_cast<const uint8_t*>(draw.verts.data()) + draw.verts.size() * sizeof(draw.verts[0]));
}

std::vector<std::pair<size_t, size_t>> OracleArtifacts::DifferenceRanges(const Bytes& left, const Bytes& right) {
    // verify/retirement_artifacts.py difference_ranges: every differing interval, tails included.
    std::vector<std::pair<size_t, size_t>> intervals;
    const size_t longest = std::max(left.size(), right.size()), shortest = std::min(left.size(), right.size());
    size_t begin = longest + 1;
    for (size_t index = 0; index < longest; ++index) {
        const bool different = index >= shortest || left[index] != right[index];
        if (different && begin > longest) begin = index;
        if (!different && begin <= longest) { intervals.emplace_back(begin, index); begin = longest + 1; }
    }
    if (begin <= longest) intervals.emplace_back(begin, longest);
    return intervals;
}

bool OracleArtifacts::Parse(int argc, char** argv, int first) {
    movie_oracle::ExportArgs args;
    if (!movie_oracle::ExportArgs::Parse(argc, argv, first, args)) return false;
    directory_ = args.directory;
    source_manifest_ = args.source_manifest;
    return true;
}

void OracleArtifacts::Setting(const std::string& key, const std::string& model, const std::string& native) {
    arms_[0].settings[key] = model;
    arms_[1].settings[key] = native;
}
void OracleArtifacts::Counter(bool native, const std::string& field, const std::string& row) {
    arms_[native].counters[field] = row;
}
void OracleArtifacts::Note(const std::string& key, const std::string& value) { notes_.emplace_back(key, value); }
void OracleArtifacts::Attribute(const Attribution& attribution) { attributions_.push_back(attribution); }

void OracleArtifacts::Finish(const std::string& test, const std::string& schema, int argc, char** argv) {
    if (!Enabled()) return;
    CHECK(arms_[0].input == arms_[1].input && !arms_[0].input.empty());  // both arms consumed the same bytes
    CHECK(!arms_[0].outputs.empty());
    for (const auto& channel : arms_[0].outputs) CHECK(arms_[1].outputs.count(channel.first));
    CHECK(arms_[0].outputs.size() == arms_[1].outputs.size());
    const auto binary = std::filesystem::canonical(argv[0]);
    const auto directory = std::filesystem::absolute(directory_);
    const std::string binary_sha = Digest(Load(binary)), source_sha = Digest(Load(source_manifest_));
    CHECK(!std::filesystem::exists(directory) && std::filesystem::create_directories(directory));
    std::ostringstream command; command << '[' << Quote(binary.string());
    for (int i = 1; i < argc; ++i) command << ',' << Quote(argv[i]);
    command << ']';
    std::ostringstream index;
    index << "{\"schema\":" << Quote(schema) << ",\"test\":" << Quote(test);
    for (const auto& note : notes_) index << ',' << Quote(note.first) << ':' << Quote(note.second);
    // Attributions: exact differing ranges per channel, the native bytes as the expected output.
    index << ",\"attributions\":{";
    bool first_attribution = true;
    for (const auto& attribution : attributions_) {
        const Bytes& model = arms_[0].outputs.at(attribution.channel);
        const Bytes& native = arms_[1].outputs.at(attribution.channel);
        const auto ranges = DifferenceRanges(model, native);
        CHECK(!ranges.empty());
        const auto expected = directory / "attributions";
        std::filesystem::create_directories(expected);
        const auto file = expected / (attribution.channel + ".expected.bin");
        Save(file, native);
        index << (first_attribution ? "" : ",") << Quote(attribution.channel) << ":{\"ranges\":[";
        for (size_t i = 0; i < ranges.size(); ++i)
            index << (i ? "," : "") << '[' << ranges[i].first << ',' << ranges[i].second << ']';
        index << "],\"reason\":" << Quote(attribution.reason)
              << ",\"specification\":{\"file\":{\"path\":" << Quote(attribution.specification_file)
              << ",\"sha256\":" << Quote(Digest(Load(attribution.specification_file))) << "}"
              << ",\"needle\":" << Quote(attribution.specification_needle) << "}"
              << ",\"expected\":{\"path\":" << Quote("attributions/" + attribution.channel + ".expected.bin")
              << ",\"sha256\":" << Quote(Digest(native)) << "}"
              << ",\"pin\":" << Quote(attribution.pin) << "}";
        first_attribution = false;
    }
    index << "},\"arms\":{";
    for (bool native : {false, true}) {
        const Arm& arm = arms_[native];
        const std::string mode = native ? "native" : "model";
        const auto dir = directory / mode;
        CHECK(std::filesystem::create_directory(dir));
        Save(dir / "input.bin", arm.input);
        const std::string input_sha = Digest(arm.input);
        std::map<std::string, std::string> hashes;
        for (const auto& channel : arm.outputs) {
            Save(dir / (channel.first + ".bin"), channel.second);
            hashes[channel.first] = Digest(channel.second);
        }
        std::ostringstream receipt;
        receipt << "{\"schema\":\"retirement-oracle-run-v1\",\"mode\":" << Quote(mode) << ",\"test\":" << Quote(test)
                << ",\"command\":" << command.str() << ",\"binary_sha256\":" << Quote(binary_sha)
                << ",\"source_sha256\":" << Quote(source_sha) << ",\"input_sha256\":" << Quote(input_sha)
                << ",\"exit_code\":0,\"completed\":true,\"settings\":{";
        bool first = true;
        for (const auto& setting : arm.settings) {
            receipt << (first ? "" : ",") << Quote(setting.first) << ':' << Quote(setting.second); first = false;
        }
        receipt << "},\"outputs\":{";
        first = true;
        for (const auto& hash : hashes) {
            receipt << (first ? "" : ",") << Quote(hash.first) << ':' << Quote(hash.second); first = false;
        }
        receipt << "},\"counters\":{";
        first = true;
        for (const auto& counter : arm.counters) {
            receipt << (first ? "" : ",") << Quote(counter.first) << ':' << Quote(counter.second); first = false;
        }
        receipt << "}}\n";
        Save(dir / "receipt.json", receipt.str());
        const auto ref = [&](const std::string& file, const std::string& hash) {
            return "{\"path\":" + Quote(mode + "/" + file) + ",\"sha256\":" + Quote(hash) + "}";
        };
        index << (native ? "," : "") << Quote(mode) << ":{\"input\":" << ref("input.bin", input_sha)
              << ",\"receipt\":" << ref("receipt.json", Digest(Load(dir / "receipt.json"))) << ",\"outputs\":{";
        first = true;
        for (const auto& hash : hashes) {
            index << (first ? "" : ",") << Quote(hash.first) << ':' << ref(hash.first + ".bin", hash.second); first = false;
        }
        index << "}}";
    }
    index << "}}\n";
    Save(directory / "artifacts.json", index.str());
    std::printf("%s artifacts: %s\n", test.c_str(), directory.string().c_str());
}

}  // namespace retirement
