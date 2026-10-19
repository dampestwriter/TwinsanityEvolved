#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"

// Optional evidence from the actual oracle invocation. Images are lossless
// deltas, never PRNG recipes or hashes standing in for consumed bytes.
//
// Two sinks for the same byte streams:
//   --artifacts DIR the receipt: every stream written to DIR (GBs; the dev host).
//   --digest FILE the cross-platform identity lane: the SAME bytes
//                     are streamed into one SHA-256 per channel and FILE receives
//                     `<channel>,<sha256>` lines (the platform_identity.py lane grammar) --
//                     nothing else is written. A build of this tree on another host that
//                     produces the same digests ran the same arithmetic on the same inputs.
class MathLibraryOracleArtifacts {
public:
    struct Case {
        uint32_t entry;
        uint32_t dispatch;
        uint32_t operation;
        uint32_t trial;
        uint32_t step;
        bool selected;
    };

    MathLibraryOracleArtifacts(int argc, char* const* argv, const char* name,
        const char* source_digest, size_t memory_size, size_t scratch_offset,
        uint32_t accepted_control_entry = 0);
    bool Enabled() const { return enabled_; }
    void Fixture(const char* name, const uint8_t* bytes, size_t length);
    void Begin(Case key, const CpuContext& context, const uint8_t* memory);
    // Counter increments immediately before the provided NEW body executes.
    // Called in ordinary tests too; artifact collection remains optional.
    void RunModel(const std::function<void()>& body);
    void ModelOutput(const CpuContext& context, const uint8_t* memory);
    void CheckActualInput(const CpuContext& context, const uint8_t* memory);
    void ActualOutput(const CpuContext& context, const uint8_t* memory, const std::string& census);
    // Only a completely passing oracle calls Finish. I/O failures throw;
    // incomplete/failed runs have no complete.json.
    void Finish();

private:
    using Counts = std::map<std::string, uint64_t>;
    // One output channel: a file (artifacts) or a digesting streambuf (digest mode).
    struct Sink {
        std::string channel;
        std::unique_ptr<std::streambuf> buffer;
        std::unique_ptr<std::ostream> stream;
        std::ostream& out() { return *stream; }
        void close();
        std::string digest() const;   // digest mode only
    };
    struct CounterFile {
        Sink sink;
        Counts previous;
        Counts totals;
        uint64_t records = 0;
    };
    void Open(Sink& sink, const char* channel);
    static void Word(std::ostream& stream, uint32_t value);
    static void Long(std::ostream& stream, uint64_t value);
    static void JsonString(std::ostream& stream, const std::string& value);
    static void WriteCounts(std::ostream& stream, const Counts& values);
    static Counts ParseCensus(const std::string& census);
    static void Delta(std::ostream& stream, const std::vector<uint8_t>& image,
        const std::vector<uint8_t>& base);
    void Image(std::vector<uint8_t>& destination, const CpuContext& context, const uint8_t* memory);
    void WriteOutput(Sink& sink, CounterFile& counters,
        const CpuContext& context, const uint8_t* memory, const Counts& values);
    void WriteCounters(CounterFile& file, const Counts& values);
    static void CloseCounters(CounterFile& file);

    bool enabled_ = false;
    bool digest_mode_ = false;
    bool pending_ = false;
    bool model_ran_ = false;
    bool model_written_ = false;
    bool actual_checked_ = false;
    size_t memory_size_ = 0;
    uint64_t records_ = 0;
    uint64_t cases_ = 0;
    int rounding_ = 0;
    Case key_{};
    std::filesystem::path directory_;   // --artifacts DIR, or the --digest FILE path
    Sink inputs_, model_, selected_, control_;
    CounterFile model_counts_, selected_counts_, control_counts_;
    Counts current_model_counts_;
    Counts fixtures_;
    std::vector<std::pair<std::string, std::string>> fixture_digests_;
    std::vector<uint8_t> input_, previous_input_, output_;
};
